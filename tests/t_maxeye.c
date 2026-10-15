// "Max resolution per eye" (kl_ovrp_set_eye_max_choice, KleptonEyeMax.swift):
// which cap the display's request is held to. The launch line's KL_OVRP_EYE_MAX
// first, then the player's choice for the game, then the title's own cap (4000
// for Gun Club VR), then the general 3072; Native leaves the request as it is.
// The render scale still applies after the cap, so "Resolution" stays a
// fraction of the chosen size. kl_ovrp_title_eye_cap answers by name before a
// guest runs. Each case is a child of its own, since a target is set once per
// process. `make maxeye`, in `make check`. No game content.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "klepton.h"
#include "guest/kl_driver.h"
#include "xr/kl_ovrp.h"

#define GUNCLUB "x2285803881494677"
#define OTHER "maxeye-test-other"

// What the guest is told for a 4633x3717 request (the display's own size).
static int eye(const char *target, int choice, const char *env, const char *rs, int want_w, int want_h) {
    static kl_target t;
    t.name = target;
    kl_driver_init(&t, NULL, 0);
    kl_driver_set_alarm(0);
    if (env) setenv("KL_OVRP_EYE_MAX", env, 1); else unsetenv("KL_OVRP_EYE_MAX");
    if (rs) setenv("KL_OVRP_RENDER_SCALE", rs, 1); else unsetenv("KL_OVRP_RENDER_SCALE");
    kl_ovrp_set_eye_max_choice(choice);
    kl_ovrp_set_eye_texture_size(4633, 3717);
    int w = 0, h = 0, fw = 0, fh = 0;
    float s = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    kl_ovrp_eye_texture_scale(&s, &fw, &fh);
    if (w != want_w || h != want_h) { fprintf(stderr, "  eye %dx%d, want %dx%d\n", w, h, want_w, want_h); return 1; }
    // MetalFX restores to the capped size, before the render scale.
    if (!rs && (fw != w || fh != h)) { fprintf(stderr, "  full %dx%d, want %dx%d\n", fw, fh, w, h); return 1; }
    return 0;
}

static int run(const char *what, const char *target, int choice, const char *env, const char *rs, int w, int h) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) _exit(eye(target, choice, env, rs, w, h));
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

int main(void) {
    printf("=== Max resolution per eye: launch line > choice > title > 3072 ===\n");
    int ok = 1;
    ok &= kl_ovrp_title_eye_cap(GUNCLUB) == 4000;
    ok &= kl_ovrp_title_eye_cap(OTHER) == 3072;
    ok &= kl_ovrp_title_eye_cap("cs1") == 2560;
    printf("%s title caps by name: Gun Club 4000, cs1 2560, other 3072\n", ok ? "ok  " : "FAIL");
    ok &= run("other title, default: 3072 (3072x2465)", OTHER, 0, NULL, NULL, 3072, 2465);
    ok &= run("Gun Club, default: its own 4000 (4000x3209)", GUNCLUB, 0, NULL, NULL, 4000, 3209);
    ok &= run("Gun Club, chose 2560: 2560x2054", GUNCLUB, 2560, NULL, NULL, 2560, 2054);
    ok &= run("other title, chose 4608: 4608x3697", OTHER, 4608, NULL, NULL, 4608, 3697);
    ok &= run("other title, Native: the whole request (4633x3717)", OTHER, KL_OVRP_EYE_NATIVE, NULL, NULL, 4633, 3717);
    ok &= run("Gun Club, chose 4608, KL_OVRP_EYE_MAX=3072: the launch line (3072x2465)", GUNCLUB, 4608, "3072", NULL, 3072, 2465);
    ok &= run("other title, Native, KL_OVRP_EYE_MAX=2048: the launch line (2048x1643)", OTHER, KL_OVRP_EYE_NATIVE, "2048", NULL, 2048, 1643);
    ok &= run("other title, chose 4000 at 80 %: 3200x2568", OTHER, 4000, NULL, "0.8", 3200, 2568);
    ok &= run("other title, a nonsense choice (-7): the default 3072", OTHER, -7, NULL, NULL, 3072, 2465);
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
