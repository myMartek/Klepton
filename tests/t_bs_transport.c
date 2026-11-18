// Bigscreen local-file player 4/6: transport and callbacks (2026-10-24).
// State machine over kl_avdec: play/pause/resume/stop/seek/volume + READY /
// PLAYING / PAUSED / ENDED events to an injected listener. EOS is real: the
// 1 s tiny clip runs out while playing.
// `make bs_transport`, in `make check`.
#include <stdio.h>
#include <unistd.h>

#include "../runtime/media/kl_bsplayer.h"

static int g_fail;
static kl_bsplayer_event g_evs[16];
static int g_nevs;

static void rec(void *ctx, kl_bsplayer_event ev) {
    (void)ctx;
    if (g_nevs < 16) g_evs[g_nevs++] = ev;
}

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

static int has(kl_bsplayer_event ev) {
    for (int i = 0; i < g_nevs; i++)
        if (g_evs[i] == ev) return 1;
    return 0;
}

int main(void) {
    printf("=== bigscreen transport ===\n");
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_tiny.mp4", reason, sizeof reason);
    ok(p != NULL, "tiny mp4 opens");
    if (!p) {
        printf("FAIL bigscreen transport\n");
        return 1;
    }
    kl_bsplayer_set_listener(p, rec, NULL);

    kl_bsplayer_play(p);
    ok(has(KL_BS_EV_READY) && has(KL_BS_EV_PLAYING), "play -> READY, PLAYING");

    kl_bsplayer_pause(p);
    ok(has(KL_BS_EV_PAUSED), "pause -> PAUSED");
    int n0 = g_nevs;
    for (int i = 0; i < 5; i++) kl_bsplayer_update(p);
    ok(g_nevs == n0, "paused update fires nothing");

    kl_bsplayer_resume(p);
    ok(g_evs[g_nevs - 1] == KL_BS_EV_PLAYING, "resume -> PLAYING");

    // Seek clamps synchronously (return = clamped target, ms).
    ok(kl_bsplayer_seek_to_us(p, -1) == 0, "seek(-1) clamps to 0");
    int w = 0, h = 0;
    const char *mime = NULL;
    long long dur = 0;
    kl_bsplayer_info(p, &w, &h, &mime, &dur);
    ok(dur > 0 && kl_bsplayer_seek_to_us(p, dur + 1000000) == dur,
       "seek(past end) clamps to dur");

    // Volume clamps, stored (no audible path on this host yet).
    kl_bsplayer_set_volume(p, 2.0f);
    ok(kl_bsplayer_volume(p) == 1.0f, "volume clamps to 1");
    kl_bsplayer_set_volume(p, -1.0f);
    ok(kl_bsplayer_volume(p) == 0.0f, "volume clamps to 0");

    // Stop: paused at 0, idempotent (one PAUSED total for both stops).
    int paused_before = 0;
    for (int i = 0; i < g_nevs; i++) paused_before += g_evs[i] == KL_BS_EV_PAUSED;
    kl_bsplayer_stop(p);
    kl_bsplayer_stop(p);
    int paused_after = 0;
    for (int i = 0; i < g_nevs; i++) paused_after += g_evs[i] == KL_BS_EV_PAUSED;
    ok(paused_after == paused_before + 1, "stop idempotent, one PAUSED");

    // EOS: play the 1 s clip out; ENDED arrives exactly once.
    g_nevs = 0;
    kl_bsplayer_play(p);
    int ended = 0;
    for (int i = 0; i < 200 && !ended; i++) {
        kl_bsplayer_update(p);
        for (int j = 0; j < g_nevs; j++) ended += g_evs[j] == KL_BS_EV_ENDED;
        if (!ended) usleep(50000);
    }
    ok(ended == 1, "EOS -> exactly one ENDED");
    int n1 = g_nevs;
    for (int i = 0; i < 5; i++) kl_bsplayer_update(p);
    ok(g_nevs == n1, "no duplicate ENDED");

    // Replay after the end restarts from the top without a seek: the full
    // 1 s clip plays again instead of ending instantly on the spent reader.
    g_nevs = 0;
    kl_bsplayer_play(p);
    int ended2 = 0, iters = 0;
    for (int i = 0; i < 200 && !ended2; i++) {
        kl_bsplayer_update(p);
        for (int j = 0; j < g_nevs; j++) ended2 += g_evs[j] == KL_BS_EV_ENDED;
        if (!ended2) {
            usleep(50000);
            iters++;
        }
    }
    ok(ended2 == 1 && iters > 5, "replay after EOS plays the full clip again");

    kl_bsplayer_close(p);
    printf("%s bigscreen transport\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
