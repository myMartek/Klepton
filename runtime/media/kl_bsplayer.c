// See kl_bsplayer.h.
#include "kl_bsplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <strings.h>

#include "kl_avdec.h"
#include "kl_env.h"
#include "kl_ffplay.h"

struct kl_bsplayer {
    kl_avdec *dec;
    kl_avdemux *dm;
    // ...or, for a file AVFoundation refuses (MKV, AVI, WebM, TS, WMV, OGV),
    // the same two halves through FFmpeg (kl_ffplay.h). fv set = this path;
    // fa NULL there = a film without sound FFmpeg decodes. fa without fv: the
    // picture through AVFoundation, the sound (DTS, TrueHD) through FFmpeg.
    kl_ffvid *fv;
    kl_ffaud *fa;
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
    // The Audio Tracks menu: the film's path (FFmpeg opens it again for
    // another track), the list FFmpeg gave once, and a switch the pump must
    // answer by reopening the sink when the new track's rate or channels
    // differ (sink_reformat, under amu).
    char *path;
    char *tracks;
    int ntracks;
    int tracks_done;
    int sink_reformat;
    pthread_t tthr;                 // kl_bsplayer_audio_tracks_prefetch
    int tthr_live;
    // KL_BS_SOUND_ASYNC: FFmpeg's sound is opened on sthr; until it is in
    // (fa_pending, under amu) the pump waits instead of calling it silent.
    pthread_t sthr;
    int sthr_live;
    int fa_pending;
    // Subtitles (the Subtitles menu): the list (embedded text streams, then
    // files beside the film), and a reader thread that keeps the next cues
    // of the chosen one in a queue; the caller's poll hands out what is due
    // at the picture's position. All under smu.
    pthread_mutex_t smu;
    pthread_cond_t scv;
    struct bs_text *texts;
    int ntexts;
    int texts_done;
    int sub_sel;                    // index into texts, -1 = off
    unsigned sub_gen;               // bumped by a pick or a seek
    long long sub_seek_us;          // a seek's target (the picture may lag it), -1
    long long sub_hold_us;          // the poll's position until the picture is there
    long long sub_hold_until_ms;    // ...or until this (monotonic), -1
    int sub_quit;
    pthread_t subthr;
    int subthr_live;
    struct bs_cue { char *text; long long start, end; } cues[32];
    int ncues;
};

struct bs_text {
    char lang[16];
    char title[160];
    char codec[16];
    char path[1024];                // the film, or the file beside it
    int nth;                        // subtitle stream in that file
    int dispo;                      // 1 default, 2 forced
    int file;                       // a file beside the film
};

// One player, two engines: AVFoundation (dec + dm) or FFmpeg (fv + fa).
static void bs_play(kl_bsplayer *p, int on) {
    if (p->fv) kl_ffvid_play(p->fv, on); else kl_avdec_play(p->dec, on);
}
static void bs_seek(kl_bsplayer *p, long long ms) {
    if (p->fv) kl_ffvid_seek(p->fv, ms); else kl_avdec_seek(p->dec, ms);
}
static long long bs_pos_ms(kl_bsplayer *p) {
    return p->fv ? kl_ffvid_position_ms(p->fv) : kl_avdec_position_ms(p->dec);
}
static int bs_complete(kl_bsplayer *p) {
    return p->fv ? kl_ffvid_complete(p->fv) : kl_avdec_complete(p->dec);
}
static const void *bs_frame(kl_bsplayer *p, int *w, int *h, size_t *bytes,
                            unsigned long long *serial) {
    return p->fv ? kl_ffvid_frame(p->fv, w, h, bytes, serial)
                 : kl_avdec_frame(p->dec, w, h, bytes, serial);
}
static void bs_frame_lock(kl_bsplayer *p) {
    if (p->fv) kl_ffvid_frame_lock(p->fv); else kl_avdec_frame_lock(p->dec);
}
static void bs_frame_unlock(kl_bsplayer *p) {
    if (p->fv) kl_ffvid_frame_unlock(p->fv); else kl_avdec_frame_unlock(p->dec);
}
static int bs_info(kl_bsplayer *p, int *w, int *h, const char **mime,
                   long long *dur_us, float *fps) {
    return p->fv ? kl_ffvid_info(p->fv, w, h, mime, dur_us, fps)
                 : kl_avdemux_info(p->dm, w, h, mime, dur_us, fps);
}
static int bs_a_info(kl_bsplayer *p, int *rate, int *ch) {
    if (p->fa) return kl_ffaud_info(p->fa, rate, ch);
    if (p->fv) return 0;
    return kl_avdemux_audio_info(p->dm, rate, ch);
}
static long long bs_a_time_us(kl_bsplayer *p) {
    if (p->fa) return kl_ffaud_time_us(p->fa);
    if (p->fv) return -1;
    return kl_avdemux_audio_time_us(p->dm);
}
static long bs_a_read(kl_bsplayer *p, unsigned char *buf, unsigned long cap) {
    if (p->fa) return kl_ffaud_read(p->fa, buf, cap);
    if (p->fv) return -1;
    return kl_avdemux_audio_read(p->dm, buf, cap);
}
static int bs_a_advance(kl_bsplayer *p) {
    if (p->fa) return kl_ffaud_advance(p->fa);
    if (p->fv) return 0;
    return kl_avdemux_audio_advance(p->dm);
}
static int bs_a_seek_us(kl_bsplayer *p, long long us) {
    if (p->fa) return kl_ffaud_seek_us(p->fa, us);
    if (p->fv) return -1;
    return kl_avdemux_audio_seek_us(p->dm, us);
}

static void kl_bsplayer_fire(kl_bsplayer *p, kl_bsplayer_event ev) {
    if (p->listener) p->listener(p->lctx, ev);
}

static void *bs_sound_open_thread(void *arg);

kl_bsplayer *kl_bsplayer_open(const char *path, char *reason, size_t cap) {
    return kl_bsplayer_open2(path, 0, reason, cap);
}

kl_bsplayer *kl_bsplayer_open2(const char *path, int flags, char *reason, size_t cap) {
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
    kl_ffvid *fv = NULL;
    kl_ffaud *fa = NULL;
    if (!dm || !dec) {
        if (dec) kl_avdec_close(dec);
        if (dm) kl_avdemux_close(dm);
        dec = NULL;
        dm = NULL;
        // What AVFoundation cannot open, FFmpeg may (kl_ffplay.h).
        char why[300] = "FFmpeg is not here";
        fv = kl_ffvid_open(path, why, sizeof why);
        if (!fv) {
            if (reason && cap)
                snprintf(reason, cap, "unsupported container or no video track: %s (%s)",
                         path, why);
            return NULL;
        }
        if (!(flags & (KL_BS_NO_SOUND | KL_BS_SOUND_ASYNC))) fa = kl_ffaud_open(path);
    } else if (!kl_avdemux_audio_info(dm, NULL, NULL) &&
               !(flags & (KL_BS_NO_SOUND | KL_BS_SOUND_ASYNC))) {
        // Picture through AVFoundation, sound through FFmpeg: an MP4/MOV
        // whose sound no Apple decoder takes (a Blu-ray remux with DTS,
        // DTS-HD MA or TrueHD). AVFoundation does not even list a DTS track
        // in an MP4 ('mp4a' object type 0xA9), so any film it finds silent
        // is asked of FFmpeg, which reads the file a second time for the
        // sound and folds it to stereo. A truly silent film costs one probe.
        fa = kl_ffaud_open(path);
        if (fa || kl_avdemux_audio_foreign(dm))
            fprintf(stderr, "  [bsplayer] %s: sound through FFmpeg%s\n", path,
                    fa ? "" : " failed, the film plays silent");
    }
    kl_bsplayer *p = calloc(1, sizeof *p);
    if (!p) {
        if (dec) kl_avdec_close(dec);
        if (dm) kl_avdemux_close(dm);
        kl_ffaud_close(fa);
        kl_ffvid_close(fv);
        if (reason && cap) snprintf(reason, cap, "out of memory");
        return NULL;
    }
    p->dec = dec;
    p->dm = dm;
    p->fv = fv;
    p->fa = fa;
    p->volume = 1.0f;
    p->path = strdup(path);
    pthread_mutex_init(&p->amu, NULL);
    pthread_mutex_init(&p->smu, NULL);
    pthread_cond_init(&p->scv, NULL);
    p->sub_sel = -1;
    p->sub_seek_us = -1;
    p->sub_hold_us = -1;
    // The sound FFmpeg would give (an MKV's, an MP4's DTS) opened off the
    // caller's thread: for a film on pCloud that open is network-bound, and
    // the caller is the game's main thread.
    if ((flags & KL_BS_SOUND_ASYNC) && !(flags & KL_BS_NO_SOUND) &&
        (fv || !kl_avdemux_audio_info(dm, NULL, NULL)) && kl_ff_available()) {
        p->fa_pending = 1;
        if (pthread_create(&p->sthr, NULL, bs_sound_open_thread, p) == 0) p->sthr_live = 1;
        else p->fa_pending = 0;
    }
    return p;
}

int kl_bsplayer_info(kl_bsplayer *p, int *w, int *h, const char **mime,
                     long long *dur_us) {
    if (!p) return -1;
    float fps = 0;
    return bs_info(p, w, h, mime, dur_us, &fps) ? 0 : -1;
}

void kl_bsplayer_close(kl_bsplayer *p) {
    if (!p) return;
    if (p->tthr_live) pthread_join(p->tthr, NULL);   // a track list being read
    if (p->sthr_live) pthread_join(p->sthr, NULL);   // the sound being opened
    if (p->subthr_live) {                             // the subtitle reader
        pthread_mutex_lock(&p->smu);
        p->sub_quit = 1;
        pthread_cond_broadcast(&p->scv);
        pthread_mutex_unlock(&p->smu);
        pthread_join(p->subthr, NULL);
    }
    for (int i = 0; i < p->ncues; i++) free(p->cues[i].text);
    free(p->texts);
    pthread_cond_destroy(&p->scv);
    pthread_mutex_destroy(&p->smu);
    // Park the audio thread first: it owns sink.close and must not touch
    // the demuxer past this point.
    if (p->arun) {
        pthread_mutex_lock(&p->amu);
        p->arun = 0;
        pthread_mutex_unlock(&p->amu);
        pthread_join(p->athr, NULL);
    }
    if (p->dec) kl_avdec_close(p->dec);
    if (p->dm) kl_avdemux_close(p->dm);
    kl_ffaud_close(p->fa);
    kl_ffvid_close(p->fv);
    pthread_mutex_destroy(&p->amu);
    free(p->path);
    free(p->tracks);
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
    int has = bs_a_info(p, &r, &c);
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
    bs_a_seek_us(p, pic_ms * 1000);
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
    int rate = 0, ch = 0, has = 0;
    kl_bsplayer_audio_sink sink;
    void *sctx;
    for (;;) {      // a sound still being opened (KL_BS_SOUND_ASYNC) is waited for
        pthread_mutex_lock(&p->amu);
        has = bs_a_info(p, &rate, &ch);
        sink = p->sink;
        sctx = p->sctx;
        int wait = !has && p->fa_pending && p->arun;
        pthread_mutex_unlock(&p->amu);
        if (!wait) break;
        usleep(20000);
    }
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
        int reformat = p->sink_reformat;
        int nrate = 0, nch = 0;
        if (reformat) {
            p->sink_reformat = 0;
            bs_a_info(p, &nrate, &nch);
        }
        pthread_mutex_unlock(&p->amu);
        if (!run) break;
        // Another sound track (kl_bsplayer_select_audio): a sink opened for
        // 44.1 kHz mono would play a 48 kHz stereo track at the wrong speed.
        if (nrate <= 0) nrate = 44100;
        if (nch <= 0 || nch > 2) nch = 2;
        if (reformat && (nrate != rate || nch != ch)) {
            if (sink.close) sink.close(sctx);
            rate = nrate;
            ch = nch;
            int orc = sink.open(sctx, (unsigned)rate, (unsigned)ch);
            pthread_mutex_lock(&p->amu);
            p->sink_open = orc == 0;
            pthread_mutex_unlock(&p->amu);
            fprintf(stderr, "  [bs] sound track switch: sink reopened at %d Hz, %d ch%s\n",
                    rate, ch, orc == 0 ? "" : " (refused)");
            if (orc != 0) break;
            if (sink.play) sink.play(sctx);
        }
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
            long long pic = bs_pos_ms(p);
            pthread_mutex_lock(&p->amu);
            long long aud = bs_a_time_us(p);
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
        long long pts = bs_a_time_us(p);
        long n = pts < 0 ? -1
                         : bs_a_read(p, buf, sizeof buf);
        if (n > 0) bs_a_advance(p);
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
static void bs_sub_resync(kl_bsplayer *p, long long us);

static void bs_audio_seek(kl_bsplayer *p, long long us) {
    if (!p) return;
    bs_sub_resync(p, us);       // every seek and rewind passes here
    pthread_mutex_lock(&p->amu);
    bs_a_seek_us(p, us);
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
    if (bs_complete(p)) {
        // Replay after the end restarts from the top: the old reader is
        // spent (its next sample is permanently EOS), so rewind first.
        bs_seek(p, 0);
        bs_audio_seek(p, 0);
    }
    bs_play(p, 1);
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
    bs_play(p, 0);
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
    bs_play(p, 0);
    bs_seek(p, 0);
    p->playing = 0;
    bs_audio_rewind(p);   // audio cursor to 0 + sink flushed
    if (was_playing) kl_bsplayer_fire(p, KL_BS_EV_PAUSED);
}

long long kl_bsplayer_seek_to_us(kl_bsplayer *p, long long us) {
    if (!p) return 0;
    long long dur = 0;
    bs_info(p, NULL, NULL, NULL, &dur, NULL);
    if (us < 0) us = 0;
    if (us > dur) us = dur;
    bs_seek(p, us / 1000);
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
    return bs_pos_ms(p);
}

long long kl_bsplayer_audio_position_us(kl_bsplayer *p) {
    if (!p) return -1;
    pthread_mutex_lock(&p->amu);
    long long a = bs_a_time_us(p);
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
    if (!p->ended_fired && p->ready_fired && bs_complete(p)) {
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
    bs_frame_lock(p);
    const void *cur = bs_frame(p, &w, &h, &bytes, &serial);
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
    bs_frame_unlock(p);
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
    bs_play(p, 1);
    // The decoder pumps on its own thread; serial counts published frames.
    int fw = 0, fh = 0;
    size_t bytes = 0;
    unsigned long long serial = 0;
    for (int i = 0; i < 200; i++) {
        const void *f = bs_frame(p, &fw, &fh, &bytes, &serial);
        if (f && serial != 0 && bytes) break;
        usleep(50000);
    }
    int rc = -1;
    bs_frame_lock(p);
    const void *cur = bs_frame(p, &fw, &fh, &bytes, &serial);
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
    bs_frame_unlock(p);
    bs_play(p, 0);
    return rc;
}

// ---- the Audio Tracks menu ----
//
// FFmpeg lists the streams: it sees every one, also the DTS track
// AVFoundation hides in an MP4. Picking one moves the sound to FFmpeg (if it
// was not there already) reading that stream, put where the picture is.

int kl_bsplayer_select_audio(kl_bsplayer *p, int nth);

// AVFoundation plays the first sound track it can see; the file may flag
// another as its default (one AVFoundation cannot see: DTS). FFmpeg's pick
// is that default, so when its track ID is not AVFoundation's, switch.
static void bs_follow_file_default(kl_bsplayer *p) {
    if (!p->dm) return;
    pthread_mutex_lock(&p->amu);
    int fa = p->fa != NULL;
    int avf_id = kl_avdemux_audio_track_id(p->dm);
    int want = -1, want_id = 0, k = 0;
    for (const char *l = p->tracks; l && *l; k++) {
        const char *e = strchr(l, '\n');
        // fields: lang, title, codec, channels, default, current, id
        int col = 0, cur = 0, id = 0;
        for (const char *q = l; q && q < (e ? e : l + strlen(l)); col++) {
            if (col == 5) cur = atoi(q);
            if (col == 6) id = atoi(q);
            q = strchr(q, '\t');
            if (q) q++;
        }
        if (cur) { want = k; want_id = id; }
        l = e ? e + 1 : NULL;
    }
    pthread_mutex_unlock(&p->amu);
    if (fa || want < 0 || !avf_id || want_id == avf_id) return;
    fprintf(stderr, "  [bs] %s: the file's default sound is track %d (id %d), "
                    "AVFoundation plays id %d: switching\n", p->path, want, want_id, avf_id);
    kl_bsplayer_select_audio(p, want);
}

int kl_bsplayer_audio_tracks(kl_bsplayer *p, char *buf, size_t cap) {
    if (!p) return 0;
    pthread_mutex_lock(&p->amu);
    int done = p->tracks_done;
    pthread_mutex_unlock(&p->amu);
    if (!done) {
        for (;;) {      // the sound FFmpeg is opening answers the list itself
            pthread_mutex_lock(&p->amu);
            int pending = p->fa_pending;
            pthread_mutex_unlock(&p->amu);
            if (!pending) break;
            usleep(20000);
        }
        char *list = malloc(8192);
        int n = -1;
        if (list) {
            list[0] = 0;
            pthread_mutex_lock(&p->amu);
            if (p->fa) n = kl_ffaud_tracks(p->fa, list, 8192);
            pthread_mutex_unlock(&p->amu);
            if (n < 0 && !p->fa) {
                // The sound is AVFoundation's: FFmpeg opens the file once
                // more, outside the pump's lock, only to read its streams.
                kl_ffaud *t = kl_ffaud_open(p->path);
                if (t) n = kl_ffaud_tracks(t, list, 8192);
                kl_ffaud_close(t);
            }
        }
        pthread_mutex_lock(&p->amu);
        if (!p->tracks_done) {
            p->tracks_done = 1;
            p->ntracks = n > 0 ? n : 0;
            p->tracks = n > 0 ? list : NULL;
            if (n <= 0) free(list);
        } else {
            free(list);
        }
        pthread_mutex_unlock(&p->amu);
        fprintf(stderr, "  [bs] %s: %d sound track(s)\n", p->path, n > 0 ? n : 0);
        bs_follow_file_default(p);
    }
    pthread_mutex_lock(&p->amu);
    int n = p->ntracks;
    if (buf && cap) {
        buf[0] = 0;
        if (p->tracks) snprintf(buf, cap, "%s", p->tracks);
    }
    pthread_mutex_unlock(&p->amu);
    return n;
}

int kl_bsplayer_select_audio(kl_bsplayer *p, int nth) {
    if (!p || nth < 0) return -1;
    long long pic_us = bs_pos_ms(p) * 1000;    // outside amu, as the pump does
    pthread_mutex_lock(&p->amu);
    kl_ffaud *fa = p->fa;
    int rc = -1;
    if (fa && kl_ffaud_select(fa, nth) == 0) {
        rc = 0;
        bs_a_seek_us(p, pic_us);
        p->sink_reformat = 1;
        p->realign_armed = 0;
    }
    pthread_mutex_unlock(&p->amu);
    if (!fa) {
        kl_ffaud *nfa = kl_ffaud_open(p->path);
        if (nfa && kl_ffaud_select(nfa, nth) == 0) {
            kl_ffaud_seek_us(nfa, bs_pos_ms(p) * 1000);
            pthread_mutex_lock(&p->amu);
            if (!p->fa) {
                p->fa = nfa;        // bs_a_* take FFmpeg's sound from now on
                nfa = NULL;
                rc = 0;
                p->sink_reformat = 1;
                p->realign_armed = 0;
            }
            pthread_mutex_unlock(&p->amu);
        }
        kl_ffaud_close(nfa);
    }
    pthread_mutex_lock(&p->amu);
    kl_bsplayer_audio_sink sink = p->sink;
    void *sctx = p->sctx;
    int open = p->sink_open;
    pthread_mutex_unlock(&p->amu);
    if (rc == 0 && sink.flush && open) sink.flush(sctx);
    fprintf(stderr, "  [bs] %s: sound track %d %s\n", p->path, nth,
            rc == 0 ? "selected" : "refused");
    return rc;
}

static void *bs_tracks_thread(void *arg) {
    kl_bsplayer_audio_tracks(arg, NULL, 0);
    kl_bsplayer_text_tracks(arg, NULL, 0);
    return NULL;
}

void kl_bsplayer_audio_tracks_prefetch(kl_bsplayer *p) {
    if (!p || p->tthr_live) return;
    if (pthread_create(&p->tthr, NULL, bs_tracks_thread, p) == 0) p->tthr_live = 1;
}

int kl_bsplayer_audio_tracks_peek(kl_bsplayer *p, char *buf, size_t cap) {
    if (!p) return -1;
    pthread_mutex_lock(&p->amu);
    int done = p->tracks_done;
    pthread_mutex_unlock(&p->amu);
    return done ? kl_bsplayer_audio_tracks(p, buf, cap) : -1;
}

static void *bs_sound_open_thread(void *arg) {
    kl_bsplayer *p = arg;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    kl_ffaud *fa = kl_ffaud_open(p->path);
    long long us = bs_pos_ms(p) * 1000;      // the picture ran on meanwhile
    if (fa && us > 0) kl_ffaud_seek_us(fa, us);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_mutex_lock(&p->amu);
    if (fa && !p->fa) {         // a SelectAudioTrack may have been quicker
        p->fa = fa;
        fa = NULL;
        p->sink_reformat = 1;
    }
    int got = p->fa != NULL;
    p->fa_pending = 0;
    pthread_mutex_unlock(&p->amu);
    kl_ffaud_close(fa);
    fprintf(stderr, "  [bs] %s: sound through FFmpeg %s after %lld ms (picture at %.1f s)\n",
            p->path, got ? "in" : "failed, the film plays silent",
            (long long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000,
            (double)us / 1e6);
    return NULL;
}

// ---- the Subtitles menu ----
//
// Text only: Bigscreen's cue callback carries text, so picture subtitles
// (PGS, VobSub) are not listed. Sources, in this order: the film's own text
// streams, then files beside it named like it (Film.srt, Film.de.srt,
// Film.en.forced.ass, Film.vtt). Off until one is picked.

static const char *bs_sub_exts[] = { ".srt", ".ass", ".ssa", ".vtt" };

static void bs_text_files(kl_bsplayer *p, struct bs_text *out, int *n, int max) {
    const char *slash = strrchr(p->path, '/');
    if (!slash) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - p->path), p->path);
    const char *base = slash + 1;
    const char *dot = strrchr(base, '.');
    size_t blen = dot ? (size_t)(dot - base) : strlen(base);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    // Sorted by name so the list is the same every time.
    char names[32][256];
    int nn = 0;
    while ((de = readdir(d)) && nn < 32) {
        const char *nm = de->d_name;
        size_t l = strlen(nm);
        if (l <= blen || strncmp(nm, base, blen) != 0 || nm[blen] != '.') continue;
        int ok = 0;
        for (size_t k = 0; k < sizeof bs_sub_exts / sizeof bs_sub_exts[0]; k++) {
            size_t el = strlen(bs_sub_exts[k]);
            if (l > el && !strcasecmp(nm + l - el, bs_sub_exts[k])) ok = 1;
        }
        if (ok) snprintf(names[nn++], sizeof names[0], "%s", nm);
    }
    closedir(d);
    for (int i = 0; i < nn; i++)
        for (int j = i + 1; j < nn; j++)
            if (strcmp(names[j], names[i]) < 0) {
                char t[256];
                memcpy(t, names[i], sizeof t);
                memcpy(names[i], names[j], sizeof t);
                memcpy(names[j], t, sizeof t);
            }
    for (int i = 0; i < nn && *n < max; i++) {
        struct bs_text *t = &out[(*n)++];
        memset(t, 0, sizeof *t);
        snprintf(t->path, sizeof t->path, "%s/%s", dir, names[i]);
        t->file = 1;
        // The parts between the film's name and the extension: a language
        // ("de", "ger") and "forced".
        const char *ext = strrchr(names[i], '.');
        snprintf(t->codec, sizeof t->codec, "%s", ext + 1);
        for (char *c = t->codec; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        char mid[128] = "";
        if (ext > names[i] + blen + 1)
            snprintf(mid, sizeof mid, "%.*s", (int)(ext - (names[i] + blen + 1)), names[i] + blen + 1);
        char *save = NULL;
        for (char *tok = strtok_r(mid, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
            if (!strcasecmp(tok, "forced")) t->dispo |= 2;
            else if (!strcasecmp(tok, "default")) t->dispo |= 1;
            else if (!t->lang[0] && strlen(tok) <= 3) snprintf(t->lang, sizeof t->lang, "%s", tok);
        }
    }
}

int kl_bsplayer_text_tracks(kl_bsplayer *p, char *buf, size_t cap) {
    if (!p) return 0;
    pthread_mutex_lock(&p->smu);
    int done = p->texts_done;
    pthread_mutex_unlock(&p->smu);
    if (!done) {
        struct bs_text *t = calloc(32, sizeof *t);
        int n = 0;
        if (t) {
            char *list = malloc(8192);
            int k = list ? kl_ffsub_tracks(p->path, list, 8192) : -1;
            const char *l = list;
            for (int i = 0; i < k && l && *l && n < 32; i++) {
                const char *e = strchr(l, '\n');
                char line[512];
                snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
                char *f[8] = {0};
                char *q = line;
                for (int c = 0; c < 8 && q; c++) {
                    f[c] = q;
                    q = strchr(q, '\t');
                    if (q) *q++ = 0;
                }
                if (f[7] && atoi(f[7])) {          // text, not pictures
                    struct bs_text *x = &t[n++];
                    snprintf(x->lang, sizeof x->lang, "%s", f[0] ? f[0] : "");
                    snprintf(x->title, sizeof x->title, "%s", f[1] ? f[1] : "");
                    snprintf(x->codec, sizeof x->codec, "%s", f[2] ? f[2] : "");
                    snprintf(x->path, sizeof x->path, "%s", p->path);
                    x->nth = i;
                    x->dispo = f[4] ? atoi(f[4]) : 0;
                }
                l = e ? e + 1 : NULL;
            }
            free(list);
            bs_text_files(p, t, &n, 32);
        }
        pthread_mutex_lock(&p->smu);
        if (!p->texts_done) {
            p->texts_done = 1;
            p->texts = t;
            p->ntexts = n;
            t = NULL;
        }
        pthread_mutex_unlock(&p->smu);
        free(t);
        fprintf(stderr, "  [bs] %s: %d subtitle track(s)\n", p->path, n);
    }
    pthread_mutex_lock(&p->smu);
    int n = p->ntexts;
    if (buf && cap) {
        size_t len = 0;
        buf[0] = 0;
        for (int i = 0; i < n; i++) {
            struct bs_text *x = &p->texts[i];
            int w = snprintf(buf + len, cap - len, "%s\t%s\t%s\t%d\t%d\n", x->lang, x->title,
                             x->codec, x->dispo, x->file);
            if (w < 0 || (size_t)w >= cap - len) break;
            len += (size_t)w;
        }
    }
    pthread_mutex_unlock(&p->smu);
    return n;
}

int kl_bsplayer_text_tracks_peek(kl_bsplayer *p, char *buf, size_t cap) {
    if (!p) return -1;
    pthread_mutex_lock(&p->smu);
    int done = p->texts_done;
    pthread_mutex_unlock(&p->smu);
    return done ? kl_bsplayer_text_tracks(p, buf, cap) : -1;
}

static void bs_cues_clear_locked(kl_bsplayer *p) {
    for (int i = 0; i < p->ncues; i++) free(p->cues[i].text);
    p->ncues = 0;
}

static void bs_sub_resync(kl_bsplayer *p, long long us) {
    pthread_mutex_lock(&p->smu);
    p->sub_gen++;
    p->sub_seek_us = us;
    // AVFoundation's picture reports the old position for a moment after a
    // seek; the poll would take the new cues for stale ones and drop them.
    p->sub_hold_us = us;
    p->sub_hold_until_ms = bs_now_ms() + 1500;
    bs_cues_clear_locked(p);
    pthread_cond_broadcast(&p->scv);
    pthread_mutex_unlock(&p->smu);
}

// Keeps the cues of the chosen source ahead of the picture: up to 32, and
// none further ahead than a minute. A pick or a seek (sub_gen) reopens or
// re-seeks the source where the picture is.
static void *bs_sub_thread(void *arg) {
    kl_bsplayer *p = arg;
    kl_ffsub *src = NULL;
    int src_sel = -1;
    unsigned gen = 0;
    int eof = 0;
    char *text = malloc(4096);
    pthread_mutex_lock(&p->smu);
    for (;;) {
        if (p->sub_quit) break;
        if (p->sub_gen != gen) {
            gen = p->sub_gen;
            int sel = p->sub_sel;
            long long target = p->sub_seek_us;
            p->sub_seek_us = -1;
            struct bs_text t = {0};
            if (sel >= 0 && sel < p->ntexts) t = p->texts[sel];
            pthread_mutex_unlock(&p->smu);
            if (sel != src_sel) {
                kl_ffsub_close(src);
                src = sel >= 0 ? kl_ffsub_open(t.path, t.nth) : NULL;
                src_sel = sel;
                fprintf(stderr, "  [bs] subtitles: %s%s\n", sel >= 0 ? t.path : "off",
                        sel >= 0 && !src ? " (could not be read)" : "");
            }
            long long pos = target >= 0 ? target : bs_pos_ms(p) * 1000;
            if (src) kl_ffsub_seek_us(src, pos);
            eof = 0;
            pthread_mutex_lock(&p->smu);
            if (gen != p->sub_gen) continue;
            bs_cues_clear_locked(p);
        }
        long long ahead = p->ncues ? p->cues[p->ncues - 1].start : -1;
        int want = src && !eof && text && p->ncues < 32;
        pthread_mutex_unlock(&p->smu);
        long long pos = bs_pos_ms(p) * 1000;
        pthread_mutex_lock(&p->smu);
        if (want && (ahead < 0 || ahead < pos + 60000000LL)) {
            pthread_mutex_unlock(&p->smu);
            long long st = 0, en = 0;
            int rc = kl_ffsub_next(src, text, 4096, &st, &en);
            pthread_mutex_lock(&p->smu);
            if (gen != p->sub_gen) continue;            // picked or seeked meanwhile
            if (rc != 1) { eof = 1; continue; }
            if (p->ncues < 32) {
                p->cues[p->ncues].text = strdup(text);
                p->cues[p->ncues].start = st;
                p->cues[p->ncues].end = en;
                p->ncues++;
            }
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&p->scv, &p->smu, &ts);
    }
    pthread_mutex_unlock(&p->smu);
    kl_ffsub_close(src);
    free(text);
    return NULL;
}

int kl_bsplayer_select_text(kl_bsplayer *p, int nth) {
    if (!p) return -1;
    pthread_mutex_lock(&p->smu);
    if (nth >= p->ntexts || nth < -1) {
        pthread_mutex_unlock(&p->smu);
        return -1;
    }
    p->sub_sel = nth;
    p->sub_gen++;
    bs_cues_clear_locked(p);
    pthread_cond_broadcast(&p->scv);
    int start = !p->subthr_live && nth >= 0;
    if (start && pthread_create(&p->subthr, NULL, bs_sub_thread, p) == 0) p->subthr_live = 1;
    pthread_mutex_unlock(&p->smu);
    fprintf(stderr, "  [bs] %s: subtitle track %d %s\n", p->path, nth, nth >= 0 ? "selected" : "off");
    return 0;
}

int kl_bsplayer_sub_poll(kl_bsplayer *p, char *text, size_t cap, long long *start_us,
                         long long *end_us) {
    if (!p) return 0;
    long long pos = bs_pos_ms(p) * 1000;
    pthread_mutex_lock(&p->smu);
    if (p->sub_hold_us >= 0) {
        if ((pos >= p->sub_hold_us - 200000 && pos <= p->sub_hold_us + 2000000) ||
            bs_now_ms() > p->sub_hold_until_ms)
            p->sub_hold_us = -1;            // the picture is there (or never will be)
        else
            pos = p->sub_hold_us;
    }
    int got = 0;
    // What ended before the picture is stale (a seek, a slow reader).
    while (p->ncues && p->cues[0].end <= pos) {
        free(p->cues[0].text);
        memmove(p->cues, p->cues + 1, sizeof p->cues[0] * (size_t)(p->ncues - 1));
        p->ncues--;
    }
    if (p->ncues && p->cues[0].start <= pos) {
        if (text && cap) snprintf(text, cap, "%s", p->cues[0].text);
        if (start_us) *start_us = p->cues[0].start;
        if (end_us) *end_us = p->cues[0].end;
        free(p->cues[0].text);
        memmove(p->cues, p->cues + 1, sizeof p->cues[0] * (size_t)(p->ncues - 1));
        p->ncues--;
        got = 1;
        pthread_cond_broadcast(&p->scv);
    }
    pthread_mutex_unlock(&p->smu);
    return got;
}
