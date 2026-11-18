// Bigscreen local-file player 2/6: first-frame decode (2026-10-24).
// kl_avdec pumps BGRA frames on its own thread once playing; decode_first
// waits for the first published frame (serial != 0) and hands out a copy.
// `make bs_decode`, in `make check`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/media/kl_bsplayer.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("=== bigscreen decode ===\n");
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_tiny.mp4", reason, sizeof reason);
    ok(p != NULL, "tiny mp4 opens");
    if (p) {
        unsigned char *bgra = NULL;
        int w = 0, h = 0;
        ok(kl_bsplayer_decode_first(p, &bgra, &w, &h) == 0, "first frame decodes");
        ok(bgra != NULL && w == 160 && h == 90, "160x90 BGRA buffer");
        if (bgra) {
            // testsrc2 is colourful: some byte must be non-zero.
            size_t n = (size_t)w * h * 4, nz = 0;
            for (size_t i = 0; i < n; i += 997) nz += bgra[i] != 0;
            ok(nz > 0, "frame is not blank");
            free(bgra);
        }
        kl_bsplayer_close(p);
    }
    ok(kl_bsplayer_decode_first(NULL, NULL, NULL, NULL) == -1,
       "decode_first(NULL) -> -1");
    printf("%s bigscreen decode\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
