// Bigscreen local-file player 3/6: present to guest texture (2026-10-24).
// present() latcht den neuesten Decoder-Frame via injizierbarem Uploader in
// die Gast-Textur — ohne GL testbar: der Stub zählt Aufrufe, die echte
// SurfaceTexture-Anbindung kommt in Task 5.
// `make bs_present`, in `make check`.
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/media/kl_bsplayer.h"

static int g_fail;
static int g_uploads;
static int g_uw, g_uh;
static unsigned long long g_userial;

static int stub_upload(void *ctx, const unsigned char *bgra, int w, int h,
                       unsigned long long serial) {
    (void)ctx;
    (void)bgra;
    g_uploads++;
    g_uw = w;
    g_uh = h;
    g_userial = serial;
    return 0;
}

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("=== bigscreen present ===\n");
    ok(kl_bsplayer_present(NULL) == -1, "present(NULL) -> -1");

    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_tiny.mp4", reason, sizeof reason);
    ok(p != NULL, "tiny mp4 opens");
    if (p) {
        // Frische Session: noch kein Frame publiziert -> nichts zu latchen,
        // auch ohne Uploader.
        ok(kl_bsplayer_present(p) == 1, "fresh session -> 1 (no frame)");

        unsigned char *bgra = NULL;
        int w = 0, h = 0;
        ok(kl_bsplayer_decode_first(p, &bgra, &w, &h) == 0, "first frame decodes");
        free(bgra);

        // Neuer Frame da, aber kein Uploader gesetzt -> -1 (Fehlverdrahtung,
        // nie still schlucken).
        ok(kl_bsplayer_present(p) == -1, "no uploader -> -1");

        kl_bsplayer_set_uploader(p, stub_upload, NULL);
        g_uploads = 0;
        ok(kl_bsplayer_present(p) == 0, "new frame latches -> 0");
        ok(g_uploads == 1 && g_uw == 160 && g_uh == 90 && g_userial != 0,
           "uploader saw 160x90 + serial");
        ok(kl_bsplayer_present(p) == 1, "same frame again -> 1");
        ok(g_uploads == 1, "no duplicate upload");
        kl_bsplayer_close(p);
    }
    printf("%s bigscreen present\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
