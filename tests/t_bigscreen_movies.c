// Bigscreen x2497738113633933 local-files path (2026-10-24): the Quest keeps
// shared films in /sdcard/Movies and Bigscreen names that folder either as a
// literal or via Environment.getExternalStoragePublicDirectory(DIRECTORY_*).
// The helper below pins the guest-visible spelling: NULL -> /sdcard,
// "Movies" -> /sdcard/Movies, everything else -> /sdcard/<type>.
// `make bigscreen_movies`, in `make check`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../runtime/jni/kl_jni_publicdir.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("=== bigscreen movies path ===\n");
    char out[1024];
    kl_public_dir_for_type(NULL, out, sizeof out);
    ok(strcmp(out, "/sdcard") == 0, "NULL -> /sdcard");
    kl_public_dir_for_type("", out, sizeof out);
    ok(strcmp(out, "/sdcard") == 0, "empty -> /sdcard");
    kl_public_dir_for_type("Movies", out, sizeof out);
    ok(strcmp(out, "/sdcard/Movies") == 0, "Movies -> /sdcard/Movies");
    kl_public_dir_for_type("DCIM", out, sizeof out);
    ok(strcmp(out, "/sdcard/DCIM") == 0, "DCIM -> /sdcard/DCIM");
    // Never overflows the caller buffer: truncation still terminates.
    char tiny[8];
    kl_public_dir_for_type("Movies", tiny, sizeof tiny);
    ok(tiny[sizeof tiny - 1] == '\0', "short buffer stays terminated");

    // Delete-flag: devicectl cannot delete, so the app honors a
    // __delete__.txt list (one film name per line) inside the Movies dir.
    char dir[] = "/tmp/bs-movies-test-XXXXXX";
    ok(mkdtemp(dir) != NULL, "scratch dir");
    if (dir[0]) {
        char f1[256], f2[256], flag[256], sub[256];
        snprintf(f1, sizeof f1, "%s/a.mp4", dir);
        snprintf(f2, sizeof f2, "%s/keep.mp4", dir);
        snprintf(flag, sizeof flag, "%s/__delete__.txt", dir);
        FILE *f = fopen(f1, "w"); if (f) { fputs("x", f); fclose(f); }
        f = fopen(f2, "w"); if (f) { fputs("x", f); fclose(f); }
        f = fopen(flag, "w");
        if (f) {
            fputs("a.mp4\n", f);
            fputs("missing.mp4\n", f);
            fputs("../evil.mp4\n", f);
            fputs("__delete__.txt\n", f);
            fputs("\n", f);
            fclose(f);
        }
        snprintf(sub, sizeof sub, "%s/sub", dir);
        mkdir(sub, 0700);
        int n = kl_movies_process_delete_flag(dir);
        ok(n == 1, "one film deleted, rest refused");
        ok(access(f1, F_OK) != 0, "a.mp4 gone");
        ok(access(f2, F_OK) == 0, "keep.mp4 stays");
        ok(access(flag, F_OK) != 0, "flag consumed");
        ok(access(sub, F_OK) == 0, "subdir untouched");
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        system(cmd);
    }
    // No flag, no work, no crash.
    ok(kl_movies_process_delete_flag("/tmp/bs-movies-test-nope") == 0,
       "missing flag is a no-op");
    printf("%s bigscreen movies path\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
