// The generic Android NativeActivity + OpenXR door. See kl_native.h for the
// shape and why it is not one of the engine-specific families. The whole family
// is a thin skin over the shared kl_nativeactivity harness (kl_na_create /
// kl_na_start / kl_na_stop): map the one entry library, run its static
// initializers, hand it ANativeActivity_onCreate, and then turn the looper while
// the guest's own android_main thread runs the game and submits its OpenXR
// frames. GTA Vice City VR (libmiamivr) is the first guest through it.
#include "kl_native.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "klepton.h"
#include "kl_nativeactivity.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_openxr.h"
#include "kl_opensl.h"
#include "kl_xashservers.h"

static char      g_libdir[1200];
static char      g_engine_lib[256];   // e.g. "libmiamivr.so"
static kl_image *g_img;
static char      g_err[256];

const char *kl_native_error(void) { return g_err; }
static int native_fail(const char *m) {
    snprintf(g_err, sizeof g_err, "%s", m);
    return 1;
}

#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

static int kl_dir_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

// Env-driven cvar tuning without re-staging the data tree. When KL_XASH_CVARS is
// set, write its contents as a userconfig.cfg into the Xash game dirs under
// `base`. Xash execs userconfig.cfg AFTER config.cfg, so these win even for
// ARCHIVED cvars (vr_height_adjust, vr_smoothturn, ...) that config.cfg would
// otherwise restore to their saved value. The value is one or more `cvar value`
// assignments separated by ';' or newlines, e.g.
//   KL_XASH_CVARS="vr_smoothturn 1; vr_turn_angle 5; vr_height_adjust -0.20"
// This is the on-device equivalent of hand-editing userconfig.cfg on the host,
// so a knob is retuned by changing an env var between launches — no restage. The
// value is the COMPLETE set the file will hold (it overwrites, it does not merge).
static void kl_xash_write_userconfig(const char *base, FILE *out) {
    const char *cvars = getenv("KL_XASH_CVARS");
    if (!cvars || !*cvars || !base || !*base) return;

    // Body: one assignment per line (';' -> newline), with a provenance header.
    char body[2048];
    int n = snprintf(body, sizeof body, "// Klepton: generated from KL_XASH_CVARS\n");
    for (const char *p = cvars; *p && n < (int)sizeof body - 2; p++)
        body[n++] = (*p == ';') ? '\n' : *p;
    if (n && body[n - 1] != '\n') body[n++] = '\n';
    body[n] = 0;

    // Drop it next to every config.cfg the tree ships: valve is always mounted and
    // is the dir the engine stats; the active -game mod (HL_Gold_HD for hl1,
    // cstrike for cs1) also gets one. A dir that is not present is skipped, not an
    // error, so the same list serves both titles.
    static const char *const rel[] = { "valve", "cstrike", "HL_Gold_HD" };
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char dir[1400];
        snprintf(dir, sizeof dir, "%s/%s", base, rel[i]);
        if (!kl_dir_exists(dir)) continue;
        char path[1500];
        snprintf(path, sizeof path, "%s/userconfig.cfg", dir);
        FILE *f = fopen(path, "w");
        if (!f) { if (out) fprintf(out, "  [xash] userconfig write FAILED: %s\n", path); continue; }
        fwrite(body, 1, (size_t)n, f);
        fclose(f);
        if (out) fprintf(out, "  [xash] wrote %s\n", path);
    }
    if (out) { fprintf(out, "  [xash] KL_XASH_CVARS applied:\n%s", body); fflush(out); }
}

// Surface Xash's own console into our log. The engine's `-log` writes precache
// lists, map spawn, scripted_sequence and AI node-graph messages to a file in the
// tree (engine.log / qconsole.log) — none of which reach our stderr log, so
// gameplay bugs (an NPC that won't path, a missing model) are invisible here.
// We dump it at STARTUP, which reads the PREVIOUS session's log before the engine
// truncates it: re-run once after a bad session and its console lands in our log.
// KL_XASH_NO_ENGINELOG suppresses it if the dump is ever unwanted.
static void kl_xash_dump_enginelog(const char *base, FILE *out) {
    if (!out || !base || !*base) return;
    if (getenv("KL_XASH_NO_ENGINELOG")) return;
    static const char *const rel[] = {
        "engine.log", "valve/qconsole.log", "HLGOLD/HL_Gold_HD/qconsole.log",
        "cstrike/qconsole.log",
    };
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", base, rel[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fprintf(out, "  [xash] ==== previous-session engine console: %s ====\n", rel[i]);
        char line[1024];
        while (fgets(line, sizeof line, f)) fprintf(out, "  [elog] %s", line);
        fprintf(out, "  [xash] ==== end %s ====\n", rel[i]);
        fclose(f);
    }
    fflush(out);
}

// Recursively find the first *.log file under `dir` (depth-limited). Reports each
// candidate it sees; returns 1 and fills `out` on the first hit. Used to locate the
// engine's console log without hard-coding a path the fork may not use.
static int kl_elog_find(const char *dir, int depth, char *out, size_t osz) {
    if (depth < 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int got = 0;
    while (!got && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;              // skip "." ".." and dotfiles
        char p[1600];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            size_t l = strlen(e->d_name);
            if (l >= 4 && !strcmp(e->d_name + l - 4, ".log")) {
                fprintf(stderr, "  [elog-scan] found %s\n", p);
                snprintf(out, osz, "%s", p);
                got = 1;
            }
        } else if (S_ISDIR(st.st_mode)) {
            got = kl_elog_find(p, depth - 1, out, osz);
        }
    }
    closedir(d);
    return got;
}

// Live-tail the engine's -log into stderr as it is written, so the engine console
// (precache / AI node-graph / scripted_sequence) is visible in our streamed log no
// matter how the session ends. A full reinstall wipes the data container, so
// reading a PRIOR session's log is unreliable — tailing the CURRENT one is not.
//
// We do not know the engine's exact log path (the fork may write engine.log under
// the rootdir, a gamedir, or not at all if -log's Sys_InitLog failed), so DISCOVER
// it: enumerate *.log under the tree and tail the first match (preferring
// engine.log). Keeps re-scanning until one appears, since the engine creates it
// only once it starts. Keeps a byte offset across calls; cheap to call every frame.
// KL_XASH_NO_ENGINELOG opts out. Called from the guest pump loop.
void kl_xash_tail_enginelog(void) {
    if (getenv("KL_XASH_NO_ENGINELOG")) return;
    const char *base = getenv("XASH3D_BASEDIR");
    if (!base || !*base) return;
    static char path[1500];
    static long off = 0;
    static int  found = 0, scanned_once = 0;

    if (!found) {
        // Recursively hunt for any *.log the engine may have written (engine.log,
        // qconsole.log, ...), wherever it landed under the tree. First match wins.
        found = kl_elog_find(base, 4, path, sizeof path);
        if (found) { fprintf(stderr, "  [elog-scan] tailing %s\n", path); fflush(stderr); }
        else if (!scanned_once) {
            scanned_once = 1;
            fprintf(stderr, "  [elog-scan] no *.log under %s yet (waiting; -log may have failed)\n", base);
            fflush(stderr);
        }
        if (!found) return;
    }

    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end < off) off = 0;              // truncated / rotated -> re-read from start
        if (end > off) {
            fseek(f, off, SEEK_SET);
            char line[1024];
            while (fgets(line, sizeof line, f)) fprintf(stderr, "  [elog] %s", line);
            off = ftell(f);
            fflush(stderr);
        }
    }
    fclose(f);
}

// hl1's HL_Gold_HD mod ships NESTED at <base>/HLGOLD/HL_Gold_HD, one level below
// the rootdir, where Xash's depth-1 game scan can't see it — so `-game HL_Gold_HD`
// can't MOUNT it and the engine runs with the wrong content search paths (its
// menu/cursor/sound/save/transition state then disagree, and the seamless
// c0a0->c0a0a tram handoff dies with "Can't find connection"). Promote each nested
// mod (a dir two levels down with gameinfo.txt/liblist.gam) UP to the rootdir with
// a rename — same filesystem, so it is instant — making it a first-class depth-1
// game whose gameinfo `basedir "valve"` fallback (a real rootdir sibling) resolves
// normally. Renames are deferred so the directory is not mutated mid-readdir.
static void kl_xash_promote_nested_mods(const char *base, FILE *out) {
    DIR *d = opendir(base);
    if (!d) return;
    char moves[4][2][1400];
    int nmoves = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nmoves < 4) {
        if (e->d_name[0] == '.') continue;
        char subdir[1300];
        snprintf(subdir, sizeof subdir, "%s/%s", base, e->d_name);
        if (!kl_dir_exists(subdir)) continue;
        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *se;
        while ((se = readdir(sd)) && nmoves < 4) {
            if (se->d_name[0] == '.') continue;
            struct stat st;
            char probe[1500];
            snprintf(probe, sizeof probe, "%s/%s/gameinfo.txt", subdir, se->d_name);
            int is_mod = (stat(probe, &st) == 0);
            if (!is_mod) {
                snprintf(probe, sizeof probe, "%s/%s/liblist.gam", subdir, se->d_name);
                is_mod = (stat(probe, &st) == 0);
            }
            if (!is_mod) continue;
            char dest[1400];
            snprintf(dest, sizeof dest, "%s/%s", base, se->d_name);
            if (kl_dir_exists(dest)) continue;              // already at rootdir
            snprintf(moves[nmoves][0], sizeof moves[0][0], "%s/%s", subdir, se->d_name);
            snprintf(moves[nmoves][1], sizeof moves[0][1], "%s", dest);
            nmoves++;
        }
        closedir(sd);
    }
    closedir(d);
    for (int i = 0; i < nmoves; i++) {
        if (rename(moves[i][0], moves[i][1]) == 0) {
            if (out) fprintf(out, "  [xash] promoted nested mod %s -> %s\n", moves[i][0], moves[i][1]);
        } else if (out) {
            fprintf(out, "  [xash] promote %s FAILED: %s\n", moves[i][0], strerror(errno));
        }
    }
}

// Override numeric TBXR launch args (--supersampling, --msaa) in the on-device
// commandline.txt from env, so they can be tuned WITHOUT re-staging that file.
// These are raw launch args, not cvars, so userconfig can't reach them; we replace
// the value token that follows each matched flag, in place. Only touches an arg
// whose env is set; idempotent. e.g. KL_XASH_SUPERSAMPLING=1.0 for perf.
// Where the effective commandline.txt lives. The launcher UI can hand us an edited
// copy in a writable in-app file (KL_XASH_COMMANDLINE_FILE) so the args still take
// effect when the picked game folder is read-only; otherwise it is base/commandline.txt.
// klb_fopen redirects the guest engine's own commandline.txt read to the same file.
static void kl_xash_clpath(const char *base, char *out, size_t cap) {
    const char *clf = getenv("KL_XASH_COMMANDLINE_FILE");
    if (clf && *clf) snprintf(out, cap, "%s", clf);
    else             snprintf(out, cap, "%s/commandline.txt", base);
}

static void kl_xash_rewrite_commandline(const char *base, FILE *out) {
    struct { const char *env, *arg; } ov[] = {
        { "KL_XASH_SUPERSAMPLING", "--supersampling" },
        { "KL_XASH_MSAA",          "--msaa" },
    };
    int any = 0;
    for (size_t i = 0; i < sizeof ov / sizeof *ov; i++) if (getenv(ov[i].env)) any = 1;
    if (!any || !base || !*base) return;

    char path[1300];
    kl_xash_clpath(base, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return;
    line[strcspn(line, "\r\n")] = 0;

    char outbuf[2560];
    int n = 0;
    const char *pending = NULL;                 // token to override the NEXT value with
    char *save = NULL;
    for (char *tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        const char *emit = tok;
        if (pending) { emit = pending; pending = NULL; }
        else {
            for (size_t i = 0; i < sizeof ov / sizeof *ov; i++) {
                const char *v = getenv(ov[i].env);
                if (v && *v && !strcmp(tok, ov[i].arg)) { pending = v; break; }
            }
        }
        n += snprintf(outbuf + n, sizeof outbuf - n, "%s%s", n ? " " : "", emit);
        if (n >= (int)sizeof outbuf - 2) break;
    }
    outbuf[n++] = '\n'; outbuf[n] = 0;
    f = fopen(path, "w");
    if (f) { fwrite(outbuf, 1, (size_t)n, f); fclose(f);
             if (out) fprintf(out, "  [xash] commandline.txt -> %s", outbuf); }
}

// The SourceVR profile a target stands for ("sourcevr-portal" -> "portal"), or
// NULL for any other target.
static const char *kl_sourcevr_profile(void) {
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    return t && strncmp(t, "sourcevr-", 9) == 0 && t[9] ? t + 9 : NULL;
}

static void kl_mkdir_p(const char *path) {
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *c = tmp + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(tmp, 0755); *c = '/'; }
    mkdir(tmp, 0755);
}

// A default is applied once and remembered by a marker file: 1 the first time,
// 0 from then on. The marker is what makes it a default rather than a rule.
static int kl_once(const char *marker) {
    struct stat st;
    if (stat(marker, &st) == 0) return 0;
    FILE *f = fopen(marker, "wb");
    if (f) fclose(f);
    return 1;
}
// --- a reader for the one file we take out of a VPK -------------------------
//
// Valve's VPK: a directory file (<name>_dir.vpk, v1 or v2 header) whose tree is
// extension -> path -> name -> entry {crc u32, preload u16, archive u16, offset
// u32, length u32, 0xffff}, the preload bytes following the entry; the data is
// in <name>_NNN.vpk at the offset, or (archive 0x7fff) in the directory file
// itself after the tree. Enough to find one script; nothing is cached.
static unsigned char *kl_vpk_read(const char *dirvpk, const char *want, size_t *out_len) {
    FILE *f = fopen(dirvpk, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 28 || sz > (64 << 20)) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    uint32_t sig = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    uint32_t ver = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    uint32_t tree = (uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24;
    if (sig != 0x55aa1234u || (ver != 1 && ver != 2)) { free(b); return NULL; }
    size_t i = ver == 1 ? 12 : 28, data0 = i + tree;
    unsigned char *result = NULL;
    // `want` is "path/name.ext"; split it the way the tree is keyed.
    const char *dot = strrchr(want, '.'), *slash = strrchr(want, '/');
    if (!dot || !slash || dot < slash) { free(b); return NULL; }
    char wext[32], wpath[256], wname[128];
    snprintf(wext, sizeof wext, "%s", dot + 1);
    snprintf(wpath, sizeof wpath, "%.*s", (int)(slash - want), want);
    snprintf(wname, sizeof wname, "%.*s", (int)(dot - slash - 1), slash + 1);
    #define KL_VPK_STR(dst) do { size_t n0 = i; while (i < data0 && b[i]) i++; if (i >= data0) goto out; \
                                 snprintf(dst, sizeof dst, "%.*s", (int)(i - n0), (const char *)b + n0); i++; } while (0)
    for (;;) {
        char ext[32]; KL_VPK_STR(ext);
        if (!ext[0]) break;
        for (;;) {
            char path[256]; KL_VPK_STR(path);
            if (!path[0]) break;
            for (;;) {
                char name[128]; KL_VPK_STR(name);
                if (!name[0]) break;
                if (i + 18 > data0) goto out;
                uint16_t pre  = (uint16_t)(b[i + 4] | b[i + 5] << 8);
                uint16_t arch = (uint16_t)(b[i + 6] | b[i + 7] << 8);
                uint32_t off  = (uint32_t)b[i + 8] | (uint32_t)b[i + 9] << 8 | (uint32_t)b[i + 10] << 16 | (uint32_t)b[i + 11] << 24;
                uint32_t len  = (uint32_t)b[i + 12] | (uint32_t)b[i + 13] << 8 | (uint32_t)b[i + 14] << 16 | (uint32_t)b[i + 15] << 24;
                i += 18;
                const unsigned char *preload = b + i;
                i += pre;
                if (strcmp(ext, wext) == 0 && strcmp(name, wname) == 0 &&
                    strcmp(path[0] == ' ' && !path[1] ? "" : path, wpath) == 0) {
                    if ((size_t)pre + len > (16 << 20)) goto out;
                    result = malloc((size_t)pre + len + 1);
                    if (!result) goto out;
                    memcpy(result, preload, pre);
                    if (len) {
                        if (arch == 0x7fff) {
                            if ((size_t)data0 + off + len > (size_t)sz) { free(result); result = NULL; goto out; }
                            memcpy(result + pre, b + data0 + off, len);
                        } else {
                            char apath[1600];
                            const char *d = strstr(dirvpk, "_dir.vpk");
                            if (!d) { free(result); result = NULL; goto out; }
                            snprintf(apath, sizeof apath, "%.*s_%03u.vpk", (int)(d - dirvpk), dirvpk, (unsigned)arch);
                            FILE *a = fopen(apath, "rb");
                            if (!a || fseek(a, (long)off, SEEK_SET) != 0 || fread(result + pre, 1, len, a) != len) {
                                if (a) fclose(a);
                                free(result); result = NULL; goto out;
                            }
                            fclose(a);
                        }
                    }
                    result[pre + len] = 0;
                    *out_len = (size_t)pre + len;
                    goto out;
                }
            }
        }
    }
out:
    #undef KL_VPK_STR
    free(b);
    return result;
}

// The damage flash. The retail HUD animations (HL2's, which Portal and Portal
// 2 share) paint the WHOLE HUD surface orange, then red, when the player takes
// a big hit — `event HudTakeDamageHigh`: HudDamageIndicator DmgFullscreenColor
// "255 88 0 200" -> "255 0 0 200" -> out — and on this port the HUD surface is
// a quad 60 degrees wide in front of the eyes, so the flash is a BOX in the
// middle of the view (Marcel, 2026-11-09: "eine orangene Box, die dann rot
// wird"). The directional bars at the surface's edges (DmgColorLeft/Right,
// DmgHighColor*) are separate and stay.
//
// The retail scripts/hudanimations.txt is read out of the game's own VPK, every
// `Animate HudDamageIndicator DmgFullscreenColor "r g b a"` line gets a = 0,
// and the whole file is written to the write root's scripts/, where a loose
// file beats the VPK's (the menu backdrop takes the same door). A manifest that
// lists a small file of ours before the retail one was the first attempt
// (2026-11-09); whether the controller keeps the first or the last definition
// of an event could not be settled, and a complete file asks no such question.
// Written once (marker): a user who edits the file keeps the edit.
// KL_SOURCEVR_DAMAGE_FLASH=1 leaves the retail file alone.
static void kl_sourcevr_hud_flash_off(const char *root, const char *gamedir, const char *game, FILE *out) {
    if (kl_env_on("KL_SOURCEVR_DAMAGE_FLASH", 0)) return;
    char path[1500], marker[1500];
    snprintf(marker, sizeof marker, "%s/scripts/.avpplay-hudanimations-full", game);
    snprintf(path, sizeof path, "%s/scripts", game);
    kl_mkdir_p(path);
    if (!kl_once(marker)) return;
    // The first attempt's files, should they be there: the full copy makes them pointless.
    snprintf(path, sizeof path, "%s/scripts/hudanimations_manifest.txt", game); unlink(path);
    snprintf(path, sizeof path, "%s/scripts/hudanimations_avpplay.txt", game);  unlink(path);
    // Where the retail script lives: Portal's pak, HL2's misc pak, Portal 2's pak01.
    const char *vpk = strcmp(gamedir, "portal") == 0 ? "common/portal/portal_pak_dir.vpk"
                    : strcmp(gamedir, "hl2") == 0    ? "common/hl2/hl2_misc_dir.vpk"
                    :                                  "common/portal2/pak01_dir.vpk";
    snprintf(path, sizeof path, "%s/%s", root, vpk);
    size_t n = 0;
    unsigned char *text = kl_vpk_read(path, "scripts/hudanimations.txt", &n);
    if (!text) {
        if (out) { fprintf(out, "  [env] SourceVR: scripts/hudanimations.txt not found in %s; the damage flash stays\n", vpk); fflush(out); }
        unlink(marker);          // next start tries again (the data may still be arriving)
        return;
    }
    // Line by line: on a DmgFullscreenColor animate line, the last number inside the quotes becomes 0.
    int patched = 0;
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        if (memmem(text + i, e - i, "DmgFullscreenColor", 18) && memmem(text + i, e - i, "Animate", 7)) {
            unsigned char *q1 = memchr(text + i, '"', e - i);
            unsigned char *q2 = q1 ? memchr(q1 + 1, '"', (size_t)(text + e - q1 - 1)) : NULL;
            if (q1 && q2) {
                // Walk back from the closing quote over the alpha digits and blank them to "0".
                unsigned char *d = q2 - 1;
                while (d > q1 && (*d == ' ' || *d == '\t')) d--;
                unsigned char *dend = d + 1;
                while (d > q1 && *d >= '0' && *d <= '9') d--;
                d++;
                if (dend > d) { *d = '0'; for (unsigned char *z = d + 1; z < dend; z++) *z = ' '; patched++; }
            }
        }
        i = e + 1;
    }
    snprintf(path, sizeof path, "%s/scripts/hudanimations.txt", game);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(text, 1, n, f); fclose(f); }
    free(text);
    if (out) { fprintf(out, "  [env] SourceVR: full-screen damage flash off — scripts/hudanimations.txt (%zu bytes, %d lines patched) from %s into the write root, once\n", n, patched, vpk); fflush(out); }
}

// The game's language, from the start window's choice. SourceVR's launcher
// takes the language from config.cfg's archived `vr_language <steam name>` and
// only without one "follows the device"; a wrong name ("de", typed into its
// console) is rejected with "use an installed Steam language's short name".
// Klepton carries the start window's choice as KL_DEVICE_LANG ("de_DE"); here it
// becomes the Steam name and is written into config.cfg before the launcher
// reads it (every start, so a change in the start window takes effect; the
// game's own language row writes the same cvar). No choice, or English: the
// line is emptied so the launcher follows the device, which kl_jni now tells
// the same language. Voices need the language's VO VPK beside the English one.
static const char *kl_sourcevr_steam_language(const char *tag) {
    if (!tag || !*tag) return NULL;
    char l[8] = {0}, c[8] = {0};
    size_t i = 0;
    for (; tag[i] && tag[i] != '_' && tag[i] != '-' && tag[i] != '.' && i < 7; i++) l[i] = (char)tolower((unsigned char)tag[i]);
    if (tag[i] == '_' || tag[i] == '-') {
        size_t j = 0;
        for (i++; tag[i] && tag[i] != '.' && j < 7; i++) c[j++] = (char)toupper((unsigned char)tag[i]);
    }
    static const struct { const char *l, *c, *steam; } map[] = {
        {"en", "",   "english"},  {"de", "",   "german"},    {"fr", "",   "french"},
        {"es", "MX", "latam"},    {"es", "",   "spanish"},   {"it", "",   "italian"},
        {"pt", "BR", "brazilian"},{"pt", "",   "portuguese"},{"ru", "",   "russian"},
        {"ko", "",   "koreana"},  {"ja", "",   "japanese"},  {"zh", "TW", "tchinese"},
        {"zh", "HK", "tchinese"}, {"zh", "",   "schinese"},  {"pl", "",   "polish"},
        {"nl", "",   "dutch"},    {"sv", "",   "swedish"},   {"da", "",   "danish"},
        {"fi", "",   "finnish"},  {"no", "",   "norwegian"}, {"nb", "",   "norwegian"},
        {"cs", "",   "czech"},    {"hu", "",   "hungarian"}, {"tr", "",   "turkish"},
        {"th", "",   "thai"},     {"el", "",   "greek"},     {"bg", "",   "bulgarian"},
        {"ro", "",   "romanian"}, {"uk", "",   "ukrainian"}, {"vi", "",   "vietnamese"},
        {"id", "",   "indonesian"},
    };
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++)
        if (strcmp(map[k].l, l) == 0 && (!*map[k].c || strcmp(map[k].c, c) == 0)) return map[k].steam;
    return NULL;
}

static void kl_sourcevr_language(const char *game, FILE *out) {
    // The start window's "English" only switches the device language off
    // (KL_LOCALE_DEVICE=0, KleptonLanguage.apply) and leaves KL_DEVICE_LANG
    // holding the headset's own, so on a German headset Portal stayed German
    // with English chosen (headset log 2026-11-10: "KL_LOCALE_DEVICE=0,
    // KL_DEVICE_LANG=de_DE" -> vr_language 'german'). Off means English here too.
    const char *tag = getenv("KL_DEVICE_LANG");
    const char *from_device = getenv("KL_LOCALE_DEVICE");
    if (from_device && strcmp(from_device, "0") == 0) tag = "en";
    const char *steam = kl_sourcevr_steam_language(tag);
    const char *want = steam && strcmp(steam, "english") != 0 ? steam : "";
    char path[1500];
    snprintf(path, sizeof path, "%s/cfg/config.cfg", game);
    FILE *f = fopen(path, "rb");
    if (!f) return;               // no config yet: the wizard has not run; next start
    static char text[65536];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    if (n >= sizeof text - 1) return;
    text[n] = 0;
    char line[64];
    snprintf(line, sizeof line, "vr_language \"%s\"", want);
    char *hit = strstr(text, "vr_language \"");
    if (hit) {
        char *end = strchr(hit + 13, '"');
        if (!end) return;
        if (strncmp(hit, line, strlen(line)) == 0 && end + 1 == hit + strlen(line)) {   // already so
            if (out) { fprintf(out, "  [env] SourceVR: vr_language \"%s\" kept (start window: %s)\n", want, tag ? tag : "(unset)"); fflush(out); }
            return;
        }
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(text, 1, (size_t)(hit - text), f);
            fputs(line, f);
            fputs(end + 1, f);
            fclose(f);
        }
    } else {
        if ((f = fopen(path, "ab")) != NULL) { fprintf(f, "%s\n", line); fclose(f); }
    }
    if (out) { fprintf(out, "  [env] SourceVR: vr_language \"%s\" (start window: %s)\n", want, tag ? tag : "(unset)"); fflush(out); }
}

static void kl_sourcevr_portal_defaults(const char *game, int backdrop, FILE *out) {
    char path[1500], marker[1500];
    // The menu backdrop (Portal only: background2 is a Portal map).
    snprintf(marker, sizeof marker, "%s/scripts/.avpplay-chapterbackgrounds", game);
    snprintf(path, sizeof path, "%s/scripts", game);
    kl_mkdir_p(path);
    if (backdrop && kl_once(marker)) {
        snprintf(path, sizeof path, "%s/scripts/chapterbackgrounds.txt", game);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs("\"chapters\"\n{\n", f);
            for (int c = 1; c <= 13; c++) fprintf(f, "\t%d\t\"background2\"\n", c);
            fputs("}\n", f);
            fclose(f);
            if (out) { fprintf(out, "  [env] Portal: menu backdrop set to background2 (chapterbackgrounds.txt, once)\n"); fflush(out); }
        }
    }
    // The recursion depth, replaced only where the wizard's "2" stands.
    snprintf(marker, sizeof marker, "%s/cfg/.avpplay-portal-depth", game);
    snprintf(path, sizeof path, "%s/cfg/config.cfg", game);
    FILE *f = fopen(path, "rb");
    if (!f) return;               // no config yet: the wizard has not run; next start
    static char text[65536];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    static const char was[] = "r_portal_stencil_depth \"2\"";
    char *hit = n < sizeof text - 1 ? strstr(text, was) : NULL;
    if (!hit) return;
    if (!kl_once(marker)) return;
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite(text, 1, (size_t)(hit - text), f);
        fputs("r_portal_stencil_depth \"1\"", f);
        fputs(hit + sizeof was - 1, f);
        fclose(f);
        if (out) { fprintf(out, "  [env] Portal: r_portal_stencil_depth 2 -> 1 (once)\n"); fflush(out); }
    }
}

// The weapon angle. `hlvr_controller_pointer_angle_offset` ("Weapon angle" in
// the game's VR settings) is 20 by default under Klepton, not the port's 5: the
// guest patch sourcevr-hl2-weapon-angle-20 (kl_guestpatch.c) moves the ConVar's
// default and the middle of its slider. A config written before that still
// archives the old default, and would now read as the slider's +15 end; it is
// moved to 20 once (marker), so a player who sets 5 afterwards keeps it. Tied
// to the patch: KL_GUEST_PATCH=0 or its name in KL_GUEST_PATCH_OFF leaves the
// config alone as well.
static void kl_sourcevr_weapon_angle(const char *game, FILE *out) {
    static const char name[] = "hlvr_controller_pointer_angle_offset";
    if (!kl_env_on("KL_GUEST_PATCH", 1)) return;
    const char *off = getenv("KL_GUEST_PATCH_OFF");
    if (off && strstr(off, "sourcevr-hl2-weapon-angle-20")) return;
    char cfg[1500], marker[1500];
    snprintf(cfg, sizeof cfg, "%s/cfg/config.cfg", game);
    snprintf(marker, sizeof marker, "%s/cfg/.avpplay-weapon-angle", game);
    FILE *f = fopen(cfg, "rb");
    if (!f) return;               // no config yet: the ConVar's own default applies
    static char text[65536];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    if (n >= sizeof text - 1) return;
    text[n] = 0;
    char *hit = strstr(text, name);
    if (!hit) return;
    char *q1 = strchr(hit, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2 || q1 - hit > (long)sizeof name + 2) return;
    if (strtod(q1 + 1, NULL) != 5.0) return;          // not the old default: somebody chose it
    if (!kl_once(marker)) return;
    if ((f = fopen(cfg, "wb")) != NULL) {
        fwrite(text, 1, (size_t)(q1 + 1 - text), f);
        fputs("20", f);
        fputs(q2, f);
        fclose(f);
        if (out) { fprintf(out, "  [env] SourceVR: weapon angle 5 -> 20 in the config (once; the old default)\n"); fflush(out); }
    }
}

void kl_engine_setenv(const char *libdir, const char *entry_lib, FILE *out) {
    const char *files = kl_jni_files_dir();
    if (!files) files = "";
    if (entry_lib && strstr(entry_lib, "xash")) {
        // Xash3D (cs1/hl1). The data tree stages under <files>/xash (valve,
        // cstrike, HLGOLD). Engine ref/menu/client libs are the translated ones
        // in libdir. The game picks its mod from XASH3D_GAMEDIR — cstrike if the
        // tree has it (Counter-Strike), else valve (Half-Life). KL_XASH_* override.
        //
        // The engine prints its whole console (map spawn, precache, AI node-graph,
        // scripted_sequence) to STDOUT (fd 1) via printf/puts. Our streamed log is
        // STDERR (fd 2), so none of it was visible. Point fd 1 at fd 2 and unbuffer
        // stdout so the engine console lands in our log, live. This is the runtime
        // doing the redirect (not the guest, whose own dup2 onto fd 1/2 we decline
        // in kl_libc_slink.c); KL_XASH_NO_ENGINELOG opts out.
        if (!getenv("KL_XASH_NO_ENGINELOG")) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
            setvbuf(stdout, NULL, _IONBF, 0);
        }
        char base[1200], sub[1300];
        // The picked-folder override (launcher file picker) IS the game root, so
        // promotion, commandline.txt and -game all resolve against it; only the
        // unset (in-app staged) case falls back to <files>/xash.
        const char *bov = getenv("KL_XASH_BASEDIR");
        if (bov && *bov) snprintf(base, sizeof base, "%s", bov);
        else             snprintf(base, sizeof base, "%s/xash", files);
        // hl1's real mod (HL_Gold_HD) ships NESTED at HLGOLD/HL_Gold_HD, where the
        // engine's depth-1 game scan can't mount it. Promote it to the rootdir so
        // `-game HL_Gold_HD` resolves normally, with valve as its basedir fallback.
        kl_xash_promote_nested_mods(base, out);
        // Apply any env overrides to the raw launch args (KL_XASH_SUPERSAMPLING /
        // KL_XASH_MSAA) by rewriting commandline.txt on device — no restage needed.
        kl_xash_rewrite_commandline(base, out);
        const char *gamedir = getenv("KL_XASH_GAMEDIR");
        if (!gamedir) {
            // Run the mod the launch asks for: commandline.txt's `-game <mod>`
            // (hl1 = HL_Gold_HD). With the mod promoted above, the engine mounts it
            // AND valve (its gameinfo basedir) as the fallback, so content, menu,
            // sound, saves and the seamless c0a0->c0a0a tram transitions are all
            // coherent. Forcing valve here ran the base game with the wrong assets
            // and a split-brain state that crashed the tram's level transition.
            static char gbuf[128];
            char clpath[1300];
            kl_xash_clpath(base, clpath, sizeof clpath);
            FILE *cl = fopen(clpath, "r");
            if (cl) {
                char line[1024];
                if (fgets(line, sizeof line, cl)) {
                    char *g = strstr(line, "-game ");
                    if (g) {
                        g += 6;
                        int i = 0;
                        while (g[i] && g[i] != ' ' && g[i] != '\t' &&
                               g[i] != '\n' && g[i] != '\r' && i < (int)sizeof gbuf - 1)
                            gbuf[i] = g[i], i++;
                        gbuf[i] = 0;
                        if (gbuf[0]) gamedir = gbuf;
                    }
                }
                fclose(cl);
            }
        }
        if (!gamedir) {
            snprintf(sub, sizeof sub, "%s/cstrike", base);
            gamedir = kl_dir_exists(sub) ? "cstrike" : "valve";
        }
        setenv("XASH3D_BASEDIR",    getenv("KL_XASH_BASEDIR") ? getenv("KL_XASH_BASEDIR") : base, 1);
        // NOT XASH3D_RODIR: our data tree is a single writable directory, and
        // Xash aborts ("RoDir and default rootdir can't point to same directory")
        // when RODIR is set and equals the rootdir. Leaving it unset makes Xash
        // use BASEDIR as the one writable root, which is what we have.
        setenv("HOME",              base, 1);
        setenv("XASH3D_GAMEDIR",    gamedir, 1);
        setenv("XASH3D_ENGLIBDIR",  libdir, 1);
        setenv("XASH3D_GAMELIBDIR", libdir, 1);
        // The player's own servers (favourites, admin access, the menus that use it) – before the
        // userconfig is written, because the admin access adds a setting to it. See kl_xashservers.c.
        extern const char *kl_jni_assets_dir(void);
        kl_xash_servers_prepare(base, gamedir, kl_jni_assets_dir(), out);
        // Env-tunable cvars → userconfig.cfg, so knobs like vr_height_adjust can be
        // retuned between launches with no restage (see kl_xash_write_userconfig).
        kl_xash_write_userconfig(base, out);
        // Surface the previous session's engine console (precache/AI/scripted) into
        // our log, before the engine's -log truncates it (see kl_xash_dump_enginelog).
        kl_xash_dump_enginelog(base, out);
        // The VR fork reads getenv("xr_manufacturer") to pick a controller /
        // interaction profile and strcmp()s it against "PICO" / "PLAY FOR DREAM"
        // — a NULL (the Java VR launcher normally setenv's it from
        // Build.MANUFACTURER) segfaults in Host_Main. Klepton's OpenXR advertises
        // /interaction_profiles/oculus/touch_controller, so hand it an Oculus-class
        // identity: that takes the default (non-PICO) touch-controller branch.
        setenv("xr_manufacturer", getenv("KL_XR_MANUFACTURER") ? getenv("KL_XR_MANUFACTURER") : "Oculus", 1);
        // Team Beef's lambda1vr fork (hl1) reads a DIFFERENT var: OPENXR_HMD,
        // which it stores verbatim (getenv, no copy) and strstr()s for "meta" /
        // "pico" to pick the interaction profile — a NULL (unset) segfaults in
        // TBXR_InitialiseOpenXR. Give it a Meta-Quest identity so it matches the
        // "meta" branch, i.e. Klepton's /interaction_profiles/oculus/touch_controller.
        setenv("OPENXR_HMD", getenv("KL_OPENXR_HMD") ? getenv("KL_OPENXR_HMD") : "meta_quest", 1);
        if (out) { fprintf(out, "  [env] XASH3D_BASEDIR=%s GAMEDIR=%s ENGLIBDIR=%s xr_manufacturer=%s OPENXR_HMD=%s\n",
                           base, gamedir, libdir, getenv("xr_manufacturer"), getenv("OPENXR_HMD")); fflush(out); }
    } else if (entry_lib && strstr(entry_lib, "launcher") && kl_sourcevr_profile()) {
        // SourceVR (targets "sourcevr-<profile>"; see visionos/targets.py). Its
        // liblauncher builds the engine's command line itself, from what its
        // Java launcher puts in the environment (MainActivity.setupEngineEnvironment):
        //
        //   APP_LIB_PATH                  where the engine's libraries are
        //   SOURCEVR_SHARED_CONTENT_PATH  /sdcard/SourceVRPort/common — the Steam files
        //   VALVE_GAME_PATH               /sdcard/SourceVRPort/user   — the write root
        //   SOURCEVR_WRITE_GAME_PATH      <user>/<game directory>
        //   SOURCEVR_GAME                 the game directory (portal, episodic …)
        //   EXTRAS_VPK_PATH               the staged overlay folders, comma-separated
        //   VR_CONTENT_PATH               the staged VR layers, comma-separated
        //
        // Everything lives under <files>/SourceVRPort, which is also what the
        // guest's hard-wired /sdcard/SourceVRPort resolves to. The staged folders
        // are laid out on the Mac by tools/sourcevr_prepare.py. A folder that is
        // not there is left out of its list, as the launcher does.
        const char *profile = kl_sourcevr_profile();
        if (strcmp(profile, "portal") == 0) {
            // The loading presenter holds the XR lock across its paced frame,
            // then immediately takes it again. Let the waiting engine thread
            // acquire it on unlock instead of repeatedly losing to that loop.
            // Keep explicit launch overrides for comparison.
            setenv("KL_MUTEX_POLICY", "fairshare", 0);
        }
        static const struct { const char *profile, *gamedir; const char *layers[5]; } P[] = {
            { "hl2",         "hl2",         { "hl2vr", "hl2bodyvr", "hl2bodyhla" } },
            { "ep1",         "episodic",    { "ep1vr", "hl2vr", "hl2bodyvr", "hl2bodyhla" } },
            { "ep2",         "ep2",         { "ep2vr", "hl2vr", "hl2bodyvr", "hl2bodyhla" } },
            { "lostcoast",   "lostcoast",   { "hl2vr", "hl2bodyvr", "hl2bodyhla" } },
            { "portal",      "portal",      { "portalvr" } },
            { "portal2",     "portal2",     { "portal2vr" } },
            { "entropyzero", "EntropyZero", { "hl2vr", "hl2bodyvr", "hl2bodyhla" } },
        };
        const char *gamedir = profile;
        const char *const *layers = NULL;
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
            if (strcmp(P[i].profile, profile) == 0) { gamedir = P[i].gamedir; layers = P[i].layers; }
        const char *gov = getenv("KL_SOURCEVR_GAME");
        if (gov && *gov) gamedir = gov;

        char root[1100], p[1400], list[4200];
        snprintf(root, sizeof root, "%s/SourceVRPort", files);
        // Absolute: the launcher dlopens "<APP_LIB_PATH>/filesystem_stdio.so" and
        // the loader, handed a relative path, looks for "lib<path>" — on the Mac
        // host, where libdir is "sourcevr-portal/lib/arm64-v8a", that is
        // "libsourcevr-portal/lib/arm64-v8a/filesystem_stdio.so: not found".
        static char libabs[1200];
        if (libdir && libdir[0] != '/' && realpath(libdir, libabs)) libdir = libabs;
        setenv("APP_LIB_PATH",    libdir, 1);
        setenv("APP_MOD_LIB",     libdir, 1);
        setenv("BASE_PATH",       libdir, 1);
        setenv("EXECUTABLE_PATH", libdir, 1);
        snprintf(p, sizeof p, "%s/common", root);
        setenv("SOURCEVR_SHARED_CONTENT_PATH", p, 1);
        snprintf(p, sizeof p, "%s/user", root);
        kl_mkdir_p(p);
        setenv("VALVE_GAME_PATH", p, 1);
        // The write game directory, <root>/user/<gamedir>: kept in its own
        // buffer because `p` is reused for the staged folders below, and the
        // three per-game edits after that (the Portal defaults, the
        // vr_eye_resolution rewrite, the gameinfo.txt seed) belong HERE, not in
        // the overlay — with `p` they landed in runtime/v1/overlay/<profile>,
        // which worked on the headset only because the overlay is a search
        // path too, and left the Mac host run without a gameinfo.txt in the
        // write root ("Setup file 'gameinfo.txt' doesn't exist in subdirectory
        // 'hl2'", 2026-11-06).
        char wr[1400];
        snprintf(wr, sizeof wr, "%s/user/%s", root, gamedir);
        kl_mkdir_p(wr);
        setenv("SOURCEVR_WRITE_GAME_PATH", wr, 1);
        // Where the staged folders (overlay, vr layers) are: <root>/runtime/v1
        // when tools/sourcevr_prepare.py laid them out into the data folder on
        // the Mac, else the app's assets — AVP Play unpacks the APK into the
        // target's tree and run.sh has the script lay the folders out into
        // <tree>/assets/sourcevr-runtime/v1, which the app copies to the
        // headset with the rest of the assets. Read in place: nothing in them
        // is written to.
        char rt[1300];
        snprintf(rt, sizeof rt, "%s/runtime/v1", root);
        {
            extern const char *kl_jni_assets_dir(void);
            const char *assets = kl_jni_assets_dir();
            snprintf(p, sizeof p, "%s/overlay/%s", rt, profile);
            if (!kl_dir_exists(p) && assets && *assets) {
                snprintf(p, sizeof p, "%s/sourcevr-runtime/v1", assets);
                if (kl_dir_exists(p)) snprintf(rt, sizeof rt, "%s", p);
            }
            if (out) { fprintf(out, "  [env] SourceVR staged folders: %s\n", rt); fflush(out); }
        }
        // Two defaults AVP Play ships for Portal (his word, 2026-11-01), each
        // applied ONCE and remembered by a marker beside it, so a player who
        // changes them afterwards keeps the change:
        //   - the menu backdrop: the retail background map carries a camera
        //     flight that made him dizzy; `scripts/chapterbackgrounds.txt` in
        //     the write root (a search path) points every chapter at
        //     background2, whose camera stands still.
        //   - `r_portal_stencil_depth "1"` instead of the "2" the first-run
        //     wizard writes: the second recursion is what dropped him to 34 fps
        //     standing in a portal (GPU 99 %).
        if (strcmp(gamedir, "portal") == 0 || strcmp(gamedir, "hl2") == 0 ||
            strcmp(gamedir, "portal2") == 0) {
            kl_sourcevr_hud_flash_off(root, gamedir, wr, out);
            kl_sourcevr_language(wr, out);
        }
        // Portal 2 takes the recursion default too: its headset log of
        // 2026-11-10 shows each portal view drawn into its own 3840x3081
        // target and the game GPU-bound at 17-37 ms a frame with the wizard's
        // depth of 2.
        if (strcmp(gamedir, "hl2") == 0) kl_sourcevr_weapon_angle(wr, out);
        if (strcmp(gamedir, "portal") == 0)  kl_sourcevr_portal_defaults(wr, 1, out);
        if (strcmp(gamedir, "portal2") == 0) kl_sourcevr_portal_defaults(wr, 0, out);
        // The picture size. The engine writes vr_eye_resolution "2064x2208" — a
        // Quest 3's own panel — into its config at its first exit and from then
        // on starts at that size whatever this headset recommends (the first
        // run, with no config yet, took 88 % of the recommendation). "auto"
        // keeps the size following the runtime, which is where the app's render
        // resolution setting acts. Only that one default is replaced, so another
        // size picked in the game's own settings stays; KL_SOURCEVR_KEEP_RES=1
        // leaves the file alone altogether.
        if (!kl_env_on("KL_SOURCEVR_KEEP_RES", 0)) {
            char cfg[1500];
            snprintf(cfg, sizeof cfg, "%s/cfg/config.cfg", wr);
            FILE *f = fopen(cfg, "rb");
            if (f) {
                static char text[65536];
                size_t n = fread(text, 1, sizeof text - 1, f);
                fclose(f);
                text[n] = 0;
                static const char was[] = "vr_eye_resolution \"2064x2208\"";
                char *hit = n < sizeof text - 1 ? strstr(text, was) : NULL;
                if (hit && (f = fopen(cfg, "wb")) != NULL) {
                    fwrite(text, 1, (size_t)(hit - text), f);
                    fputs("vr_eye_resolution \"auto\"", f);
                    fputs(hit + sizeof was - 1, f);
                    fclose(f);
                    if (out) { fprintf(out, "  [env] config.cfg: vr_eye_resolution 2064x2208 -> auto\n"); fflush(out); }
                }
            }
        }
        // The engine is started in the write root and looks for <game>/gameinfo.txt
        // THERE; the launcher seeds it from the Steam files the first time
        // (SharedContentMigration.ensurePrivateGameInfo). Without it the engine
        // stops with "Setup file 'gameinfo.txt' doesn't exist in subdirectory".
        {
            char from[1500], to[1500];
            snprintf(from, sizeof from, "%s/common/%s/gameinfo.txt", root, gamedir);
            snprintf(to, sizeof to, "%s/gameinfo.txt", wr);
            struct stat gs;
            if (stat(to, &gs) != 0 || !S_ISREG(gs.st_mode)) {
                FILE *in = fopen(from, "rb"), *o = in ? fopen(to, "wb") : NULL;
                if (in && o) {
                    char buf[8192]; size_t n;
                    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, o);
                }
                if (out) { fprintf(out, "  [env] gameinfo.txt %s the write root from %s\n",
                                   in && o ? "copied into" : "could NOT be copied into", from); fflush(out); }
                if (in) fclose(in);
                if (o) fclose(o);
            }
        }
        setenv("SOURCEVR_GAME", gamedir, 1);
        if (!getenv("LANG")) setenv("LANG", "en_US", 1);

        // Overlay: the profile's folder, and its anniversary/ subfolder where
        // the launcher adds it (shaders for the 20th Anniversary game files).
        list[0] = 0;
        snprintf(p, sizeof p, "%s/overlay/%s", rt, profile);
        if (kl_dir_exists(p)) snprintf(list, sizeof list, "%s", p);
        if (kl_env_on("KL_SOURCEVR_ANNIVERSARY", 1)) {
            snprintf(p, sizeof p, "%s/overlay/%s/anniversary", rt, profile);
            if (kl_dir_exists(p) && strlen(list) + strlen(p) + 2 < sizeof list) {
                if (list[0]) strcat(list, ",");
                strcat(list, p);
            }
        }
        setenv("EXTRAS_VPK_PATH", list, 1);
        if (out) { fprintf(out, "  [env] EXTRAS_VPK_PATH=%s\n", list); fflush(out); }

        list[0] = 0;
        for (int i = 0; layers && i < 5 && layers[i]; i++) {
            snprintf(p, sizeof p, "%s/vr/%s", rt, layers[i]);
            if (!kl_dir_exists(p) || strlen(list) + strlen(p) + 2 >= sizeof list) continue;
            if (list[0]) strcat(list, ",");
            strcat(list, p);
        }
        setenv("VR_CONTENT_PATH", list, 1);
        if (out) { fprintf(out, "  [env] SourceVR profile %s: game %s, APP_LIB_PATH=%s, root %s\n"
                                "  [env] VR_CONTENT_PATH=%s\n", profile, gamedir, libdir, root, list); fflush(out); }
    } else if (entry_lib && strstr(entry_lib, "launcher")) {
        // Source (hl2/portal). The launcher (FileSystem_GetExecutableDir) builds
        // "<APP_LIB_PATH>/filesystem_stdio.so" and dlopens the engine/tier0/mod
        // modules from there — a NULL APP_LIB_PATH is the "(null)/filesystem_stdio.so"
        // abort. All the engine .so live in the guest lib dir, so APP_LIB_PATH,
        // APP_MOD_LIB and BASE_PATH all point there. (EXECUTABLE_PATH is not what
        // this port reads; keep it set anyway — it is harmless and some builds do.)
        setenv("APP_LIB_PATH",    libdir, 1);
        setenv("APP_MOD_LIB",     libdir, 1);
        setenv("BASE_PATH",       libdir, 1);
        setenv("EXECUTABLE_PATH", libdir, 1);

        // APP_DATA_PATH is the game content root: the tree stages under
        // <files>/srceng (HL2, valve variant) or <files>/Source (Portal). The
        // game folder path and which game to run come off the same root: the
        // Portal launcher reads PORTAL_GAME_PATH + SOURCE_GAME (portal/portal2),
        // the HL2 launcher reads VALVE_GAME_PATH. Set them all — each launcher
        // ignores the ones it does not use. KL_SOURCE_GAME overrides the pick.
        char dp[1300];
        int is_portal;
        // KL_SOURCE_DATA_PATH points the content root straight at a folder the user
        // supplied (the launcher's "srceng"/"Source" tree), instead of the staged
        // <files>/srceng | /Source — so the picker can hand us the game folder
        // itself. Which game to run then comes from the target, not dir-sniffing.
        const char *dpov = getenv("KL_SOURCE_DATA_PATH");
        if (dpov && *dpov) {
            snprintf(dp, sizeof dp, "%s", dpov);
            extern const char *kl_driver_target_name(void);
            const char *tn = kl_driver_target_name();
            is_portal = tn && !strcmp(tn, "portal");
        } else {
            snprintf(dp, sizeof dp, "%s/srceng", files);
            is_portal = !kl_dir_exists(dp);
            if (is_portal) snprintf(dp, sizeof dp, "%s/Source", files);
        }
        setenv("APP_DATA_PATH",    dp, 1);
        setenv("PORTAL_GAME_PATH", dp, 1);
        setenv("VALVE_GAME_PATH",  dp, 1);
        const char *game = getenv("KL_SOURCE_GAME");
        if (!game) game = is_portal ? "portal" : "hl2";
        setenv("SOURCE_GAME", game, 1);
        if (out) { fprintf(out, "  [env] APP_LIB_PATH=%s APP_DATA_PATH=%s SOURCE_GAME=%s\n",
                           libdir, dp, game); fflush(out); }

        // HL2Q3VR ships per-render-category single-pass DIAGNOSTIC env vars it reads
        // via getenv (klb_getenv -> host getenv, same path SOURCE_GAME/HL2QUEST_GAME
        // take). Forward KL_HL2_DIAG_* so we can isolate which category a still-missing
        // surface belongs to (e.g. the invisible train-car shell: walls/floor/ceiling
        // gone while the seats render) without cheats or cfg. Only set when present.
        static const struct { const char *kl, *port; } diag[] = {
            { "KL_HL2_DIAG_OPAQUE",      "HL2QUEST_DIAG_SINGLE_PASS_OPAQUE" },
            { "KL_HL2_DIAG_BRUSH",       "HL2QUEST_DIAG_SINGLE_PASS_BRUSHMODELS" },
            { "KL_HL2_DIAG_ENTITIES",    "HL2QUEST_DIAG_SINGLE_PASS_ENTITIES" },
            { "KL_HL2_DIAG_STATICPROPS", "HL2QUEST_DIAG_SINGLE_PASS_STATICPROPS" },
        };
        for (size_t i = 0; i < sizeof diag / sizeof diag[0]; i++) {
            const char *v = getenv(diag[i].kl);
            if (v && *v) {
                setenv(diag[i].port, v, 1);
                if (out) { fprintf(out, "  [env] %s=%s\n", diag[i].port, v); fflush(out); }
            }
        }
    }
}

int kl_native_configure(const char *libdir, const char *entry_lib, FILE *out) {
    if (!libdir || !*libdir)       return native_fail("no library directory");
    if (!entry_lib || !*entry_lib) return native_fail("no entry library");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    snprintf(g_engine_lib, sizeof g_engine_lib, "%s.so", entry_lib);
    // SourceVR comes through this door (see visionos/targets.py) and needs what
    // its Java launcher would have put in the environment before the library
    // loads; the SDL doors call this from their own configure.
    if (kl_sourcevr_profile()) kl_engine_setenv(libdir, entry_lib, out);
    return 0;
}

int kl_native_load(FILE *out) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", g_libdir, g_engine_lib);
    if (out) { fprintf(out, "=== %s entry ===\n", g_engine_lib); fflush(out); }

    // Load with the DT_NEEDED chain, dependencies FIRST, then run the target's
    // init. libmiamivr NEEDS libc++_shared.so (it imports operator new / _Znwm,
    // delete, the C++ runtime), and that dependency must be mapped, wired and
    // initialised BEFORE libmiamivr's own static initializers run — otherwise the
    // first allocation in them hits an unresolved _Znwm and aborts in
    // kl_unresolved_named. kl_load_auto loads only the one file and would leave
    // exactly that gap; kl_load_recursive walks DT_NEEDED (skipping shim-served
    // names like libopenxr_loader/libandroid/libc, which bind through the shims)
    // and register+inits each, deps first, then the target.
    //
    // Tea For God (x6323807174332876) lists libfmod/libfmodstudio in its
    // DT_NEEDED BEFORE libc++_shared, and libfmodstudio's static initializers
    // call __cxa_guard_acquire — which Android's libstdc++.so would serve, but
    // which here only libc++_shared exports. Walking DT_NEEDED in order binds
    // FMOD first and its init aborts in kl_unresolved_named. Pull libc++_shared
    // in up front, the way UE4_CHAIN does, so FMOD binds against it.
    {
        extern const char *kl_driver_target_name(void);
        const char *tn = kl_driver_target_name();
        if (tn && strcmp(tn, "x6323807174332876") == 0) {
            char cxx[1400];
            snprintf(cxx, sizeof cxx, "%s/libc++_shared.so", g_libdir);
            if (kl_can_load(cxx) && !kl_load_recursive(cxx) && out) {
                fprintf(out, "  libc++_shared.so preload failed: %s\n", kl_error());
                fflush(out);
            }
        }
    }
    kl_image *img = kl_load_recursive(path);
    if (!img) { snprintf(g_err, sizeof g_err, "%s: %s", g_engine_lib, kl_error()); return 1; }
    g_img = img;
    if (out) {
        // In the shape tools/symbolize_sample.py parses: this guest owns its own
        // render thread, so `sample <pid>` is the instrument for "what is it
        // doing", and without the load address every guest frame reads as unknown.
        fprintf(out, "  mapped %-22s @%p %7.2f MB\n", g_engine_lib,
                kl_base(img), kl_span(img) / 1048576.0);
        kl_dl_report_images(out);
        fflush(out);
    }

    // JNI_OnLoad if the library has one. A NativeActivity game usually does not —
    // its entry points are the ANativeActivity_onCreate / android_main exports —
    // so this is reported, not required.
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
    if (onload) {
        kl_jni_local_frame_push();
        int v = onload(kl_jni_vm(), NULL);
        kl_jni_local_frame_pop();
        if (out) { fprintf(out, "  JNI_OnLoad returned 0x%08x\n", v); fflush(out); }
    } else if (out) {
        fprintf(out, "  no JNI_OnLoad — ANativeActivity_onCreate is the entry\n");
        fflush(out);
    }
    return 0;
}

unsigned kl_native_gap(FILE *out) {
    if (!g_img) return 0;
    unsigned n = 0;
    const char *const *miss = kl_missing_imports(g_img, &n);
    if (out) {
        fprintf(out, "  %u unresolved import(s)%s\n", n, n ? ":" : "");
        for (unsigned i = 0; i < n && i < 40; i++) fprintf(out, "    %s\n", miss[i]);
        if (n > 40) fprintf(out, "    ... and %u more\n", n - 40);
        fflush(out);
    }
    return n;
}

int kl_native_create(FILE *out) {
    // android_native_app_glue spawns android_main on its own thread from inside
    // onCreate; the game loop and every OpenXR frame run there. We only stand the
    // activity up — kl_native_start then delivers the window the render thread
    // has been blocking for.
    //
    // internalDataPath is getFilesDir() == <dataDir>/files on Android, and a
    // NativeActivity game builds paths straight off it: GTA Vice City VR reads
    // <internalDataPath>/gamedata. It must resolve to where stage_files="files"
    // staged the tree (<dataDir>/files/gamedata) — but the real container path is
    // ~90 chars and the game's casepath sizes fixed 128-byte path buffers for a
    // Quest's short storage, so handing it the full path overflows and FORTIFY
    // aborts. Use the SHORT /sdcard spelling instead: kl_guest_path maps /sdcard ->
    // <dataDir> (kl_guest_extstorage_map), so /sdcard/files == getFilesDir ==
    // <dataDir>/files, and klb_getcwd hands the same short spelling back after the
    // game chdir's into it. Every path the game builds then stays well under 128.
    return kl_na_create(g_img, "ANativeActivity_onCreate", "/sdcard/files", out);
}

void kl_native_start(FILE *out) { kl_na_start(out); }
void kl_native_stop(FILE *out)  { kl_na_stop(out); }

double kl_native_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        // The UI thread's task queue and the frame clock, on the same thread that
        // turns the looper — the only definition of "UI thread" that matters here.
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        kl_jni_tick_choreographer();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_native_report(FILE *out) {
    if (!out) return;
    // Who is blocked on what, first: this guest owns its render thread, so no
    // return value anywhere says "the engine stopped", and a run that drew nothing
    // looks like one that drew until the mutex-owner map is read.
    kl_pthread_report(out);
    kl_egl_report(out);
    kl_openxr_report(out);
    kl_opensl_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
