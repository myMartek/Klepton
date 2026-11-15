// The door of a Mach-O guest (KL_GUEST_MACHO: the vivecraft row).
//
// Included by kl_app.c at its end, and only in an app built for such a target
// (KL_MACHO_GUEST, set by gen_xcodeproj.py): it is the rest of that file, not a
// unit of its own, so it reads kl_app.c's own state (the target, the paths, the
// status line, the guest thread) directly. Every other app compiles none of it.
//
// What this guest is. No APK, no klepton-ld, no ELF: a Darwin dylib in
// Frameworks/ (Minecraft: Java Edition with Vivecraft as a GraalVM native-image,
// from rebelancap's vivecraft-visionos) entered through ONE exported C function,
// `int klmc_main(int argc, char **argv)` — native-image's run_main behind a
// shim — on a pthread with a big stack. Ported from that tree's kl_app.c
// (release 1.0.3), where the kind is called "native"; here that name belongs to
// the Android NativeActivity guests.
//
// Layout (the app's "Bundle game payload" phase puts the tree into the bundle,
// and KleptonPayload.swift lays Documents out from it before this runs —
// symlinks for jars and assets, copies for the writable defaults):
//   <bundle>/Frameworks/libminecraft.dylib ... libopenxr_loader.dylib
//   <bundle>/klepton-payload/          the game's files as the release has them
//   <container>/vivecraft/launch.txt   the command line, one argument per line,
//                                      with @ROOT@ @FW@ @CONTAINER@ @HOME@ @TMP@
//                                      @CLASSPATH@ @XMX@ @USER@ placeholders
//   <container>/vivecraft/baked.cp     the baked classpath, one ROOT-relative
//                                      entry per line (the image READS these jars
//                                      at run time: mod resources, client jar)
//   <container>/vivecraft/root/        inst-<version>/{fgamedir,assets,...}
//   <container>/vivecraft/.payload-version
//                                      "bundle" or "external", then the hash
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <pthread/introspection.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>

#include "kl_app.h"       // kl_msa_session_t, and this door's own prototypes
#include "kl_ndk.h"       // kl_ndk_window_set_size — the flat window's surface

int kl_app_target_is_macho(void) {
    const kl_target *t = g_target ? g_target
                                  : kl_target_lookup(kl_env_str("KL_TARGET", default_target()));
    return t && t->kind == KL_GUEST_MACHO;
}

// --- <container>/klepton-env.txt ----------------------------------------------
//
// NAME=VALUE per line, `#` comments. The game's launcher keeps the choices that
// must hold for a whole process in it (windowed or immersive: KL_IMMERSIVE), and
// a knob can be put there from a Mac between launches, because a start from the
// Home View has no command line. Applied FIRST and with overwrite, so it wins
// over the target's defaults and every setenv(..., 0); each line is said out
// loud. Absent file = nothing happens. For this kind of guest only: no other
// target reads such a file.
//
// What it set is kept, so that kl_app_boot can repeat it into the boot log
// (configure runs before the log exists) and a later run in the same process can
// take back a line that has been removed since.
static char g_env_file_applied[1024];

static void macho_env_file_apply(const char *container) {
    char path[1200], line[512];
    snprintf(path, sizeof path, "%s/klepton-env.txt", container);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq || eq == s) {
            fprintf(stderr, "  [env-file] \"%s\" is not NAME=VALUE — skipped\n", s);
            continue;
        }
        *eq = 0;
        setenv(s, eq + 1, 1);
        fprintf(stderr, "  [env-file] %s=%s (klepton-env.txt)\n", s, eq + 1);
        size_t at = strlen(g_env_file_applied);
        snprintf(g_env_file_applied + at, sizeof g_env_file_applied - at,
                 "%s%s=%s", at ? " " : "", s, eq + 1);
    }
    fclose(f);
}

static char g_macho_root[1024];      // <container>/vivecraft
// The game dir and assets dir: launch.txt's --gameDir / --assetsDir (@ROOT@ ->
// <tree>/root). The preflight, the vrEnabled edit and the guest's cwd
// (Vivecraft reads input/profiles/ relative to it) all follow it.
static char g_macho_gamedir[1200];
static char g_macho_container[1024];
static int  g_macho_w = 1280, g_macho_h = 720;

// Value of the launch.txt line after `opt` (one argument per line), @ROOT@ expanded.
static int macho_launch_value(const char *opt, char *out, size_t cap) {
    char p[1200], line[4096];
    snprintf(p, sizeof p, "%s/launch.txt", g_macho_root);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int next = 0, ok = -1;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        if (next) {
            if (!strncmp(line, "@ROOT@", 6)) snprintf(out, cap, "%s/root%s", g_macho_root, line + 6);
            else snprintf(out, cap, "%s", line);
            ok = 0;
            break;
        }
        if (!strcmp(line, opt)) next = 1;
    }
    fclose(f);
    return ok;
}

// VR or a window, and ONE knob decides it — KL_IMMERSIVE, the same one the
// Swift side reads for Immersive.wanted — so the game's mode and the scene the
// app opens cannot disagree. VR means:
//   -Dklepton.vr.android=true    (the image pins os.version to "Android-...",
//                                 which selects Vivecraft's Mobile/OpenXR path)
//   vrEnabled=true               in fgamedir/config/vivecraft-client-config.json
//                                 (rewritten at every boot to match the mode)
//   KLGLFW_VR=1                  the SDL stand-in does not swap the flat window:
//                                 the picture leaves through xrEndFrame
//   no kl_mono capture           nothing reads the window back
//   the OpenXR frame pacer       xrWaitFrame blocks on the compositor's publish
// KL_IMMERSIVE=0 is the game in a visionOS window, VR off.
static int macho_vr(void) { return kl_env_on("KL_IMMERSIVE", 1); }

// Flip "vrEnabled" in Vivecraft's client config to the mode this launch runs
// in. A plain text edit of the one key (the file is Vivecraft's own JSON, one
// "key": "value" per line), so nothing else in it moves.
static void macho_set_vr_config(int on) {
    char p[1300];
    snprintf(p, sizeof p, "%s/config/vivecraft-client-config.json", g_macho_gamedir);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("  [macho] %s missing — Vivecraft will write defaults (VR %s)\n", p, on ? "OFF!" : "off"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 16);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    const char *want = on ? "\"vrEnabled\": \"true\"" : "\"vrEnabled\": \"false\"";
    const char *other = on ? "\"vrEnabled\": \"false\"" : "\"vrEnabled\": \"true\"";
    char *hit = strstr(buf, other);
    if (hit) {
        size_t ol = strlen(other), wl = strlen(want);
        char *out = malloc(got + wl + 16);
        size_t pre = (size_t)(hit - buf);
        memcpy(out, buf, pre);
        memcpy(out + pre, want, wl);
        strcpy(out + pre + wl, hit + ol);
        f = fopen(p, "wb");
        if (f) { fwrite(out, 1, strlen(out), f); fclose(f); }
        free(out);
        printf("  [macho] vivecraft-client-config.json: vrEnabled -> %s\n", on ? "true" : "false");
    } else {
        printf("  [macho] vivecraft-client-config.json: vrEnabled already %s%s\n", on ? "true" : "false",
               strstr(buf, want) ? "" : " (key NOT FOUND)");
    }
    free(buf);
}

static int macho_configure(const char *resources, const char *container) {
    macho_env_file_apply(container);

    snprintf(g_macho_container, sizeof g_macho_container, "%s", container);
    snprintf(g_macho_root, sizeof g_macho_root, "%s/%s", container, g_target->tree);
    snprintf(g_dylibs, sizeof g_dylibs, "%s/Frameworks", resources);
    snprintf(g_libdir, sizeof g_libdir, "%s", g_dylibs);
    snprintf(g_assets, sizeof g_assets, "%s/root/assets", g_macho_root);
    snprintf(g_apk,    sizeof g_apk,    "(none: a Mach-O guest)");
    snprintf(g_files,  sizeof g_files,  "%s", g_macho_root);
    snprintf(g_log,    sizeof g_log,    "%s/klepton-boot.log", container);
    static char crash[1024];
    snprintf(crash, sizeof crash, "%s/klepton-crash.log", container);
    kl_fault_set_crash_path(crash);

    char p[1200];
    snprintf(p, sizeof p, "%s/%s.dylib", g_dylibs, g_target->entry_lib);
    if (!have(p)) return missing("the game's library (mkmacho.sh embeds it)", p);
    snprintf(p, sizeof p, "%s/launch.txt", g_macho_root);
    if (!have(p)) return missing("the game's launch.txt (the payload was not laid out)", p);
    snprintf(p, sizeof p, "%s/baked.cp", g_macho_root);
    if (!have(p)) return missing("the game's baked.cp (the payload was not laid out)", p);
    if (macho_launch_value("--gameDir", g_macho_gamedir, sizeof g_macho_gamedir) != 0)
        snprintf(g_macho_gamedir, sizeof g_macho_gamedir, "%s/root/fgamedir", g_macho_root);
    if (macho_launch_value("--assetsDir", g_assets, sizeof g_assets) != 0)
        snprintf(g_assets, sizeof g_assets, "%s/root/assets", g_macho_root);
    printf("  [macho] game dir %s, assets %s (launch.txt)\n", g_macho_gamedir, g_assets);
    {
        // KleptonPayload.swift wrote this before the launcher. One line in the
        // boot log so a pull shows whether this container came from the bundle.
        char mp[1200], line1[80], line2[80];
        snprintf(mp, sizeof mp, "%s/.payload-version", g_macho_root);
        FILE *mf = fopen(mp, "r");
        if (mf) {
            line1[0] = line2[0] = 0;
            if (fgets(line1, sizeof line1, mf)) {
                size_t n = strlen(line1);
                while (n && (line1[n - 1] == '\n' || line1[n - 1] == '\r')) line1[--n] = 0;
            }
            if (fgets(line2, sizeof line2, mf)) {
                size_t n = strlen(line2);
                while (n && (line2[n - 1] == '\n' || line2[n - 1] == '\r')) line2[--n] = 0;
            }
            fclose(mf);
            printf("  [payload] %s %s\n", line1, line2);
        }
    }
    if (!have(g_macho_gamedir)) return missing("the game dir (the payload was not laid out)", g_macho_gamedir);
    if (!have(g_assets)) return missing("Minecraft's assets (the payload was not laid out)", g_assets);
    {
        // Minecraft 26.x asks its window for GL 3.3 core and refuses less; libltw
        // then reports "3.3 OpenLTW". The SDL stand-in sets this itself on a 3.3
        // request; pinning it here as well makes the report independent of hint
        // order. Not overwritten (the env file wins).
        char ver[64];
        if (macho_launch_value("--version", ver, sizeof ver) == 0 && !strncmp(ver, "26.", 3)) {
            setenv("LTW_REPORT_GL_VERSION", "3.3", 0);
            printf("  [macho] Minecraft %s: LTW_REPORT_GL_VERSION=%s\n", ver, getenv("LTW_REPORT_GL_VERSION"));
        }
    }

    // The flat surface size: kl_glfb's root pbuffer, kl_ndk's window (what
    // eglCreateWindowSurface reports, hence what kl_present calls the mono
    // picture) and the SDL stand-in's window all have to agree, and all three
    // are fixed before ANGLE comes up.
    const char *sz = kl_env_str("KL_NATIVE_SIZE", "1280x720");
    int w = 0, h = 0;
    if (sscanf(sz, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { g_macho_w = w; g_macho_h = h; }
    char wh[32];
    snprintf(wh, sizeof wh, "%dx%d", g_macho_w, g_macho_h);
    setenv("KL_GLFB_SIZE", wh, 0);
    setenv("KLGLFW_SIZE", wh, 0);
    setenv("KLGLFW_MONITOR", wh, 0);
    kl_ndk_window_set_size(NULL, g_macho_w, g_macho_h);
    kl_glfb_set_size(g_macho_w, g_macho_h);

    // The stand-in's app mode: EGL from ltw, which takes ITS EGL from libklegl,
    // which hands every name to macho_egl_proc below (the kl_egl gateway).
    setenv("KLGLFW_HOST", "klepton", 1);
    snprintf(p, sizeof p, "%s/libklegl.dylib", g_dylibs);
    setenv("LTW_EGL_LIBRARY", p, 1);
    snprintf(p, sizeof p, "%s/shots", g_macho_root);
    mkdir(p, 0755);
    setenv("KLGLFW_SHOT_DIR", p, 0);
    setenv("KLGLFW_FPS", "1", 0);
    setenv("KL_ANGLE_DIR", g_dylibs, 0);
    setenv("KL_GLFB", "1", 0);
    {
        int vr = macho_vr();
        printf("  [macho] mode: %s (KL_IMMERSIVE)\n", vr ? "VR — immersive space, Vivecraft Mobile OpenXR -> kl_openxr"
                                                        : "FLAT — a visionOS window, VR off");
        macho_set_vr_config(vr);
        if (vr) setenv("KLGLFW_VR", "1", 1);
        else unsetenv("KLGLFW_VR");
    }
    // The target's defaults (kl_driver_env_defaults) and the phase hook; the
    // driver has no boot sequence for this kind and is asked for none.
    kl_driver_init(g_target, g_libdir, g_door);
    kl_driver_set_phase_hook(app_phase);
    snprintf(g_status, sizeof g_status, "configured");
    return 0;
}

// --- boot and run ---------------------------------------------------------------
// Boot loads the support dylibs in the order their constructors need (libklegl
// primed BEFORE libltw, whose load-time constructor resolves every ES 3.0 entry
// point through it), then the image; run calls its entry on a big-stack pthread
// and returns when the game does.
typedef int (*macho_main_fn)(int, char **);
static macho_main_fn g_macho_main;
static void *g_macho_mc;             // the image's dlopen handle (JNI_GetCreatedJavaVMs for the teardown)
static void (*g_macho_push)(int, int, int, int, int, double, double);
static char **g_macho_argv;
static int    g_macho_argc;
static int    g_macho_rc = -1;

// --- the image's exit(), after the game has returned --------------------------
// A finished isolate stays alive (see macho_isolate_teardown) and so do its
// daemon threads — one of them Minecraft's post-main ClientShutdownWatchdog,
// which sleeps 15 s and then calls System.exit(-8) ("Client shutdown from
// post-main"). In a one-run process that was harmless; with the launcher back
// on screen, or a second game running, it would kill the app. So the image's own
// imports of exit/_exit (its __got, the image only — nothing else in the process
// is touched) are re-pointed here: a call from a thread of a FINISHED isolate
// parks that thread for good (it is in native code, so the isolate is
// indifferent) and says so once; any other call — a crash exit while a game is
// running — exits exactly as before.
static void (*g_real_exit)(int);
static void (*g_real__exit)(int);
static void *volatile g_dead_isolates[8];
static volatile int g_n_dead_isolates;
static void *(*g_graal_cur_thread)(void *);   // graal_get_current_thread(isolate) -> IsolateThread* or NULL

static int macho_thread_is_from_dead_isolate(void) {
    if (!g_graal_cur_thread) return 0;
    for (int i = 0; i < g_n_dead_isolates && i < 8; i++)
        if (g_dead_isolates[i] && g_graal_cur_thread(g_dead_isolates[i])) return 1;
    return 0;
}
// The JDK "closes" fd 0-2 by dup2'ing /dev/null over them, and a finished
// isolate's exit path does exactly that to System.out (the run-1 watchdog's
// System.exit left fd 1 on /dev/null, and every later printf — the second game's
// stdout and ours — vanished from klepton-boot.log). fd 2 is the log as well
// (open_log), so fd 1 is put back on it whenever they differ.
static void macho_repair_stdout(void) {
    struct stat a, b;
    if (fstat(STDOUT_FILENO, &a) != 0 || fstat(STDERR_FILENO, &b) != 0) return;
    if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) return;
    if (!S_ISREG(b.st_mode)) return;   // only when stderr really is the log file
    fflush(stdout);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    fprintf(stderr, "[macho] stdout had been pointed away from the log (a finished isolate closed System.out) — restored\n");
}
static void macho_park_forever(int code, const char *which) {
    macho_repair_stdout();
    char name[64] = "";
    pthread_getname_np(pthread_self(), name, sizeof name);
    printf("[macho] %s(%d) from a thread of a FINISHED game isolate (\"%s\") — swallowed, the thread is parked; "
           "the app and the launcher stay up\n", which, code, name);
    fflush(NULL);
    for (;;) pause();
}
// One game run's end, whichever way it comes: klmc_main returning (Quit Game,
// the normal path) or an exit() from the running game's isolate (a crash exit,
// or the post-main watchdog when shutdown hangs on a non-daemon thread). Either
// way macho_run stops waiting and the window gets the launcher back; on the exit
// path the calling thread is parked and the isolate joins the finished ones.
// KL_MC_EXIT_TO_LAUNCHER=0 = an exit() during a run ends the process as it
// always did.
static pthread_mutex_t g_nrun_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_nrun_cv = PTHREAD_COND_INITIALIZER;
static int  g_nrun_active, g_nrun_done;
static unsigned g_nrun_gen;               // which run macho_run is waiting on (an older entry thread may return late)
// The game closed its window (SDL_Quit, the last act of Minecraft.close()) in
// THIS run. Seen on the headset with the reference build (LAN server + voice
// chat): Quit Game -> Stopping! -> the window closed, and then run_main did not
// return — it sat in SVM's join of the non-daemon Java threads, held by a
// thread the remote session left behind — until the post-main watchdog's
// exit(-8) 15 s later; meanwhile the space showed the last picture and the
// launcher said Playing. So once the window is closed the run is given
// KL_MC_QUIT_GRACE_MS (default 2500; a clean quit returned 2.0 s after the
// window closed) to return, and past it the run ENDS anyway: the isolate is
// parked like any finished one (its watchdog exit is swallowed), the entry
// thread is left wherever it is, and the launcher comes back. 0 = wait for the
// return (or the watchdog) as before.
static int  g_nrun_closed;
static struct timespec g_nrun_closed_at;   // CLOCK_MONOTONIC
static void *volatile g_macho_vm;         // THIS run's JavaVM (its invoke table's reserved0 = the live isolate);
                                          // reset at the start of every run (macho_run)
static int macho_isolate_is_dead(void *iso) {
    for (int i = 0; i < g_n_dead_isolates && i < 8; i++) if (g_dead_isolates[i] == iso) return 1;
    return 0;
}
// DestroyJavaVM unmaps the JavaVM object but JNI_GetCreatedJavaVMs still returns
// that pointer. Dereferencing it is a SIGSEGV. A readable check skips the
// corpse; the next run's VM is a new allocation and shows up once its isolate
// exists.
static int macho_mem_readable(const void *p, size_t n) {
    if (!p || n == 0 || n > 32) return 0;
    if ((uintptr_t)p & (sizeof(void *) - 1)) return 0;
    char buf[32];
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)p, n,
                             (vm_address_t)buf, &got) == KERN_SUCCESS && got == n;
}
// The newest JavaVM whose isolate is NOT a finished (parked) one. Parked
// isolates keep their JavaVM registered, so after a re-run the image reports
// several and "the last one" is not necessarily the live run's.
static void *macho_find_live_vm(void) {
    int (*get_vms)(void **, int, int *) =
        g_macho_mc ? (int (*)(void **, int, int *))dlsym(g_macho_mc, "JNI_GetCreatedJavaVMs") : NULL;
    if (!get_vms) return NULL;
    void *vms[16] = {0}; int n = 0;
    if (get_vms(vms, 16, &n) != 0 || n < 1) return NULL;
    if (n > 16) n = 16;
    static int said_stale;
    for (int i = n - 1; i >= 0; i--) {
        if (!vms[i]) continue;
        if (!macho_mem_readable(vms[i], sizeof(void *))) {
            if (!said_stale++)
                printf("[macho] JNI_GetCreatedJavaVMs returned an unmapped JavaVM "
                       "(a destroyed isolate) — skipped\n");
            continue;
        }
        void *fns = *(void **)vms[i];
        if (!macho_mem_readable(fns, sizeof(void *))) continue;
        void *iso = *(void **)fns;
        if (!macho_isolate_is_dead(iso)) return vms[i];
    }
    return NULL;
}
static void *macho_live_isolate(void) {
    void *vm = g_macho_vm;
    if (!vm || !macho_mem_readable(vm, sizeof(void *))) vm = macho_find_live_vm();
    if (!vm) return NULL;
    void *fns = *(void **)vm;
    if (!macho_mem_readable(fns, sizeof(void *))) return NULL;
    return *(void **)fns;
}
static void macho_mark_dead(void *iso) {
    if (!iso) return;
    if (macho_isolate_is_dead(iso)) return;
    if (g_n_dead_isolates < 8) g_dead_isolates[g_n_dead_isolates++] = iso;
}
static void macho_on_window_closed(void) {
    pthread_mutex_lock(&g_nrun_mu);
    if (g_nrun_active && !g_nrun_done && !g_nrun_closed) {
        g_nrun_closed = 1;
        clock_gettime(CLOCK_MONOTONIC, &g_nrun_closed_at);
        pthread_cond_broadcast(&g_nrun_cv);
    }
    pthread_mutex_unlock(&g_nrun_mu);
}
// 1 while this run's game has closed its window but the run has not ended yet:
// the window dismisses the space at once (the picture is gone) and shows
// Quitting… until kl_app_macho_runs() moves.
int kl_app_macho_closing(void) {
    pthread_mutex_lock(&g_nrun_mu);
    int r = g_nrun_active && !g_nrun_done && g_nrun_closed;
    pthread_mutex_unlock(&g_nrun_mu);
    return r;
}
// Every named thread in the process, once, when a run is cut short: the Java
// thread that held run_main's join is among them (SVM names its pthreads).
static void macho_dump_thread_names(void) {
    thread_act_array_t th; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    printf("[macho] %u threads in the process at the cut; named:", n);
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        pthread_t p = pthread_from_mach_thread_np(th[i]);
        char nm[64] = "";
        if (p) pthread_getname_np(p, nm, sizeof nm);
        if (nm[0]) printf(" \"%s\"", nm);
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
    printf("\n");
}
static void macho_exit_common(int code, const char *which) {
    if (macho_thread_is_from_dead_isolate()) macho_park_forever(code, which);
    if (kl_env_on("KL_MC_EXIT_TO_LAUNCHER", 1)) {
        pthread_mutex_lock(&g_nrun_mu);
        if (g_nrun_active && !g_nrun_done) {
            g_macho_rc = code;
            macho_mark_dead(macho_live_isolate());
            g_nrun_done = 2;
            pthread_cond_broadcast(&g_nrun_cv);
            pthread_mutex_unlock(&g_nrun_mu);
            macho_repair_stdout();
            char name[64] = "";
            pthread_getname_np(pthread_self(), name, sizeof name);
            printf("[macho] the game called %s(%d) on \"%s\" — that ends this run like a return would: the thread "
                   "is parked, the isolate %p is left, the launcher comes back (KL_MC_EXIT_TO_LAUNCHER=0 = exit "
                   "the app instead)\n", which, code, name, macho_live_isolate());
            fflush(NULL);
            for (;;) pause();
        }
        pthread_mutex_unlock(&g_nrun_mu);
    }
}
static void macho_exit_hook(int code) {
    macho_exit_common(code, "exit");
    g_real_exit(code);
    __builtin_unreachable();
}
static void macho__exit_hook(int code) {
    macho_exit_common(code, "_exit");
    g_real__exit(code);
    __builtin_unreachable();
}

// fishhook's walk, for one image: LC_DYSYMTAB's indirect symbols name the
// __got slots; the two exit slots get our hooks. Returns how many slots moved.
static int macho_rebind_exit(const void *some_symbol_in_image) {
    Dl_info info;
    if (!dladdr(some_symbol_in_image, &info) || !info.dli_fbase) return -1;
    const struct mach_header_64 *mh = info.dli_fbase;
    intptr_t slide = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if ((const void *)_dyld_get_image_header(i) == (const void *)mh) { slide = _dyld_get_image_vmaddr_slide(i); break; }
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysym = NULL;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)c)->segname, SEG_LINKEDIT))
            linkedit = (const struct segment_command_64 *)c;
        else if (c->cmd == LC_SYMTAB) symtab = (const struct symtab_command *)c;
        else if (c->cmd == LC_DYSYMTAB) dysym = (const struct dysymtab_command *)c;
    }
    if (!linkedit || !symtab || !dysym || !dysym->nindirectsyms) return -2;
    uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const struct nlist_64 *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *ind = (const uint32_t *)(base + dysym->indirectsymoff);
    int moved = 0;
    lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        if (seg->cmd != LC_SEGMENT_64) continue;
        if (strcmp(seg->segname, "__DATA") && strcmp(seg->segname, "__DATA_CONST")) continue;
        const struct section_64 *sec = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++, sec++) {
            uint32_t type = sec->flags & SECTION_TYPE;
            if (type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS) continue;
            void **slots = (void **)((uintptr_t)slide + sec->addr);
            for (uint64_t k = 0; k < sec->size / sizeof(void *); k++) {
                uint32_t si = ind[sec->reserved1 + k];
                if (si & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;
                if (si >= symtab->nsyms) continue;
                const char *nm = strs + syms[si].n_un.n_strx;
                void *hook = NULL, **real = NULL;
                if (!strcmp(nm, "_exit"))       { hook = (void *)macho_exit_hook;  real = (void **)&g_real_exit; }
                else if (!strcmp(nm, "__exit")) { hook = (void *)macho__exit_hook; real = (void **)&g_real__exit; }
                if (!hook) continue;
                if (!*real) *real = slots[k];
                vm_address_t pg = (vm_address_t)&slots[k] & ~(vm_address_t)(PAGE_SIZE - 1);
                if (vm_protect(mach_task_self(), pg, PAGE_SIZE, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
                    continue;
                slots[k] = hook;
                vm_protect(mach_task_self(), pg, PAGE_SIZE, 0, VM_PROT_READ);
                moved++;
            }
        }
    }
    if (!g_real_exit) g_real_exit = exit;
    if (!g_real__exit) g_real__exit = _exit;
    return moved;
}

// ltw's "libEGL". Three answers, by name:
//   egl*                      kl_egl's own table (kl_egl_lookup), NULL if it has
//                             none — the guest-facing EGL, whose contexts are
//                             backed by kl_glfb's real ANGLE ones in ONE share
//                             group and whose swap reaches the compositor
//   the capability queries    ANGLE's real ones: ltw must see an honest ES 3.0
//                             and the real extension list (it routes base-vertex
//                             draws off it), not kl_egl's Unity-shaped description
//   every other gl*           kl_egl_sym (kl_glfb's thunks / ANGLE) — but only
//                             if ANGLE really has it: kl_egl_sym never answers
//                             NULL (it hands out abort-by-name stubs, and tries a
//                             name again without its EXT/OES suffix), and ltw
//                             reads NULL as "not available"
static void *g_macho_gles;

static void *macho_egl_proc(const char *name) {
    if (!name) return NULL;
    if (!strncmp(name, "egl", 3)) return kl_egl_lookup(name);
    if (!g_macho_gles) {
        char p[1200];
        snprintf(p, sizeof p, "%s/libGLESv2.framework/libGLESv2", kl_env_str("KL_ANGLE_DIR", g_dylibs));
        g_macho_gles = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (!g_macho_gles) {
            fprintf(stderr, "  [macho] cannot open ANGLE's %s: %s\n", p, dlerror());
            return NULL;
        }
    }
    void *real = dlsym(g_macho_gles, name);
    if (!real) return NULL;
    static const char *const queries[] = {
        "glGetString", "glGetStringi", "glGetIntegerv", "glGetFloatv", "glGetBooleanv",
        "glGetInteger64v", "glGetIntegeri_v", "glGetInternalformativ",
    };
    for (size_t i = 0; i < sizeof queries / sizeof *queries; i++)
        if (!strcmp(queries[i], name)) return real;
    // kl_glfb's thunks are written for ANDROID callers (AAPCS64: every stack
    // argument in its own 8-byte slot — they declare those parameters int64_t
    // to read them). A Darwin caller PACKS sub-8-byte stack arguments, so a
    // thunk with two or more stack arguments reads garbage from ltw: measured
    // in the reference tree as glBlitFramebuffer(mask=0x260000004000,
    // filter=junk), i.e. the game's present blit to fb 0 never happened and the
    // window was black. These go straight to ANGLE (they lose kl_glfb's census
    // and, for the copy, its emulation of compressed formats; nothing else).
    // The list is every thunk of this tree with more than eight integer
    // arguments; glCopyImageSubData and its two spellings are this tree's own.
    static const char *const darwin_abi[] = {
        "glBlitFramebuffer", "glTexSubImage3D", "glCompressedTexSubImage3D",
        "glCopyImageSubData", "glCopyImageSubDataEXT", "glCopyImageSubDataOES",
    };
    for (size_t i = 0; i < sizeof darwin_abi / sizeof *darwin_abi; i++)
        if (!strcmp(darwin_abi[i], name)) return real;
    return kl_egl_sym(name);
}

// The flat window's input (kl_mono, ShellWindow) -> the SDL stand-in's event
// ring, which takes GLFW-coded events.
enum { KLG_KEY = 1, KLG_CHAR = 2, KLG_BUTTON = 3, KLG_CURSOR = 4, KLG_SCROLL = 5, KLG_WINDOW_SIZE = 7 };
static int g_macho_button = -1;
// Main-thread window/controller input. These functions address the SAME module
// macho_boot loaded; there is no new guest or frame loop.
static int (*g_macho_cursor_mode)(void);
static void (*g_macho_cursor_pos)(double *, double *);
static unsigned char g_flat_keys[512], g_flat_pad_buttons[3], g_flat_pointer_buttons[3];
static double g_flat_x, g_flat_y, g_flat_observed_x, g_flat_observed_y;
static int g_flat_cursor_known, g_flat_last_mode;
static unsigned g_flat_run_frame_base;
static int g_flat_size_sent;

// Tell Minecraft its window really is KL_NATIVE_SIZE. Its Window keeps the
// size it ASKED for as the space cursor positions are measured in, while the
// framebuffer is what the stand-in made; the stand-in never sends a resize, so
// every menu hover/click landed about 1.6x right and 1.4x down of the pointer.
// One window-size event per run, once the run has presented a frame
// (Minecraft's callbacks are installed by then), makes the two agree. In-world
// look is relative motion and was never affected.
static void macho_flat_sync_size(void) {
    if (g_flat_size_sent || !g_macho_push || macho_vr()) return;
    if (kl_mono_frame_count() == g_flat_run_frame_base) return;
    g_flat_size_sent = 1;
    g_macho_push(KLG_WINDOW_SIZE, g_macho_w, g_macho_h, 0, 0, 0, 0);
    fprintf(stderr, "  [macho] flat window size sent to the game: %dx%d\n", g_macho_w, g_macho_h);
}

int kl_app_macho_windowed_ready(void) {
    if (!target_is_macho() || macho_vr() || !g_macho_push || !g_macho_cursor_mode || !g_macho_cursor_pos) return 0;
    pthread_mutex_lock(&g_nrun_mu);
    int ready = g_nrun_active && !g_nrun_done && !g_nrun_closed && kl_mono_frame_count() != g_flat_run_frame_base;
    pthread_mutex_unlock(&g_nrun_mu);
    if (ready) macho_flat_sync_size();
    return ready;
}
int kl_app_macho_windowed_cursor_mode(void) {
    return kl_app_macho_windowed_ready() ? g_macho_cursor_mode() : 0;
}
static void macho_flat_button(int button, int down, int pointer) {
    if (button < 0 || button >= 3 || !g_macho_push) return;
    unsigned char *owner = pointer ? g_flat_pointer_buttons : g_flat_pad_buttons;
    int before = g_flat_pad_buttons[button] || g_flat_pointer_buttons[button];
    owner[button] = down != 0;
    int after = g_flat_pad_buttons[button] || g_flat_pointer_buttons[button];
    if (before != after) g_macho_push(KLG_BUTTON, button, after, 0, 0, 0, 0);
}
void kl_app_macho_windowed_key(int key, int down) {
    if (key < 0 || key >= (int)sizeof g_flat_keys || !g_macho_push) return;
    if (down && !kl_app_macho_windowed_ready()) return;
    down = down != 0;
    if (g_flat_keys[key] == down) return;
    g_flat_keys[key] = down;
    g_macho_push(KLG_KEY, key, 0, down, 0, 0, 0);
}
void kl_app_macho_windowed_button(int button, int down) {
    if (down && !kl_app_macho_windowed_ready()) return;
    macho_flat_button(button, down, 0);
}
static void macho_flat_note_pointer(double x, double y) {
    if (!isfinite(x) || !isfinite(y)) return;
    g_flat_x = x; g_flat_y = y; g_flat_cursor_known = 1;
    if (g_macho_cursor_pos) g_macho_cursor_pos(&g_flat_observed_x, &g_flat_observed_y);
    if (g_macho_cursor_mode) g_flat_last_mode = g_macho_cursor_mode();
}
void kl_app_macho_windowed_move(double dx, double dy) {
    if (!kl_app_macho_windowed_ready() || !isfinite(dx) || !isfinite(dy)) return;
    double observed_x = 0, observed_y = 0;
    g_macho_cursor_pos(&observed_x, &observed_y);
    int mode = g_macho_cursor_mode();
    // The exported position changes when Minecraft warps the cursor. Ring
    // injection itself does not change it, so retain our accumulated position
    // unless the game warped or changed grab mode. Pinch updates it as well.
    if (!g_flat_cursor_known || mode != g_flat_last_mode ||
        observed_x != g_flat_observed_x || observed_y != g_flat_observed_y) {
        g_flat_x = isfinite(observed_x) ? observed_x : 0;
        g_flat_y = isfinite(observed_y) ? observed_y : 0;
        g_flat_cursor_known = 1;
    }
    g_flat_observed_x = observed_x; g_flat_observed_y = observed_y; g_flat_last_mode = mode;
    g_flat_x += dx; g_flat_y += dy;
    if (mode != 0x34003) { // GLFW_CURSOR_DISABLED: unbounded deltas for MouseHandler
        g_flat_x = fmax(0, fmin(g_macho_w - 1, g_flat_x));
        g_flat_y = fmax(0, fmin(g_macho_h - 1, g_flat_y));
    }
    g_macho_push(KLG_CURSOR, 0, 0, 0, 0, g_flat_x, g_flat_y);
}
void kl_app_macho_windowed_scroll(double y) {
    if (kl_app_macho_windowed_ready() && isfinite(y)) g_macho_push(KLG_SCROLL, 0, 0, 0, 0, 0, y);
}
void kl_app_macho_windowed_release_all(void) {
    // Releases remain legal after the guest stops accepting new presses.
    for (int key = 0; key < (int)sizeof g_flat_keys; key++) {
        if (g_flat_keys[key] && g_macho_push) g_macho_push(KLG_KEY, key, 0, 0, 0, 0, 0);
        g_flat_keys[key] = 0;
    }
    for (int button = 0; button < 3; button++) {
        if ((g_flat_pad_buttons[button] || g_flat_pointer_buttons[button]) && g_macho_push)
            g_macho_push(KLG_BUTTON, button, 0, 0, 0, 0, 0);
        g_flat_pad_buttons[button] = g_flat_pointer_buttons[button] = 0;
    }
    g_macho_button = -1;
}

static void macho_pointer(int state, int action, float x, float y) {
    if (!g_macho_push) return;
    macho_flat_sync_size();
    // A scroll carries wheel deltas in x/y, not a position: it must neither
    // move the cursor nor become the flat-input tracker's "known" position
    // (the next gamepad move would continue from roughly (0, ±1)).
    if (action == KL_MONO_SCROLL) { g_macho_push(KLG_SCROLL, 0, 0, 0, 0, x, y); return; }
    macho_flat_note_pointer(x, y);
    g_macho_push(KLG_CURSOR, 0, 0, 0, 0, x, y);
    if (action == KL_MONO_DOWN) {
        g_macho_button = (state & KL_MONO_BTN_SECONDARY) ? 1 : (state & KL_MONO_BTN_TERTIARY) ? 2 : 0;
        macho_flat_button(g_macho_button, 1, 1);
    } else if (action == KL_MONO_UP && g_macho_button >= 0) {
        macho_flat_button(g_macho_button, 0, 1);
        g_macho_button = -1;
    }
}
// Android keycodes (what kl_mono_keycode_for_char produces) -> GLFW keys, plus
// the character for a printable key on its press.
static void macho_key(int down, int code) {
    if (!g_macho_push) return;
    int key = 0, ch = 0;
    if (code >= 29 && code <= 54)      { key = 65 + (code - 29); ch = 'a' + (code - 29); }
    else if (code >= 7 && code <= 16)  { key = 48 + (code - 7);  ch = '0' + (code - 7); }
    else switch (code) {
    case 66: key = 257; break;           // ENTER
    case 67: key = 259; break;           // DEL -> BACKSPACE
    case 61: key = 258; break;           // TAB
    case 62: key = 32;  ch = ' '; break; // SPACE
    case 56: key = 46;  ch = '.'; break;
    case 55: key = 44;  ch = ','; break;
    case 69: key = 45;  ch = '-'; break;
    case 4:  key = 256; break;           // BACK -> ESCAPE
    default: return;
    }
    g_macho_push(KLG_KEY, key, 0, down ? 1 : 0, 0, 0, 0);
    if (down && ch) g_macho_push(KLG_CHAR, ch, 0, 0, 0, 0, 0);
}

static void *macho_open(const char *lib, int mode) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s.dylib", g_dylibs, lib);
    void *h = dlopen(p, mode);
    if (!h) fprintf(stderr, "  [macho] dlopen %s: %s\n", p, dlerror());
    else    fprintf(stderr, "  [macho] loaded %s\n", p);
    return h;
}

// Expand one launch.txt line. Returns a malloc'd string.
static char *macho_expand(const char *line, const char *cp) {
    char tmp[1024];
    const char *t = getenv("TMPDIR");
    snprintf(tmp, sizeof tmp, "%s", t && *t ? t : "/tmp");
    size_t n = strlen(tmp); while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    char root[1200];
    snprintf(root, sizeof root, "%s/root", g_macho_root);
    struct { const char *key, *val; } subst[] = {
        { "@ROOT@", root }, { "@FW@", g_dylibs }, { "@CONTAINER@", g_macho_container },
        { "@HOME@", g_macho_root }, { "@TMP@", tmp }, { "@CLASSPATH@", cp },
        { "@XMX@", kl_env_str("KL_MC_XMX", "3G") }, { "@USER@", kl_env_str("KL_MC_USER", "Player") },
    };
    size_t cap = strlen(line) + 1;
    for (size_t i = 0; i < sizeof subst / sizeof *subst; i++)
        for (const char *q = strstr(line, subst[i].key); q; q = strstr(q + 1, subst[i].key))
            cap += strlen(subst[i].val);
    char *out = malloc(cap), *o = out;
    for (const char *s = line; *s; ) {
        size_t i = 0;
        for (; i < sizeof subst / sizeof *subst; i++) {
            size_t kl = strlen(subst[i].key);
            if (!strncmp(s, subst[i].key, kl)) {
                size_t vl = strlen(subst[i].val);
                memcpy(o, subst[i].val, vl); o += vl; s += kl;
                break;
            }
        }
        if (i == sizeof subst / sizeof *subst) *o++ = *s++;
    }
    *o = 0;
    return out;
}

static void macho_argv_add(char *a) {
    g_macho_argv = realloc(g_macho_argv, sizeof(char *) * (size_t)(g_macho_argc + 2));
    g_macho_argv[g_macho_argc++] = a;
    g_macho_argv[g_macho_argc] = NULL;
}
static void macho_argv_split(const char *env) {
    const char *v = getenv(env);
    if (!v || !*v) return;
    char *dup = strdup(v), *save = NULL;
    for (char *tok = strtok_r(dup, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        macho_argv_add(macho_expand(tok, ""));
    free(dup);
}

// The VR switch (see macho_vr): a JVM option, so it goes with the others.
static void macho_vr_args(void) {
    if (macho_vr()) macho_argv_add(strdup("-Dklepton.vr.android=true"));
}

// The Microsoft/Mojang session (Minecraft/KleptonMSA.swift, Swift because
// URLSession is the only HTTPS stack the app has): launch.txt carries an
// OFFLINE profile (--username @USER@, --uuid 0..., --accessToken 0); when the
// stored refresh token yields a session, its values replace those in place and
// --userType msa (+ --xuid) are added, so online-mode servers and Realms accept
// the join. KL_MSA=0 keeps the offline profile. Any failure is one line and the
// boot goes on offline. The token is never printed (the argv dump masks it).
extern int kl_msa_session(const char *tree, kl_msa_session_t *out);
// Minecraft/KleptonMSA.swift. 1 = this device may run the Minecraft guest, 0 =
// the ownership gate refuses. Not an env knob: KL_MSA / KL_LAUNCHER / KL_AUTOBOOT
// / KL_MC_PROFILES / the env file / a launcher script cannot make this return 1.
extern int kl_ownership_allows_launch(const char *tree);
static void macho_argv_set(const char *opt, const char *val) {
    for (int i = 0; i + 1 < g_macho_argc; i++)
        if (!strcmp(g_macho_argv[i], opt)) {
            free(g_macho_argv[i + 1]);
            g_macho_argv[i + 1] = strdup(val);
            return;
        }
    macho_argv_add(strdup(opt));
    macho_argv_add(strdup(val));
}
static void macho_msa_args(void) {
    if (!kl_env_on("KL_MSA", 1)) { printf("  [msa] offline: KL_MSA=0\n"); return; }
    kl_msa_session_t *s = calloc(1, sizeof *s);
    if (!s) return;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = kl_msa_session(g_target->tree, s);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (s->summary[0]) printf("  [msa] accounts: %s\n", s->summary);
    if (rc == 0 && s->free_account && s->name[0] && s->uuid[0]) {
        // No Java Edition: offline under the account's own name (MultiMC-style).
        // --accessToken 0 and launch.txt's user type stay; no --userType msa,
        // no --xuid.
        macho_argv_set("--username", s->name);
        macho_argv_set("--uuid", s->uuid);
        printf("  [msa] offline-owned account %s (free account) — offline uuid %s [%s, %.1f s]\n",
               s->name, s->uuid, s->note, secs);
    } else if (rc == 0 && s->name[0] && s->uuid[0] && s->token[0]) {
        macho_argv_set("--username", s->name);
        macho_argv_set("--uuid", s->uuid);
        macho_argv_set("--accessToken", s->token);
        macho_argv_set("--userType", "msa");
        if (s->xuid[0]) macho_argv_set("--xuid", s->xuid);
        printf("  [msa] session for %s (uuid %s) — expires in %.0f h [%s, %.1f s]\n",
               s->name, s->uuid, s->hours, s->note, secs);
    } else {
        printf("  [msa] offline: %s [%.1f s]\n", s->note[0] ? s->note : "no session", secs);
    }
    volatile char *z = (volatile char *)s;
    for (size_t i = 0; i < sizeof *s; i++) z[i] = 0;
    free(s);
}

// The launcher's play target (Minecraft/KleptonMCLauncher.swift): Swift
// setenv()s KL_MC_LAUNCH_ARGV before kl_app_configure, ONE ARGUMENT PER LINE (a
// world folder may hold spaces, so KL_MC_ARGS' whitespace split cannot carry
// it), e.g. "--quickPlaySingleplayer\nMy World" or
// "--quickPlayMultiplayer\nhost:25565". A dev run's own --quickPlay in
// KL_MC_ARGS wins: two quick-plays would fight. Unset/empty = the title screen.
static void macho_launcher_args(void) {
    const char *v = getenv("KL_MC_LAUNCH_ARGV");
    if (!v || !*v) return;
    const char *dev = getenv("KL_MC_ARGS");
    if (dev && strstr(dev, "--quickPlay")) {
        printf("  [launcher] play target ignored: KL_MC_ARGS already has a --quickPlay\n");
        return;
    }
    char *dup = strdup(v), *save = NULL;
    for (char *tok = strtok_r(dup, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save))
        if (*tok) macho_argv_add(strdup(tok));
    free(dup);
    printf("  [launcher] play target from the launcher window (KL_MC_LAUNCH_ARGV)\n");
}

// The per-account game dir (Minecraft/KleptonMCProfiles.swift): Swift prepares
// <instance>/profiles/<key>/ on every Play press and setenv()s its absolute
// path as KL_MC_GAMEDIR; it replaces launch.txt's --gameDir in the argv, the
// guest's cwd (macho_thread) and the vrEnabled edit. Read HERE, per argv build,
// not at configure: a later Play in the same process may pick another account.
// Unset/empty (KL_MC_PROFILES=0, a dev run) = launch.txt's shared fgamedir.
static void macho_profile_gamedir(void) {
    const char *gd = getenv("KL_MC_GAMEDIR");
    char want[sizeof g_macho_gamedir];
    if (gd && *gd) snprintf(want, sizeof want, "%s", gd);
    else if (macho_launch_value("--gameDir", want, sizeof want) != 0)
        snprintf(want, sizeof want, "%s/root/fgamedir", g_macho_root);
    if (gd && *gd && !have(want)) {
        printf("  [macho] KL_MC_GAMEDIR %s missing — the shared game dir instead\n", want);
        if (macho_launch_value("--gameDir", want, sizeof want) != 0)
            snprintf(want, sizeof want, "%s/root/fgamedir", g_macho_root);
        gd = NULL;
    }
    int changed = strcmp(want, g_macho_gamedir) != 0;
    snprintf(g_macho_gamedir, sizeof g_macho_gamedir, "%s", want);
    const char *note = getenv("KL_MC_PROFILE_NOTE");
    printf("  [macho] game dir %s (%s%s%s)\n", g_macho_gamedir, gd && *gd ? "profile" : "shared, launch.txt",
           note && *note ? "; " : "", note && *note ? note : "");
    if (changed) macho_set_vr_config(macho_vr());
}

static int macho_build_argv(void) {
    char path[1200], line[8192];
    macho_profile_gamedir();
    // The classpath: ROOT-relative entries -> absolute, ':'-joined.
    snprintf(path, sizeof path, "%s/baked.cp", g_macho_root);
    FILE *f = fopen(path, "r");
    if (!f) return fail("cannot read baked.cp");
    size_t cpcap = 1 << 16, cplen = 0;
    char *cp = malloc(cpcap); cp[0] = 0;
    int entries = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        size_t need = cplen + strlen(g_macho_root) + n + 16;
        if (need > cpcap) { cpcap = need * 2; cp = realloc(cp, cpcap); }
        cplen += (size_t)snprintf(cp + cplen, cpcap - cplen, "%s%s/root/%s", entries ? ":" : "", g_macho_root, line);
        entries++;
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/launch.txt", g_macho_root);
    f = fopen(path, "r");
    if (!f) { free(cp); return fail("cannot read launch.txt"); }
    int jvm_extra_done = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        // The first non-option argument ends the VM options: KL_MC_JVM_ARGS go in
        // just before it, where run_main still parses them as options.
        if (!jvm_extra_done && g_macho_argc > 0 && line[0] != '-') {
            macho_vr_args(); macho_argv_split("KL_MC_JVM_ARGS"); jvm_extra_done = 1;
        }
        if (!jvm_extra_done && g_macho_argc > 0 && !strncmp(line, "--", 2)) {
            macho_vr_args(); macho_argv_split("KL_MC_JVM_ARGS"); jvm_extra_done = 1;
        }
        macho_argv_add(macho_expand(line, cp));
    }
    fclose(f);
    free(cp);
    if (!jvm_extra_done) { macho_vr_args(); macho_argv_split("KL_MC_JVM_ARGS"); }
    macho_argv_split("KL_MC_ARGS");
    macho_argv_set("--gameDir", g_macho_gamedir);
    // Ask for the window the stand-in will actually make (KL_NATIVE_SIZE) rather
    // than Minecraft's 854x480 default. On its own this did not fix the menu
    // pointer — macho_flat_sync_size's window-size event does — but a request
    // that matches the real window is the right input either way. VR is left as
    // it was: its GUI is Vivecraft's and its clicks were measured there.
    if (!macho_vr()) {
        char v[16];
        snprintf(v, sizeof v, "%d", g_macho_w); macho_argv_set("--width", v);
        snprintf(v, sizeof v, "%d", g_macho_h); macho_argv_set("--height", v);
    }
    macho_launcher_args();
    macho_msa_args();
    printf("  [macho] classpath: %d entries under %s/root\n", entries, g_macho_root);
    printf("  [macho] command line (%d args):\n", g_macho_argc);
    for (int i = 0; i < g_macho_argc; i++) {
        const char *a = g_macho_argv[i];
        if (!strncmp(a, "-Dbaked.classpath=", 18))
            printf("    [%d] -Dbaked.classpath=<%d entries, %zu bytes>\n", i, entries, strlen(a));
        else if (i > 0 && !strcmp(g_macho_argv[i - 1], "--accessToken"))
            printf("    [%d] <token: %zu chars>\n", i, strlen(a));
        else
            printf("    [%d] %s\n", i, a);
    }
    fflush(NULL);
    return 0;
}

static int macho_boot(void) {
    app_phase("macho boot");
    fprintf(stderr, "  [env-file] applied: %s\n", g_env_file_applied[0] ? g_env_file_applied : "(none)");
    printf("=== the Mach-O guest: %s.dylib (no APK, no klepton-ld) ===\n", g_target->entry_lib);
    printf("  root      : %s\n", g_macho_root);
    printf("  surface   : %dx%d (KL_NATIVE_SIZE)\n", g_macho_w, g_macho_h);
    fflush(NULL);

    // The flat window: every swap is read back into kl_mono's buffer and the
    // window's pointer/keys come back into the stand-in's ring.
    // (VR: no capture — the picture leaves through xrEndFrame, and nothing
    // reads the flat window back. The input hooks are harmless either way.)
    if (!macho_vr()) kl_mono_capture_start();
    kl_mono_set_input_hooks(macho_pointer, macho_key);

    // 1. libklegl, primed BEFORE ltw can load.
    void *egl = macho_open("libklegl", RTLD_NOW | RTLD_LOCAL);
    void (*set)(void *(*)(const char *)) = egl ? (void (*)(void *(*)(const char *)))dlsym(egl, "klegl_bridge_set") : NULL;
    if (!set) return fail("libklegl.dylib missing or without klegl_bridge_set");
    set(macho_egl_proc);
    // 2. VLoader.getEGL* answer from the same gateway (eglGetCurrent*), so the
    //    handles Vivecraft gives kl_openxr are the ones the game's context has.
    void *vl = macho_open("libvloader", RTLD_NOW | RTLD_LOCAL);
    void (*vset)(void *(*)(const char *)) = vl ? (void (*)(void *(*)(const char *)))dlsym(vl, "vloader_set_egl_proc") : NULL;
    if (vset) vset(macho_egl_proc);
    // 2b. libopenxr_loader (LWJGL-OpenXR's loader) -> kl_openxr, primed
    //     explicitly: the shim's weak flat-namespace import of kl_openxr_sym
    //     needs the app image to EXPORT it, which a stripped build need not do.
    //     Priming it here makes the door independent of that. The second
    //     argument is the spec-strict input switch Vivecraft's action model
    //     relies on (kl_openxr.h); the shim calls it with 1.
    void *xl = macho_open("libopenxr_loader", RTLD_NOW | RTLD_LOCAL);
    void (*xset)(void *(*)(const char *), void (*)(int)) =
        xl ? (void (*)(void *(*)(const char *), void (*)(int)))dlsym(xl, "klxr_shim_set") : NULL;
    if (xset) xset(kl_openxr_sym, kl_openxr_set_spec_input);
    // ...and said here as well, so the input rules do not depend on the shim
    // remembering to: nothing but this guest comes through this door.
    kl_openxr_set_spec_input(1);
    printf("  [macho] OpenXR door: %s\n", xset ? "libopenxr_loader primed -> kl_openxr_sym (spec input on)"
                                               : "libopenxr_loader WITHOUT klxr_shim_set — flat lookup only");
    // 3. ltw: its constructor resolves ES 3.0 through libklegl, which brings
    //    ANGLE (kl_glfb) up here, on this thread, before the guest exists.
    app_phase("macho ltw");
    if (!macho_open("libltw", RTLD_NOW | RTLD_LOCAL)) return fail("libltw.dylib did not load");
    // 4. the windowing stand-in's platform side: libklsdl (Minecraft 26.3+,
    //    SDL3) when the app embeds it, else libklglfw. libklsdl includes the
    //    GLFW one, so both export the same klglfw_* C API, take the same
    //    GLFW-coded events and read the same KLGLFW_* environment; launch.txt's
    //    -Dklsdl.libpath / -Dklglfw.libpath name the same file, so the image's
    //    System.load gets this very module.
    char wpath[1200];
    snprintf(wpath, sizeof wpath, "%s/libklsdl.dylib", g_dylibs);
    const char *wl = have(wpath) ? "libklsdl" : "libklglfw";
    void *gf = macho_open(wl, RTLD_NOW | RTLD_LOCAL);
    g_macho_push = gf ? (void (*)(int, int, int, int, int, double, double))dlsym(gf, "klglfw_push_event") : NULL;
    g_macho_cursor_mode = gf ? (int (*)(void))dlsym(gf, "klglfw_get_cursor_mode") : NULL;
    g_macho_cursor_pos = gf ? (void (*)(double *, double *))dlsym(gf, "klglfw_get_cursor_pos") : NULL;
    printf("  [macho] input: %s%s\n", g_macho_push ? "window pointer/keys -> klglfw_push_event in " : "NONE (no klglfw_push_event in ",
           g_macho_push ? wl : "libklglfw/libklsdl)");
    void (*set_term)(void (*)(void)) = gf ? (void (*)(void (*)(void)))dlsym(gf, "klglfw_set_terminate_hook") : NULL;
    if (set_term) set_term(macho_on_window_closed);
    printf("  [macho] quit grace: %s\n", set_term ? "glfwTerminate / SDL_Quit -> macho_on_window_closed (KL_MC_QUIT_GRACE_MS)"
                                                 : "NONE (no klglfw_set_terminate_hook)");
    // 5. the image.
    app_phase("macho image");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    void *mc = macho_open(g_target->entry_lib, RTLD_NOW | RTLD_LOCAL);
    g_macho_mc = mc;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (mc) {
        g_graal_cur_thread = (void *(*)(void *))dlsym(mc, "graal_get_current_thread");
        void *probe = dlsym(mc, "run_main");
        int moved = probe ? macho_rebind_exit(probe) : -3;
        printf("  [macho] exit hook: %d __got slot(s) of the image re-pointed (exit/_exit from a finished isolate "
               "is swallowed)%s\n", moved, g_graal_cur_thread ? "" : " — NO graal_get_current_thread, hook inert");
    }
    if (!mc) return fail("the game's library did not load (see dlopen above)");
    const char *(*ver)(void) = (const char *(*)(void))dlsym(mc, "klmc_version");
    const char *entry = kl_env_str("KL_NATIVE_ENTRY", "klmc_main");
    g_macho_main = (macho_main_fn)dlsym(mc, entry);
    if (!g_macho_main) { entry = "run_main"; g_macho_main = (macho_main_fn)dlsym(mc, entry); }
    printf("  [macho] %s.dylib mapped in %.0f ms; %s; entry %s = %p\n", g_target->entry_lib,
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
           ver ? ver() : "(no klmc_version)", entry, (void *)g_macho_main);
    if (!g_macho_main) return fail("the game's library exports neither klmc_main nor run_main");
    if (macho_build_argv() != 0) return 1;
    printf("\n=== EXIT CRITERION MET: the Mach-O guest is mapped and its entry resolved ===\n");
    fflush(NULL);
    snprintf(g_status, sizeof g_status, "the game is loaded");
    return 0;
}

// --- quit to the launcher: the isolate after the game returns ----------------
// run_main (GraalVM --shared main-class entry) creates an isolate, runs main,
// joins the non-daemon threads and runs the shutdown hooks — and then returns
// WITHOUT tearing the isolate down. Its daemon threads keep running and its
// heap stays mapped: 13 s after "the guest's entry returned 0", Minecraft's own
// post-main ClientShutdownWatchdog (a daemon sleeping 15 s from the end of main)
// wrote "Client shutdown from post-main" and called System.exit(-8). So the
// isolate is destroyed here, through the image's own JNI_GetCreatedJavaVMs +
// DestroyJavaVM (SVM: attach, join the non-daemons — none left — interrupt every
// other Java thread, wait for them, unmap the heap). That interrupt is also
// what ends the watchdog's sleep. The image's BakedLauncher calls
// klepton.KleptonTeardown after Main.main, which is what lets the sound
// engine's and Netty's threads actually exit. DestroyJavaVM then runs (bounded
// by KL_MC_TEARDOWN_SECS; past it the launcher falls back to exiting the app).
// KL_MC_TEARDOWN=0 parks the isolate instead. KL_MC_RERUN=0 takes the exit
// fallback always. Everything the NEXT run needs — a new isolate, argv, the env
// — is rebuilt by kl_app_macho_rerun().
static volatile int g_macho_runs;           // guest returns so far
static volatile int g_macho_rerun_ok = 1;   // 0 once a teardown failed / timed out
static volatile int g_macho_teardown_busy;  // DestroyJavaVM in flight: quit-grace must not cut the run
static double g_macho_teardown_ms;

typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int done, rc; void *vm; } macho_td_t;
static void *macho_td_thread(void *u) {
    macho_td_t *td = u;
    // JavaVM is { const JNIInvokeInterface_ *functions; }; slot 3 = DestroyJavaVM
    // (reserved0..2 first). Typed here: the visionOS SDK ships no jni.h.
    void *const *fns = *(void *const *const *)td->vm;
    int (*destroy)(void *) = (int (*)(void *))fns[3];
    int rc = destroy(td->vm);
    pthread_mutex_lock(&td->mu);
    td->rc = rc; td->done = 1;
    pthread_cond_signal(&td->cv);
    pthread_mutex_unlock(&td->mu);
    return NULL;
}

static void macho_isolate_teardown(void) {
    void *vm = g_macho_vm ? g_macho_vm : macho_find_live_vm();
    if (!vm) {
        printf("[macho] the finished isolate: no live JavaVM (JNI_GetCreatedJavaVMs) — no re-run in this process\n");
        g_macho_rerun_ok = 0;
        return;
    }
    void *const *fns = *(void *const *const *)vm;   // JNIInvokeInterface_: reserved0 = SVM's Isolate
    void *iso = fns[0];
    if (!kl_env_on("KL_MC_TEARDOWN", 1)) {
        // Opt-out: the finished isolate is LEFT. Its heap stays mapped and its
        // daemon threads stay parked (Netty's local event loop, the sound
        // engine executor, the post-main watchdog — whose System.exit is
        // swallowed by macho_exit_hook from here on).
        macho_mark_dead(iso);
        printf("[macho] the finished game's isolate %p is parked (not torn down: KL_MC_TEARDOWN=0); "
               "its exit() is swallowed from now on — the process is ready for another Play\n", iso);
        fflush(NULL);
        return;
    }
    unsigned secs = kl_env_uint("KL_MC_TEARDOWN_SECS", 20);
    long long foot0 = kl_phys_footprint();
    // Drop the caller's pointer before the object is unmapped. The quit grace
    // waits this out (g_macho_teardown_busy) instead of returning to the
    // launcher while DestroyJavaVM is still freeing the heap.
    g_macho_vm = NULL;
    g_macho_teardown_busy = 1;
    printf("[macho] isolate teardown starting (KL_MC_TEARDOWN=1), footprint %lld MB\n", foot0 >> 20);
    fflush(NULL);
    macho_td_t *td = calloc(1, sizeof *td);   // leaked on a timeout: the helper may still write it
    pthread_mutex_init(&td->mu, NULL);
    pthread_cond_init(&td->cv, NULL);
    td->vm = vm;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &a, macho_td_thread, td) != 0) {
        pthread_attr_destroy(&a);
        printf("[macho] isolate teardown: no thread — no re-run in this process\n");
        g_macho_rerun_ok = 0;
        return;
    }
    pthread_attr_destroy(&a);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += secs;
    pthread_mutex_lock(&td->mu);
    while (!td->done)
        if (pthread_cond_timedwait(&td->cv, &td->mu, &dl) == ETIMEDOUT) break;
    int done = td->done, rc = td->rc;
    pthread_mutex_unlock(&td->mu);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_macho_teardown_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (!done) {
        macho_mark_dead(iso);
        printf("[macho] isolate teardown (KL_MC_TEARDOWN=1) did NOT finish within %u s (Java threads that ignore "
               "the interrupt) — no re-run in this process; the launcher will exit the app instead\n", secs);
        g_macho_rerun_ok = 0;
    } else if (rc != 0) {
        printf("[macho] isolate teardown: DestroyJavaVM returned %d after %.0f ms — no re-run in this process\n",
               rc, g_macho_teardown_ms);
        g_macho_rerun_ok = 0;
        free(td);
    } else {
        printf("[macho] isolate torn down in %.0f ms (DestroyJavaVM) — footprint %lld MB (was %lld MB); "
               "the process is ready for another Play\n",
               g_macho_teardown_ms, kl_phys_footprint() >> 20, foot0 >> 20);
        free(td);
    }
    fflush(NULL);
}

// --- the game's threads ---------------------------------------------------------
//
// The runtime's own thread shim only sees an ELF guest's threads (they come
// through the bionic table). This guest calls Darwin's pthread_create directly,
// with an attr that sets a stack size and nothing else, so every Java thread it
// starts (Minecraft's Worker-Main-N chunk meshers, the integrated "Server
// thread", IO-Worker-N, Sodium's pools) is born QOS_CLASS_DEFAULT: a plain attr
// does NOT inherit the creator's class on Darwin (sys/qos.h says so, and a host
// test in the reference tree confirmed it). Measured there: those pools at
// ~100 ms/frame of CPU while exploring, with 176-217 ms frame spikes.
//
// The mechanism is libpthread's introspection hook, not a rebinding of the
// image's pthread_create import (chained fixups, the GOT in __DATA_CONST). The
// hook needs no patching and catches EVERY thread the guest starts, through any
// library (LWJGL natives, OpenAL), by LINEAGE:
//   CREATE (delivered on the creating thread): if the creator carries the guest
//          mark, the child gets it too (pthread_introspection_setspecific_np)
//   START  (delivered on the new thread itself): a marked thread whose class is
//          still DEFAULT (its creator asked for no class or priority) takes
//          KL_GUEST_QOS (user-initiated by default). A creator that asked for a
//          class or a sched priority keeps it.
// The lineage root is the entry thread (macho_guest_thread_mark): it runs
// klmc_main, which is also Minecraft's "Render thread", and is created
// user-interactive below. Workers stay a tier below it.
//
// KL_GUEST_QOS here names the class (off, background, utility, default,
// initiated, interactive), as it does in the reference tree. The runtime's own
// KL_GUEST_QOS (an ELF guest's threads, on or off) never meets this guest.
static pthread_key_t g_ng_key;
static pthread_introspection_hook_t g_ng_prev;
static qos_class_t g_ng_qos;
static int g_ng_on;
static atomic_uint g_ng_raised, g_ng_kept, g_ng_failed;

static const char *macho_qos_name(qos_class_t q) {
    return q == QOS_CLASS_UNSPECIFIED      ? "unspecified" :
           q == QOS_CLASS_BACKGROUND       ? "background"  :
           q == QOS_CLASS_UTILITY          ? "utility"     :
           q == QOS_CLASS_DEFAULT          ? "default"     :
           q == QOS_CLASS_USER_INTERACTIVE ? "user-interactive" :
                                             "user-initiated";
}

static qos_class_t macho_guest_qos(void) {
    qos_class_t qos = QOS_CLASS_USER_INITIATED;
    const char *s = kl_env_str("KL_GUEST_QOS", NULL);
    if (s) {
        if      (!strcmp(s, "off") || !strcmp(s, "0")) qos = QOS_CLASS_UNSPECIFIED;
        else if (!strcmp(s, "background"))  qos = QOS_CLASS_BACKGROUND;
        else if (!strcmp(s, "utility"))     qos = QOS_CLASS_UTILITY;
        else if (!strcmp(s, "default"))     qos = QOS_CLASS_DEFAULT;
        else if (!strcmp(s, "initiated") || !strcmp(s, "1")) qos = QOS_CLASS_USER_INITIATED;
        else if (!strcmp(s, "interactive")) qos = QOS_CLASS_USER_INTERACTIVE;
        else fprintf(stderr, "  [macho] KL_GUEST_QOS=%s is not a class name (off/background/utility/default/"
                             "initiated/interactive) — using initiated\n", s);
    }
    return qos;
}

static void macho_guest_hook(unsigned int ev, pthread_t t, void *addr, size_t size) {
    if (ev == PTHREAD_INTROSPECTION_THREAD_CREATE) {
        if (pthread_getspecific(g_ng_key))
            pthread_introspection_setspecific_np(t, g_ng_key, (void *)1);
    } else if (ev == PTHREAD_INTROSPECTION_THREAD_START) {
        if (pthread_getspecific(g_ng_key)) {
            // DEFAULT = the creator asked for nothing (a plain attr). Anything
            // else is a choice: a class, or UNSPECIFIED, which is what a thread
            // created with an explicit sched priority reads — and the kernel
            // refuses a QoS on it (EPERM).
            qos_class_t q = qos_class_self();
            if (q != QOS_CLASS_DEFAULT)
                atomic_fetch_add(&g_ng_kept, 1);
            else if (pthread_set_qos_class_self_np(g_ng_qos, 0) == 0)
                atomic_fetch_add(&g_ng_raised, 1);
            else
                atomic_fetch_add(&g_ng_failed, 1);
        }
    }
    if (g_ng_prev) g_ng_prev(ev, t, addr, size);
}

static void macho_guest_qos_report(void *u) {
    (void)u;
    fprintf(stderr, "  [macho] guest threads after 60 s: %u raised to %s, %u kept the class or "
                    "sched priority their creator gave, %u refused\n",
            atomic_load(&g_ng_raised), macho_qos_name(g_ng_qos),
            atomic_load(&g_ng_kept), atomic_load(&g_ng_failed));
}

// Called by macho_run, BEFORE it creates the entry thread.
static void macho_guest_qos_install(void) {
    // Once per process: the game can run more than once (quit to the launcher,
    // Play again). A second pthread_introspection_hook_install would hand back
    // OUR hook as "previous", and the hook would call itself forever on the next
    // pthread_create.
    if (g_ng_on) return;
    g_ng_qos = macho_guest_qos();
    if (g_ng_qos == QOS_CLASS_UNSPECIFIED) {
        fprintf(stderr, "  [macho] guest threads keep the class their creator gives "
                        "(KL_GUEST_QOS=off; a plain attr = default)\n");
        return;
    }
    if (pthread_key_create(&g_ng_key, NULL) != 0) {
        fprintf(stderr, "  [macho] guest thread QoS: pthread_key_create failed — not installed\n");
        return;
    }
    g_ng_prev = pthread_introspection_hook_install(macho_guest_hook);
    g_ng_on = 1;
    fprintf(stderr, "  [macho] guest threads: every thread the guest starts (by lineage from "
                    "the entry thread, via pthread introspection) is raised to %s; the entry "
                    "thread (Render thread) is user-interactive\n", macho_qos_name(g_ng_qos));
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC),
                     dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL,
                     macho_guest_qos_report);
}

// The lineage root: called on the entry thread before it enters the guest.
static void macho_guest_thread_mark(void) {
    if (g_ng_on) pthread_setspecific(g_ng_key, (void *)1);
}

static void *macho_thread(void *u) {
    unsigned gen = (unsigned)(uintptr_t)u;
    pthread_setname_np("Klepton Native Guest");
    macho_guest_thread_mark();   // the lineage root: everything it starts is a guest thread
    char gd[1200];
    snprintf(gd, sizeof gd, "%s", g_macho_gamedir);
    if (chdir(gd) != 0) fprintf(stderr, "  [macho] chdir %s: %s\n", gd, strerror(errno));
    size_t st = pthread_get_stacksize_np(pthread_self());
    printf("\n=== entering the Mach-O guest (%zu KB stack, cwd %s) ===\n", st >> 10, gd);
    fflush(NULL);
    int rc = g_macho_main(g_macho_argc, g_macho_argv);
    printf("[macho] the guest's entry returned %d\n", rc);
    fflush(NULL);
    pthread_mutex_lock(&g_nrun_mu);
    int late = g_nrun_gen != gen || g_nrun_done != 0;
    pthread_mutex_unlock(&g_nrun_mu);
    if (late) {   // the run already ended (exit() or the quit grace); its isolate is parked
        macho_repair_stdout();
        printf("[macho] (run %u had already ended — through exit() or the quit grace — nothing more to do)\n", gen);
        fflush(NULL);
        return NULL;
    }
    g_macho_rc = rc;
    macho_isolate_teardown();
    pthread_mutex_lock(&g_nrun_mu);
    g_macho_teardown_busy = 0;
    // A grace cut that already ended this run (gen moved, or done set) must not
    // be overwritten — that store would land on the NEXT run.
    if (g_nrun_gen == gen && g_nrun_done == 0) {
        g_nrun_done = 1;
        pthread_cond_broadcast(&g_nrun_cv);
    }
    pthread_mutex_unlock(&g_nrun_mu);
    return NULL;
}

// The ownership gate, at the last call before the guest's main. macho_run is
// that call for the first Play and every re-run (the window path and the
// immersive guest thread both come here). Returning 1 does not start a thread
// and does not call klmc_main.
static int ownership_gate_refuses(void) {
    const char *tree = (g_target && g_target->tree && g_target->tree[0]) ? g_target->tree : "vivecraft";
    if (kl_ownership_allows_launch(tree)) return 0;
    printf("[gate] refused: this device has not been unlocked by a Microsoft account that owns Minecraft: Java Edition\n");
    fflush(NULL);
    return fail("ownership gate: sign in with an account that owns Minecraft: Java Edition");
}

// Blocks until the game returns. Called from the app's boot thread (window
// path) or the guest thread (immersive path) — never the main thread.
static int macho_run(void) {
    if (!g_macho_main) return fail("the game is not loaded");
    if (ownership_gate_refuses()) return 1;
    app_phase("macho run");
    unsigned mb = kl_env_uint("KL_NATIVE_STACK_MB", 8);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (size_t)mb << 20);
    // The entry thread runs klmc_main, which becomes Minecraft's "Render
    // thread" (the GL/OpenXR frame producer): user-interactive. Every thread it
    // starts gets KL_GUEST_QOS (user-initiated) through the hook installed here.
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    macho_guest_qos_install();
    pthread_t t;
    pthread_mutex_lock(&g_nrun_mu);
    g_flat_run_frame_base = kl_mono_frame_count();
    g_flat_size_sent = 0;
    g_flat_cursor_known = 0;
    g_nrun_active = 1;
    g_nrun_done = 0;
    g_nrun_closed = 0;
    unsigned gen = ++g_nrun_gen;
    g_macho_vm = NULL;   // the last run's VM is a parked isolate now; find this run's below
    pthread_mutex_unlock(&g_nrun_mu);
    unsigned grace_ms = kl_env_uint("KL_MC_QUIT_GRACE_MS", 2500);
    int rc = pthread_create(&t, &a, macho_thread, (void *)(uintptr_t)gen);
    pthread_attr_destroy(&a);
    if (rc) {
        pthread_mutex_lock(&g_nrun_mu); g_nrun_active = 0; pthread_mutex_unlock(&g_nrun_mu);
        return fail("could not create the game's entry thread");
    }
    // Until the run ends (macho_thread's return or the exit hook). Wakes once
    // a second meanwhile to find the image's JavaVM once the isolate exists —
    // the exit hook needs it to know which isolate is the live one.
    pthread_mutex_lock(&g_nrun_mu);
    while (!g_nrun_done) {
        if (!g_macho_vm) {
            pthread_mutex_unlock(&g_nrun_mu);
            void *vm = macho_find_live_vm();
            if (vm) g_macho_vm = vm;
            pthread_mutex_lock(&g_nrun_mu);
            if (g_nrun_done) break;
        }
        long wait_ms = 1000;
        // DestroyJavaVM runs on the entry thread after main returns, and it is
        // slower than the quit grace. Cutting here used to detach that thread,
        // return to the launcher, and then the next run dereferenced the VM
        // DestroyJavaVM was still unmapping. Wait the teardown out; it has its
        // own KL_MC_TEARDOWN_SECS bound and then sets g_nrun_done.
        if (g_nrun_closed && grace_ms && !g_macho_teardown_busy) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long el = (now.tv_sec - g_nrun_closed_at.tv_sec) * 1000 + (now.tv_nsec - g_nrun_closed_at.tv_nsec) / 1000000;
            if (el >= (long)grace_ms) {
                // Cut: park the isolate BEFORE the run is marked over, so its
                // watchdog's exit() finds it dead and is swallowed.
                void *iso = macho_live_isolate();
                macho_mark_dead(iso);
                g_macho_rc = 0;
                g_nrun_done = 3;
                printf("[macho] the game closed its window %.1f s ago and its main has not returned (run_main is still "
                       "joining a Java thread) — the run ends HERE: isolate %p parked, its entry thread left behind, the "
                       "launcher comes back (KL_MC_QUIT_GRACE_MS=%u; 0 = wait for the return)\n", el / 1e3, iso, grace_ms);
                break;
            }
            if ((long)grace_ms - el < wait_ms) wait_ms = (long)grace_ms - el;
        }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (wait_ms % 1000) * 1000000L;
        dl.tv_sec += wait_ms / 1000 + dl.tv_nsec / 1000000000L;
        dl.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&g_nrun_cv, &g_nrun_mu, &dl);
    }
    int how = g_nrun_done;
    g_nrun_active = 0;
    pthread_mutex_unlock(&g_nrun_mu);
    if (how == 3) { macho_dump_thread_names(); fflush(NULL); }
    if (how == 1) pthread_join(t, NULL);
    else pthread_detach(t);   // ended through exit() or the quit grace: the entry thread may never come back
    snprintf(g_status, sizeof g_status, "the game %s %d", how == 1 ? "returned" : how == 3 ? "closed (main still "
             "in shutdown), rc" : "exited with", g_macho_rc);
    __atomic_add_fetch(&g_macho_runs, 1, __ATOMIC_SEQ_CST);
    return 0;
}

// --- the launcher's side of quit-to-launcher (Minecraft/KleptonMCBoot.swift polls these) --
int kl_app_macho_runs(void) { return __atomic_load_n(&g_macho_runs, __ATOMIC_SEQ_CST); }
int kl_app_macho_rc(void) { return g_macho_rc; }
// Whether another Play can run in this process.
//
// KL_MC_RERUN is on unless set to 0, as in the reference tree: Quit Game hands
// the player back to the launcher, and its Play starts the game again in this
// process. That is the one place where this target leaves the tree's habit —
// every other game here boots once and leaves when its picture is gone, so this
// tree's compositor, controllers and picture settings had only ever seen one
// immersive space per process. What a second run needs is ported and was seen
// working in the simulator (the isolate's teardown, the exit hook, the rebuilt
// command line, kl_openxr's reset for a second instance, a second
// CompositorLayer closure with the start settings applied again); on a headset
// it is unseen. KL_MC_RERUN=0 is the way back to the tree's habit if it
// misbehaves there: the app then leaves when a game ends, and the next tap on
// its icon lands on the launcher.
//
// Memory: a parked isolate is not freed, and even a torn-down one
// leaves the process larger than it started. A run that would not fit is refused
// HERE, before it starts, and the launcher then takes the exit fallback instead
// of letting jetsam kill a game mid-play. KL_MC_RERUN_MIN_MB (default 4300,
// about the first run's peak) is the available memory a re-run needs; 0 = no
// check.
int kl_app_macho_rerun_ok(void) {
    if (!(target_is_macho() && g_macho_main && g_macho_rerun_ok && kl_env_on("KL_MC_RERUN", 1))) return 0;
    unsigned need = kl_env_uint("KL_MC_RERUN_MIN_MB", 4300);
    long long avail = kl_avail_memory();   // -1 = unknown (simulator, macOS)
    if (need && avail > 0 && avail < (long long)need << 20) {
        static int said;
        if (!said++)
            fprintf(stderr, "[macho] no re-run: %lld MB available, a game needs %u MB (KL_MC_RERUN_MIN_MB; the finished "
                            "games' isolates are still mapped) — the launcher exits the app instead\n",
                    avail >> 20, need);
        return 0;
    }
    return 1;
}

// The env file, again: anything pushed or written since the first boot. A name
// the file set last time is taken away first, so a REMOVED line really is
// removed, then the file is re-applied. (The reference put the target row's own
// value back for such a name; here the target's defaults are setenv(..., 0) in
// kl_driver_init, which runs once, so a name the file used to override and the
// target also defaults is restored by naming it there again — see below.)
static void macho_env_refresh(void) {
    char prev[sizeof g_env_file_applied];
    snprintf(prev, sizeof prev, "%s", g_env_file_applied);
    char *save = NULL;
    for (char *tok = strtok_r(prev, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        unsetenv(tok);
    }
    g_env_file_applied[0] = 0;
    macho_env_file_apply(g_macho_container);
    // The target's defaults again, for whatever the unsetenv above took away
    // (overwrite 0: a value the file or the launch environment set stands).
    kl_driver_init(g_target, g_libdir, g_door);
    printf("  [env-file] re-applied for this run: %s\n", g_env_file_applied[0] ? g_env_file_applied : "(none)");
}

// The next Play in the same process: everything run-specific is rebuilt — the
// env file, Vivecraft's vrEnabled, the whole command line (launch.txt,
// KL_MC_ARGS / KL_MC_JVM_ARGS, the launcher's KL_MC_LAUNCH_ARGV, the MSA
// session). The dylibs, ANGLE and the compositor's C side stay as boot left
// them; the isolate is new (run_main makes one per call).
int kl_app_macho_rerun(void) {
    if (!kl_app_macho_rerun_ok()) return fail("no re-run in this process (the isolate was not torn down)");
    pthread_mutex_lock(&g_guest.mu);
    int busy = g_guest.running || (g_guest.state == 1 && !g_guest.finished);
    pthread_mutex_unlock(&g_guest.mu);
    if (busy) return fail("the previous run has not ended yet");
    if (ownership_gate_refuses()) return 1;
    macho_repair_stdout();
    printf("\n=== launch %d in this process (the launcher's Play again) ===\n", kl_app_macho_runs() + 1);
    macho_env_refresh();
    macho_set_vr_config(macho_vr());
    for (int i = 0; i < g_macho_argc; i++) free(g_macho_argv[i]);
    free(g_macho_argv);
    g_macho_argv = NULL;
    g_macho_argc = 0;
    g_macho_rc = -1;
    if (macho_build_argv() != 0) return 1;
    snprintf(g_status, sizeof g_status, "the game is re-armed (launch %d)", kl_app_macho_runs() + 1);
    fflush(NULL);
    return 0;
}
