// Bigscreen local-file player: film audio (TDD RED).
// The demuxer decodes PCM but nothing plays it yet (volume is stored only).
// This pins the audio seam: facts, PCM flow through an injected sink, volume
// gain, seek reset, pause stop, and the A/V pairing across a sink stall (the
// system UI: the device stops taking audio while the picture keeps its wall
// clock). The stall half needs a clip longer than the stall:
//   ffmpeg -f lavfi -i "testsrc2=size=160x90:rate=10:duration=4" -f lavfi -i \
//     "sine=frequency=440:duration=4" -c:v libx264 -pix_fmt yuv420p -c:a aac \
//     -shortest tests/data/bs_avsync.mp4
// `make bs_aplay`, in `make check`.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/media/kl_bsplayer.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

// Fake sink: captures everything, paces nothing.
typedef struct {
    unsigned char buf[1 << 20];
    size_t n;
    size_t writes;
    int opened_rate, opened_ch;
    int played, paused;
    int flushed, closed;
} fake_sink;

static int fs_open(void *ctx, unsigned rate, unsigned ch) {
    fake_sink *f = ctx;
    f->opened_rate = (int)rate;
    f->opened_ch = (int)ch;
    return 0;
}
static size_t fs_write(void *ctx, const void *pcm, size_t bytes) {
    fake_sink *f = ctx;
    size_t room = sizeof f->buf - f->n;
    size_t n = bytes < room ? bytes : room;
    memcpy(f->buf + f->n, pcm, n);
    f->n += n;
    f->writes++;
    return n;
}
static void fs_play(void *ctx) { ((fake_sink *)ctx)->played++; }
static void fs_pause(void *ctx) { ((fake_sink *)ctx)->paused++; }
static void fs_flush(void *ctx) { ((fake_sink *)ctx)->flushed++; }
static void fs_close(void *ctx) { ((fake_sink *)ctx)->closed++; }

static long peak_abs(const fake_sink *f) {
    long peak = 0;
    const short *s = (const short *)f->buf;
    for (size_t i = 0; i < f->n / 2; i++) {
        long v = s[i] < 0 ? -(long)s[i] : (long)s[i];
        if (v > peak) peak = v;
    }
    return peak;
}

static long nonzero_bytes(const fake_sink *f) {
    long nz = 0;
    for (size_t i = 0; i < f->n; i++)
        if (f->buf[i]) nz++;
    return nz;
}

// A DEVICE-PACED sink: write() spends the chunk's own duration in real time,
// the way the render callback does, and takes nothing at all while `refuse` is
// set — which is what kl_audio does while the scene is silenced: the unit is
// stopped and every write comes back short. The unthrottled fake_sink above
// cannot show this bug, because a sink that takes everything instantly hides
// every question about who is pacing whom.
typedef struct {
    unsigned rate, frame_bytes;
    int refuse, opened, flushed;
    int stall_ms;          // one write blocks this long (a suspended pump, slow IO)
} paced_sink;

static int ps_open(void *ctx, unsigned rate, unsigned ch) {
    paced_sink *f = ctx;
    f->rate = rate;
    f->frame_bytes = (ch ? ch : 1) * 2;   // s16 interleaved
    f->opened = 1;
    return 0;
}
static size_t ps_write(void *ctx, const void *pcm, size_t bytes) {
    (void)pcm;
    paced_sink *f = ctx;
    if (f->refuse) return 0;
    if (f->stall_ms) {
        int ms = f->stall_ms;
        f->stall_ms = 0;
        usleep((useconds_t)ms * 1000);
    }
    size_t frames = f->frame_bytes ? bytes / f->frame_bytes : 0;
    if (frames && f->rate) usleep((useconds_t)(frames * 1000000ull / f->rate));
    return bytes;
}
static void ps_play(void *ctx) { (void)ctx; }
static void ps_pause(void *ctx) { (void)ctx; }
static void ps_flush(void *ctx) { ((paced_sink *)ctx)->flushed++; }
static void ps_close(void *ctx) { (void)ctx; }

// The system UI, from the film's point of view: the picture keeps its wall
// clock (kl_avdec paces absolutely), the device stops taking sound, and when it
// comes back the sound must be where the picture is — not 20 s ahead of it.
// Before the fix the pump dropped every refused chunk and read the next, so the
// demuxer's audio cursor ran through the film at several times real time.
static void avsync_across_a_sink_stall(void) {
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_avsync.mp4", reason,
                                      sizeof reason);
    ok(p != NULL, "avsync clip opens");
    if (!p) return;

    static paced_sink ps;
    memset(&ps, 0, sizeof ps);
    static const kl_bsplayer_audio_sink sink = {
        ps_open, ps_write, ps_play, ps_pause, ps_flush, ps_close,
    };
    kl_bsplayer_set_audio_sink(p, &sink, &ps);

    kl_bsplayer_play(p);
    usleep(600000);                       // ~0.6 s of real playback
    ok(ps.opened, "paced sink opened");
    long long a0 = kl_bsplayer_audio_position_us(p);

    ps.refuse = 1;                        // the scene goes inactive
    usleep(1200000);                      // 1.2 s in the system UI
    long long a1 = kl_bsplayer_audio_position_us(p);
    long long held = a1 - a0;
    printf("  sound cursor moved %+lld ms while the device took nothing\n",
           held / 1000);
    ok(held >= 0 && held < 250000,
       "the sound cursor holds while the device takes nothing");

    ps.refuse = 0;                        // back: the device takes audio again
    usleep(700000);
    long long v = kl_bsplayer_position_ms(p);
    long long a = kl_bsplayer_audio_position_us(p);
    long long off_us = a - v * 1000;
    printf("  picture %.3f s, sound %.3f s, delta %+lld ms, %lu realign(s)\n",
           (double)v / 1000.0, (double)a / 1e6, off_us / 1000,
           kl_bsplayer_av_realigns(p));
    ok(v > 1200, "picture kept running while the scene was inactive");
    ok(kl_bsplayer_av_realigns(p) >= 1, "the stall was realigned");
    ok(off_us < 400000 && off_us > -400000,
       "sound is back on the picture (|delta| < 400 ms)");

    kl_bsplayer_close(p);
}

// A pump that was AWAY: the process was suspended (or the file stalled) and no
// write was ever refused — so nothing marks the stall but the gap between two
// iterations. The picture keeps its wall clock across it, so the sound comes
// back that far behind and must be put back.
static void avsync_across_a_pump_gap(void) {
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_avsync.mp4", reason,
                                      sizeof reason);
    ok(p != NULL, "gap clip opens");
    if (!p) return;

    static paced_sink ps;
    memset(&ps, 0, sizeof ps);
    static const kl_bsplayer_audio_sink sink = {
        ps_open, ps_write, ps_play, ps_pause, ps_flush, ps_close,
    };
    kl_bsplayer_set_audio_sink(p, &sink, &ps);

    kl_bsplayer_play(p);
    usleep(500000);
    ps.stall_ms = 1200;                   // one write blocks: the pump was gone
    usleep(2600000);

    long long v = kl_bsplayer_position_ms(p);
    long long a = kl_bsplayer_audio_position_us(p);
    long long off_us = a - v * 1000;
    printf("  after a 1.2 s gap: picture %.3f s, sound %.3f s, delta %+lld ms, "
           "%lu realign(s)\n",
           (double)v / 1000.0, (double)a / 1e6, off_us / 1000,
           kl_bsplayer_av_realigns(p));
    ok(kl_bsplayer_av_realigns(p) >= 1, "a pump gap arms the realign");
    ok(off_us < 400000 && off_us > -400000,
       "sound is back on the picture after a gap (|delta| < 400 ms)");

    kl_bsplayer_close(p);
}

// One seek per interruption, not one every two seconds. While the device takes
// nothing there is nothing to hear, so realigning is pure file churn: hold, and
// realign ONCE when it comes back.
static void avsync_realigns_once_per_silence(void) {
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_avsync.mp4", reason,
                                      sizeof reason);
    ok(p != NULL, "silence clip opens");
    if (!p) return;

    static paced_sink ps;
    memset(&ps, 0, sizeof ps);
    static const kl_bsplayer_audio_sink sink = {
        ps_open, ps_write, ps_play, ps_pause, ps_flush, ps_close,
    };
    kl_bsplayer_set_audio_sink(p, &sink, &ps);

    kl_bsplayer_play(p);
    usleep(500000);
    ps.refuse = 1;
    usleep(3000000);                      // 3 s with the device taking nothing
    unsigned long during = kl_bsplayer_av_realigns(p);
    ps.refuse = 0;
    usleep(700000);
    unsigned long after = kl_bsplayer_av_realigns(p);
    printf("  realigns: %lu during the silence, %lu after it\n", during, after);
    ok(during == 0, "no realign while the device takes nothing");
    ok(after == 1, "exactly one realign when the device comes back");

    kl_bsplayer_close(p);
}

int main(void) {
    printf("=== bigscreen film audio ===\n");
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_tiny.mp4", reason, sizeof reason);
    ok(p != NULL, "tiny mp4 opens");
    if (!p) {
        printf("FAIL bigscreen film audio\n");
        return 1;
    }

    int rate = 0, ch = 0;
    ok(kl_bsplayer_audio_info(p, &rate, &ch) == 1, "tiny clip has sound");
    ok(rate == 44100 && ch == 1, "facts 44100 Hz mono");

    static const kl_bsplayer_audio_sink fs = {
        fs_open, fs_write, fs_play, fs_pause, fs_flush, fs_close,
    };
    static fake_sink f;
    kl_bsplayer_set_audio_sink(p, &fs, &f);

    kl_bsplayer_play(p);
    usleep(300000);
    kl_bsplayer_pause(p);
    ok(f.opened_rate == 44100 && f.opened_ch == 1, "sink opened with film facts");
    ok(f.n > 0, "pcm flowed to the sink");
    ok(nonzero_bytes(&f) > (long)(f.n / 4), "pcm is real sound, not silence");
    ok(f.played >= 1 && f.paused >= 1, "play/pause reached the sink");
    long full_peak = peak_abs(&f);

    // Half volume must halve the amplitude: gain is applied in the pump.
    memset(&f, 0, sizeof f);
    kl_bsplayer_set_volume(p, 0.5f);
    kl_bsplayer_seek_to_us(p, 0);
    kl_bsplayer_play(p);
    usleep(300000);
    kl_bsplayer_pause(p);
    long half_peak = peak_abs(&f);
    ok(f.n > 0, "pcm flows at half volume");
    ok(half_peak < full_peak * 6 / 10 && half_peak > full_peak * 4 / 10,
       "half volume halves the peak");

    // Seek rewinds the audio cursor: same prefix, same bytes.
    memset(&f, 0, sizeof f);
    kl_bsplayer_seek_to_us(p, 0);
    kl_bsplayer_play(p);
    usleep(200000);
    kl_bsplayer_pause(p);
    size_t n1 = f.n;
    int fl = f.flushed;
    kl_bsplayer_seek_to_us(p, 0);
    ok(f.flushed > fl, "seek flushed the sink");
    memset(&f, 0, sizeof f);
    kl_bsplayer_play(p);
    usleep(200000);
    kl_bsplayer_pause(p);
    ok(n1 > 0 && f.n > 0, "pcm flows after seek");

    kl_bsplayer_close(p);
    ok(f.closed >= 1, "close released the sink");

    avsync_across_a_sink_stall();
    avsync_across_a_pump_gap();
    avsync_realigns_once_per_silence();

    if (g_fail) {
        printf("FAIL bigscreen film audio\n");
        return 1;
    }
    printf("ok bigscreen film audio\n");
    return 0;
}
