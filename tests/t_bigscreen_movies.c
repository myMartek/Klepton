// Bigscreen x2497738113633933 local-files path (2026-10-24): the Quest keeps
// shared films in /sdcard/Movies and Bigscreen names that folder either as a
// literal or via Environment.getExternalStoragePublicDirectory(DIRECTORY_*).
// The helper below pins the guest-visible spelling: NULL -> /sdcard,
// "Movies" -> /sdcard/Movies, everything else -> /sdcard/<type>.
// `make bigscreen_movies`, in `make check`.
#include <stdio.h>
#include <string.h>
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

    printf("%s bigscreen movies path\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
