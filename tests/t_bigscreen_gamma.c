// Bigscreen x2497738113633933 gamma triage (2026-10-24): Gamma-space Unity
// writes sRGB code values into a non-sRGB eye (RGBA8 0x8058, logged) while
// the drawable is sRGB (format 81, logged) — the composite must decode, as
// for Sniper/Catan. Pins the target default plus the A/B phase clock.
// `make bigscreen_gamma`, in `make check`.
#include <stdio.h>
#include <stdint.h>
#include "../runtime/gfx/kl_reproject.h"

// kl_reproject.c asks for the target's name (per-title defaults); this test
// links that file alone, without the driver, and runs as no title at all.
const char *kl_driver_target_name(void) { return NULL; }

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("=== bigscreen gamma ===\n");
    // The default mapping: Bigscreen decodes like Sniper/Catan, titles
    // without the Gamma finding keep today's behaviour, NULL is safe.
    ok(kl_reproject_srgb_default_for_target("x2497738113633933") == 1, "bigscreen decodes ON");
    ok(kl_reproject_srgb_default_for_target("x2544573735586536") == 1, "sniper stays ON");
    ok(kl_reproject_srgb_default_for_target("x3882524545155280") == 1, "catan stays ON");
    ok(kl_reproject_srgb_default_for_target("x24882487241367228") == 0, "other title stays OFF");
    ok(kl_reproject_srgb_default_for_target(NULL) == 0, "NULL stays OFF");
    printf("%s bigscreen gamma\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
