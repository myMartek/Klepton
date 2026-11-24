// Folders from the Files app inside external storage (kl_guest_dir_aliases_set,
// runtime/libc/kl_libc.c): Bigscreen Beta's films from "On My Apple Vision Pro"
// and iCloud Drive. Against temporary folders, no game data. `make bsfolders`,
// in `make check`.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "klepton.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-60s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

static void touch(const char *p) {
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

int main(void) {
    printf("=== folders from the Files app ===\n");
    char root[] = "/tmp/kl_bsfolders.XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char ext[512], icloud[512], p[1024], buf[1024];
    snprintf(ext, sizeof ext, "%s/android-files", root);
    snprintf(icloud, sizeof icloud, "%s/Mobile Documents/Films", root);
    snprintf(p, sizeof p, "%s/Movies/iCloud Drive", ext);
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s/Sub'", p, icloud);
    if (system(cmd) != 0) return 1;
    snprintf(p, sizeof p, "%s/Film.mp4", icloud);
    touch(p);
    snprintf(p, sizeof p, "%s/Sub/Other.MP4", icloud);
    touch(p);
    kl_guest_extstorage_map(ext);

    // No list: the stand-in folder is just a folder in the container.
    const char *r = kl_guest_path("/storage/emulated/0/Movies/iCloud Drive/Film.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Movies/iCloud Drive/Film.mp4", ext);
    ok(r && strcmp(r, p) == 0, "no list: path stays in the container");

    char spec[1200];
    snprintf(spec, sizeof spec, "/Movies/iCloud Drive\t%s\n", icloud);
    kl_guest_dir_aliases_set(spec);

    r = kl_guest_path("/storage/emulated/0/Movies/iCloud Drive/Film.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Film.mp4", icloud);
    ok(r && strcmp(r, p) == 0, "film below the folder -> picked folder");

    r = kl_guest_path("/sdcard/Movies/iCloud Drive", buf, sizeof buf);
    ok(r && strcmp(r, icloud) == 0, "the folder itself (/sdcard spelling)");

    // The headset's volume is case-sensitive; a Mac's usually is not, and there
    // the wrong spelling is found as it is, so only the result can be checked.
    r = kl_guest_path("/storage/emulated/0/Movies/iCloud Drive/sub/other.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/sub/other.mp4", icloud);
    struct stat st;
    int folds = stat(p, &st) == 0;
    snprintf(p, sizeof p, "%s/Sub/Other.MP4", icloud);
    if (folds) ok(r && stat(r, &st) == 0, "other case below the picked folder is found");
    else ok(r && strcmp(r, p) == 0, "case fixed below the picked folder");

    r = kl_guest_path("/storage/emulated/0/Movies/iCloud DriveX/Film.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Movies/iCloud DriveX/Film.mp4", ext);
    ok(r && strcmp(r, p) == 0, "a longer name is not the folder");

    r = kl_guest_path("/storage/emulated/0/Movies/Own.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Movies/Own.mp4", ext);
    ok(r && strcmp(r, p) == 0, "the game's own films stay in the container");

    r = kl_guest_path("/storage/emulated/0/Movies/iCloud Drive/Film.mp4", buf, sizeof buf);
    ok(r && stat(r, &st) == 0 && S_ISREG(st.st_mode), "the film is readable through the alias");

    // The listing goes through the same resolver.
    r = kl_guest_path("/storage/emulated/0/Movies/iCloud Drive", buf, sizeof buf);
    DIR *d = r ? opendir(r) : NULL;
    int film = 0, sub = 0;
    for (struct dirent *e; d && (e = readdir(d)) != NULL; ) {
        if (!strcmp(e->d_name, "Film.mp4")) film = 1;
        if (!strcmp(e->d_name, "Sub")) sub = 1;
    }
    if (d) closedir(d);
    ok(film && sub, "the folder lists the picked folder's contents");

    // Malformed lines are skipped, the good one kept.
    snprintf(spec, sizeof spec, "Movies/rel\t/x\n/Movies/A\trelative\n/Movies/B\t%s", icloud);
    kl_guest_dir_aliases_set(spec);
    r = kl_guest_path("/sdcard/Movies/B/Film.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Film.mp4", icloud);
    ok(r && strcmp(r, p) == 0, "last line without newline is taken");
    r = kl_guest_path("/sdcard/Movies/A/Film.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Movies/A/Film.mp4", ext);
    ok(r && strcmp(r, p) == 0, "a relative host path is refused");

    kl_guest_dir_aliases_set(NULL);
    r = kl_guest_path("/sdcard/Movies/B/Film.mp4", buf, sizeof buf);
    snprintf(p, sizeof p, "%s/Movies/B/Film.mp4", ext);
    ok(r && strcmp(r, p) == 0, "cleared: back in the container");

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) {}
    printf("%s folders from the Files app\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
