// See kl_bsplayer.h.
#include "kl_bsplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "kl_avdec.h"
#include "kl_env.h"

struct kl_bsplayer {
    kl_avdec *dec;
    kl_avdemux *dm;
    kl_bsplayer_upload_fn upload;
    void *uctx;
    unsigned long long presented;   // serial of the last latched frame
    kl_bsplayer_listener_fn listener;
    void *lctx;
    int playing;
    int ready_fired;
    int ended_fired;
    float volume;
    // Film sound: a background thread pumps PCM out of the demuxer's audio
    // cursor into the sink (device-paced writes, like kl_opensl.c's feeder).
    // The demuxer cursor, the gain and the transport flag share amu: seek and
    // reads never tear, and the blocking sink.write runs unlocked.
    pthread_t athr;
    int arun;                       // thread lives
    pthread_mutex_t amu;
    kl_bsplayer_audio_sink sink;
    void *sctx;
    int sink_open;                  // sink.open answered 0 this play
    // The A/V pair. The picture lives on the wall clock (kl_avdec paces
    // absolutely); the sound lives only on what the sink accepts, so any state
    // that stops the sink moves the sound relative to the picture and nothing
    // moves it back. realign_armed is set by a write the device refused, and
    // the pump then puts the sound where the picture is (see bs_audio_realign).
    int realign_armed;
    unsigned long realigns;         // how many times that was needed
};

static void kl_bsplayer_fire(kl_bsplayer *p, kl_bsplayer_event ev) {
    if (p->listener) p->listener(p->lctx, ev);
}

kl_bsplayer *kl_bsplayer_open(const char *path, char *reason, size_t cap) {
    if (!path || !*path) {
        if (reason && cap) snprintf(reason, cap, "empty path");
        return NULL;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        if (reason && cap) snprintf(reason, cap, "no such file: %s", path);
        return NULL;
    }
    kl_avdemux *dm = kl_avdemux_open(path, 0, 0);
    kl_avdec *dec = dm ? kl_avdec_open(path, 0, 0) : NULL;
    if (!dm || !dec) {
        if (dec) kl_avdec_close(dec);
        if (dm) kl_avdemux_close(dm);
        if (reason && cap)
            snprintf(reason, cap, "unsupported container or no video track: %s", path);
        return NULL;
    }
    kl_bsplayer *p = calloc(1, sizeof *p);
    if (!p) {
        kl_avdec_close(dec);
        kl_avdemux_close(dm);
        if (reason && cap) snprintf(reason, cap, "out of memory");
        return NULL;
    }
    p->dec = dec;
    p->dm = dm;
    p->volume = 1.0f;
    pthread_mutex_init(&p->amu, NULL);
    return p;
}

int kl_bsplayer_info(kl_bsplayer *p, int *w, int *h, const char **mime,
                     long long *dur_us) {
    if (!p) return -1;
    float fps = 0;
    return kl_avdemux_info(p->dm, w, h, mime, dur_us, &fps) ? 0 : -1;
}

void kl_bsplayer_close(kl_bsplayer *p) {
    if (!p) return;
    // Park the audio thread first: it owns sink.close and must not touch
    // the demuxer past this point.
    if (p->arun) {
        pthread_mutex_lock(&p->amu);
        p->arun = 0;
        pthread_mutex_unlock(&p->amu);
        pthread_join(p->athr, NULL);
    }
    kl_avdec_close(p->dec);
    kl_avdemux_close(p->dm);
    pthread_mutex_destroy(&p->amu);
    free(p);
}

void kl_bsplayer_set_uploader(kl_bsplayer *p, kl_bsplayer_upload_fn fn,
                              void *ctx) {
    if (!p) return;
    p->upload = fn;
    p->uctx = ctx;
}

void kl_bsplayer_set_listener(kl_bsplayer *p, kl_bsplayer_listener_fn fn,
                              void *ctx) {
    if (!p) return;
    p->listener = fn;
    p->lctx = ctx;
}

int kl_bsplayer_audio_info(kl_bsplayer *p, int *rate, int *channels) {
    if (!p) return 0;
    int r = 0, c = 0;
    int has = kl_avdemux_audio_info(p->dm, &r, &c);
    if (rate) *rate = r;
    if (channels) *channels = c;
    return has;
}

void kl_bsplayer_set_audio_sink(kl_bsplayer *p,
                                const kl_bsplayer_audio_sink *sink, void *ctx) {
    if (!p) return;
    pthread_mutex_lock(&p->amu);
    if (sink) p->sink = *sink;
    else memset(&p->sink, 0, sizeof p->sink);
    p->sctx = ctx;
    pthread_mutex_unlock(&p->amu);
}

// 1024 PCM frames per chunk (the demuxer's unit), 16-bit, at most stereo.
#define BS_Aframes 1024
#define BS_Acap (BS_Aframes * 2 * 2)
// A refused write is waited out, not dropped. 20 ms is one device IO buffer on
// this host; the cap is a device that is gone for good (film paused from the
// guest, the unit torn down) and must not wedge this thread forever.
#define BS_Ahold_ms 20
#define BS_Ahold_max_ms 2000
// A gap between two pump iterations longer than this is its own stall signal:
// a suspended process (visionOS stops the thread with no error and no refused
// write) and a slow read both look like this, and the picture moved on.
#define BS_Agap_ms 1000

// Monotonic milliseconds, for nothing but the gap above.
static long long bs_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Put the sound back on the picture. The picture is the master — it paces on
// the wall clock and nothing the sound does can move it — so the sound is what
// travels. One line per event: the count says whether the run was clean, the
// delta says how far the two clocks had parted.
static void bs_audio_realign(kl_bsplayer *p, long long pic_ms, long long aud_us,
                             int thr_ms) {
    pthread_mutex_lock(&p->amu);
    kl_avdemux_audio_seek_us(p->dm, pic_ms * 1000);
    kl_bsplayer_audio_sink sink = p->sink;
    void *sctx = p->sctx;
    int open = p->sink_open;
    unsigned long n = ++p->realigns;
    p->realign_armed = 0;
    pthread_mutex_unlock(&p->amu);
    if (sink.flush && open) sink.flush(sctx);
    if (aud_us < 0)
        fprintf(stderr, "  [bs] film realign: sound had run out -> picture %.3f s "
                        "(allowed ±%d ms, %lu this run)\n",
                (double)pic_ms / 1000.0, thr_ms, n);
    else
        fprintf(stderr, "  [bs] film realign: sound %+.3f s from the picture -> %.3f s "
                        "(allowed ±%d ms, %lu this run)\n",
                (double)(aud_us - pic_ms * 1000) / 1e6,
                (double)pic_ms / 1000.0, thr_ms, n);
}

static void *bs_audio_thread(void *arg) {
    kl_bsplayer *p = arg;
    int rate = 0, ch = 0;
    pthread_mutex_lock(&p->amu);
    int has = kl_avdemux_audio_info(p->dm, &rate, &ch);
    kl_bsplayer_audio_sink sink = p->sink;
    void *sctx = p->sctx;
    pthread_mutex_unlock(&p->amu);
    if (!has || !sink.open || !sink.write) return NULL;   // silent clip/host
    if (rate <= 0) rate = 44100;
    if (ch <= 0 || ch > 2) ch = 2;
    int rc = sink.open(sctx, (unsigned)rate, (unsigned)ch);
    pthread_mutex_lock(&p->amu);
    p->sink_open = rc == 0;
    pthread_mutex_unlock(&p->amu);
    if (rc != 0) return NULL;
    if (sink.play) sink.play(sctx);
    // How far apart the pair may drift before the sound is pulled back onto the
    // picture. KL_BS_AVSYNC=0 restores the old drop-and-read-on for an A/B on
    // the device; the threshold is otherwise KL_BS_AVSYNC_MS (default 400 ms).
    int avsync = kl_env_on("KL_BS_AVSYNC", 1);
    int thr_ms = avsync ? kl_env_int("KL_BS_AVSYNC_MS", 400) : 0;
    static unsigned char buf[BS_Acap];
    int refusing = 0;          // the sink is taking nothing at this instant
    long long prev_ms = 0;
    for (;;) {
        long long now_ms = bs_now_ms();
        if (prev_ms && now_ms - prev_ms > BS_Agap_ms) {
            pthread_mutex_lock(&p->amu);
            p->realign_armed = 1;
            pthread_mutex_unlock(&p->amu);
        }
        prev_ms = now_ms;
        pthread_mutex_lock(&p->amu);
        int run = p->arun, playing = p->playing, armed = p->realign_armed;
        float vol = p->volume;
        pthread_mutex_unlock(&p->amu);
        if (!run) break;
        if (!playing) {
            usleep(10000);
            continue;
        }
        // BEFORE the read, so a cursor that already ran to the end of the
        // track (the pre-fix failure mode) still finds its way back. Not while
        // the device is still taking nothing: there is no one to hear the
        // correction, and seeking every two seconds only churns the file. It
        // fires once, on the iteration after the sink accepts again.
        if (armed && !refusing && thr_ms > 0) {
            long long pic = kl_avdec_position_ms(p->dec);
            pthread_mutex_lock(&p->amu);
            long long aud = kl_avdemux_audio_time_us(p->dm);
            pthread_mutex_unlock(&p->amu);
            if (pic > 0) {
                long long off = aud - pic * 1000;
                long long thr = (long long)thr_ms * 1000;
                if (aud < 0 || off > thr || off < -thr) {
                    bs_audio_realign(p, pic, aud, thr_ms);
                } else {
                    pthread_mutex_lock(&p->amu);
                    p->realign_armed = 0;
                    pthread_mutex_unlock(&p->amu);
                }
            }
        }
        pthread_mutex_lock(&p->amu);
        long long pts = kl_avdemux_audio_time_us(p->dm);
        long n = pts < 0 ? -1
                         : kl_avdemux_audio_read(p->dm, buf, sizeof buf);
        if (n > 0) kl_avdemux_audio_advance(p->dm);
        pthread_mutex_unlock(&p->amu);
        if (n <= 0) {   // end of sound: idle until seek/replay/close
            usleep(100000);
            continue;
        }
        // Volume gain in place (s16, clamped); the sink sees final samples.
        short *s = (short *)buf;
        for (long i = 0; i < n / 2; i++) {
            long v = (long)(s[i] * vol);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            s[i] = (short)v;
        }
        size_t off = 0;
        int held = 0;
        while (off < (size_t)n) {
            size_t w = sink.write(sctx, buf + off, (size_t)n - off);
            if (w) {
                refusing = 0;
                off += w;
                continue;
            }
            refusing = 1;
            // The device took nothing: the scene is silenced, the unit is
            // mid-rebuild, or the ring is simply full. Three states this pump
            // cannot tell apart and does not need to — what it must NOT do is
            // read on. Dropping the chunk and fetching the next is what turned
            // a visit to the system UI into a permanent A/V offset: the sound
            // ran through the film at several times real time while the
            // picture kept its wall clock. Hold, and let the check above put
            // the pair back together if the picture has moved meanwhile.
            if (!avsync) break;   // KL_BS_AVSYNC=0: the old behaviour, for A/B
            pthread_mutex_lock(&p->amu);
            p->realign_armed = 1;
            pthread_mutex_unlock(&p->amu);
            usleep(BS_Ahold_ms * 1000);
            held += BS_Ahold_ms;
            if (held >= BS_Ahold_max_ms) break;   // gone for good: this chunk is
        }                                        // lost, the next one retries
    }
    if (sink.close) sink.close(sctx);
    pthread_mutex_lock(&p->amu);
    p->sink_open = 0;
    pthread_mutex_unlock(&p->amu);
    return NULL;
}

// Start the pump thread once (first play); later plays just unpark it via
// p->playing. Silent clips and sink-less hosts skip the thread entirely.
static void bs_audio_start(kl_bsplayer *p) {
    pthread_mutex_lock(&p->amu);
    int live = p->arun;
    if (!live) p->arun = 1;
    pthread_mutex_unlock(&p->amu);
    if (live) return;
    if (pthread_create(&p->athr, NULL, bs_audio_thread, p) != 0) {
        pthread_mutex_lock(&p->amu);
        p->arun = 0;
        pthread_mutex_unlock(&p->amu);
    }
}

// Audio cursor follows the picture; the sink drops whatever it still holds.
static void bs_audio_seek(kl_bsplayer *p, long long us) {
    if (!p) return;
    pthread_mutex_lock(&p->amu);
    kl_avdemux_audio_seek_us(p->dm, us);
    p->realign_armed = 0;       // a deliberate seek IS the alignment
    kl_bsplayer_audio_sink sink = p->sink;
    void *sctx = p->sctx;
    int open = p->sink_open;
    pthread_mutex_unlock(&p->amu);
    if (sink.flush && open) sink.flush(sctx);
}

static void bs_audio_rewind(kl_bsplayer *p) {
    bs_audio_seek(p, 0);
}

void kl_bsplayer_play(kl_bsplayer *p) {
    if (!p || p->playing) return;
    if (kl_avdec_complete(p->dec)) {
        // Replay after the end restarts from the top: the old reader is
        // spent (its next sample is permanently EOS), so rewind first.
        kl_avdec_seek(p->dec, 0);
        bs_audio_seek(p, 0);
    }
    kl_avdec_play(p->dec, 1);
    p->playing = 1;
    p->ended_fired = 0;
    bs_audio_start(p);   // idempotent: parks until the first play
    if (p->sink.play && p->sink_open) p->sink.play(p->sctx);
    if (!p->ready_fired) {
        p->ready_fired = 1;
        kl_bsplayer_fire(p, KL_BS_EV_READY);
    }
    kl_bsplayer_fire(p, KL_BS_EV_PLAYING);
}

void kl_bsplayer_pause(kl_bsplayer *p) {
    if (!p || !p->playing) return;
    kl_avdec_play(p->dec, 0);
    p->playing = 0;
    if (p->sink.pause && p->sink_open) p->sink.pause(p->sctx);
    kl_bsplayer_fire(p, KL_BS_EV_PAUSED);
}

void kl_bsplayer_resume(kl_bsplayer *p) {
    kl_bsplayer_play(p);   // same edge as play; replaying fires again
}

void kl_bsplayer_stop(kl_bsplayer *p) {
    if (!p) return;
    int was_playing = p->playing;
    kl_avdec_play(p->dec, 0);
    kl_avdec_seek(p->dec, 0);
    p->playing = 0;
    bs_audio_rewind(p);   // audio cursor to 0 + sink flushed
    if (was_playing) kl_bsplayer_fire(p, KL_BS_EV_PAUSED);
}

long long kl_bsplayer_seek_to_us(kl_bsplayer *p, long long us) {
    if (!p) return 0;
    long long dur = 0;
    kl_avdemux_info(p->dm, NULL, NULL, NULL, &dur, NULL);
    if (us < 0) us = 0;
    if (us > dur) us = dur;
    kl_avdec_seek(p->dec, us / 1000);
    bs_audio_seek(p, us);   // audio cursor follows the picture, sink flushed
    return us;
}

void kl_bsplayer_set_volume(kl_bsplayer *p, float v) {
    if (!p) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    pthread_mutex_lock(&p->amu);
    p->volume = v;
    pthread_mutex_unlock(&p->amu);
}

float kl_bsplayer_volume(kl_bsplayer *p) {
    return p ? p->volume : 0;
}

long long kl_bsplayer_position_ms(kl_bsplayer *p) {
    if (!p) return 0;
    return kl_avdec_position_ms(p->dec);
}

long long kl_bsplayer_audio_position_us(kl_bsplayer *p) {
    if (!p) return -1;
    pthread_mutex_lock(&p->amu);
    long long a = kl_avdemux_audio_time_us(p->dm);
    pthread_mutex_unlock(&p->amu);
    return a;
}

unsigned long kl_bsplayer_av_realigns(kl_bsplayer *p) {
    if (!p) return 0;
    pthread_mutex_lock(&p->amu);
    unsigned long n = p->realigns;
    pthread_mutex_unlock(&p->amu);
    return n;
}

int kl_bsplayer_update(kl_bsplayer *p) {
    if (!p) return -1;
    if (!p->ended_fired && p->ready_fired && kl_avdec_complete(p->dec)) {
        p->ended_fired = 1;
        p->playing = 0;
        if (p->sink.pause && p->sink_open) p->sink.pause(p->sctx);
        kl_bsplayer_fire(p, KL_BS_EV_ENDED);
    }
    return 0;
}

int kl_bsplayer_present(kl_bsplayer *p) {
    if (!p) return -1;
    int w = 0, h = 0;
    size_t bytes = 0;
    unsigned long long serial = 0;
    kl_avdec_frame_lock(p->dec);
    const void *cur = kl_avdec_frame(p->dec, &w, &h, &bytes, &serial);
    unsigned char *copy = NULL;
    if (cur && serial != 0 && serial != p->presented && bytes) {
        size_t n = (size_t)w * h * 4;
        if (n > bytes) n = bytes;
        copy = malloc(n ? n : 1);
        if (copy && n) memcpy(copy, cur, n);
        else {
            free(copy);
            copy = NULL;
        }
    }
    kl_avdec_frame_unlock(p->dec);
    if (!copy) return 1;   // no new frame (or out of memory: skip, retry next)
    int rc = -1;
    if (p->upload) rc = p->upload(p->uctx, copy, w, h, serial);
    free(copy);
    if (rc == 0) p->presented = serial;
    return rc;
}

int kl_bsplayer_decode_first(kl_bsplayer *p, unsigned char **bgra, int *w,
                             int *h) {
    if (!p || !bgra) return -1;
    *bgra = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    kl_avdec_play(p->dec, 1);
    // The decoder pumps on its own thread; serial counts published frames.
    int fw = 0, fh = 0;
    size_t bytes = 0;
    unsigned long long serial = 0;
    for (int i = 0; i < 200; i++) {
        const void *f = kl_avdec_frame(p->dec, &fw, &fh, &bytes, &serial);
        if (f && serial != 0 && bytes) break;
        usleep(50000);
    }
    int rc = -1;
    kl_avdec_frame_lock(p->dec);
    const void *cur = kl_avdec_frame(p->dec, &fw, &fh, &bytes, &serial);
    if (cur && serial != 0 && bytes) {
        size_t n = (size_t)fw * fh * 4;
        if (n > bytes) n = bytes;
        unsigned char *out = malloc(n ? n : 1);
        if (out && n) {
            memcpy(out, cur, n);
            *bgra = out;
            if (w) *w = fw;
            if (h) *h = fh;
            rc = 0;
        } else {
            free(out);
        }
    }
    kl_avdec_frame_unlock(p->dec);
    kl_avdec_play(p->dec, 0);
    return rc;
}
