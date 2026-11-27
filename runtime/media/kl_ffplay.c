// See kl_ffplay.h.
#include "kl_ffplay.h"
#include "klff.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kl_env.h"
#include "kl_remote.h"

// ---- the framework ----------------------------------------------------------

static struct {
    klff *(*open)(const char *, int, char *, int);
    void (*close)(klff *);
    int (*info)(klff *, int *, int *, long long *, double *, char *, int, int *, int *);
    int (*video_next)(klff *, const unsigned char **, int *, int *, long long *, int *);
    int (*audio_next)(klff *, const short **, int *, long long *);
    int (*seek)(klff *, long long);
    const char *(*version)(void);
    // Optional: a framework built before the pCloud locations lacks it and
    // then plays local files only.
    klff *(*open_io)(klff_read_fn, klff_done_fn, void *, long long, const char *, int, char *, int);
    // Optional: a framework built before the Audio Tracks menu lists and
    // switches nothing.
    int (*audio_tracks)(klff *, char *, int);
    int (*audio_select)(klff *, int);
} ff;
static pthread_once_t ff_once = PTHREAD_ONCE_INIT;
static int ff_ok;

static void ff_load(void) {
    if (!kl_env_on("KL_FFMPEG", 1)) {
        fprintf(stderr, "  [ff] FFmpeg off (KL_FFMPEG=0)\n");
        return;
    }
    // Inside the app it sits in Frameworks/ like ANGLE; KL_FFMPEG_LIB names
    // another file (host tests).
    const char *path = kl_env_str("KL_FFMPEG_LIB", NULL);
    if (!path || !*path) path = "@executable_path/Frameworks/KleptonFFmpeg.framework/KleptonFFmpeg";
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "  [ff] no FFmpeg here (%s): only what AVFoundation opens plays\n", dlerror());
        return;
    }
#define FF_SYM(f, n) do { *(void **)&ff.f = dlsym(h, n); if (!ff.f) { \
        fprintf(stderr, "  [ff] %s lacks %s\n", path, n); return; } } while (0)
    FF_SYM(open, "klff_open");
    FF_SYM(close, "klff_close");
    FF_SYM(info, "klff_info");
    FF_SYM(video_next, "klff_video_next");
    FF_SYM(audio_next, "klff_audio_next");
    FF_SYM(seek, "klff_seek");
    FF_SYM(version, "klff_version");
#undef FF_SYM
    *(void **)&ff.open_io = dlsym(h, "klff_open_io");
    *(void **)&ff.audio_tracks = dlsym(h, "klff_audio_tracks");
    *(void **)&ff.audio_select = dlsym(h, "klff_audio_select");
    ff_ok = 1;
    fprintf(stderr, "  [ff] %s loaded\n", ff.version());
}

int kl_ff_available(void) {
    pthread_once(&ff_once, ff_load);
    return ff_ok;
}

// A film on a server (kl_remote.h) is read through FFmpeg's I/O callbacks;
// any other path opens as the file it is.
static long ff_remote_read(void *ctx, long long off, void *buf, unsigned long n) {
    return kl_remote_read(ctx, off, buf, n);
}
static void ff_remote_done(void *ctx) { kl_remote_close(ctx); }

static klff *ff_open(const char *path, int kind, char *e, int cap) {
    if (!kl_remote_is(path)) return ff.open(path, kind, e, cap);
    if (!ff.open_io) {
        snprintf(e, (size_t)cap, "this FFmpeg build cannot read from a server");
        return NULL;
    }
    kl_remote *r = kl_remote_open(path);
    if (!r) { snprintf(e, (size_t)cap, "not registered"); return NULL; }
    return ff.open_io(ff_remote_read, ff_remote_done, r, kl_remote_size(r),
                      kl_remote_name(r), kind, e, cap);
}

static long long ff_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- picture ----------------------------------------------------------------
//
// One thread decodes ahead by one frame and publishes it when the clock
// reaches its time. The clock is the wall clock while playing (base_pos at
// base_t) and stands while paused — kl_avdec's contract, which the sound
// pump in kl_bsplayer.c realigns against. The first frame after open or a
// seek is published at once, playing or not: the MediaInfo thumbnail and a
// scrub both wait for exactly that frame.

struct kl_ffvid {
    klff *f;
    int w, h;
    long long dur_us;
    float fps;
    char mime[40];
    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit, playing, eof, complete, have_pending, show_next;
    long long seek_req;          // ms, -1 none
    long long base_pos, base_t;  // ms
    unsigned char *pend;         // the decoded frame that waits for its time
    size_t pend_cap;
    int pw, ph;
    long long pend_pts;          // us
    pthread_mutex_t fmu;         // the published frame
    unsigned char *front;
    size_t front_cap, front_bytes;
    int fw, fh;
    unsigned long long serial;
    unsigned long hw_frames, sw_frames;
};

static long long vid_pos_locked(kl_ffvid *v) {
    long long p = v->base_pos + (v->playing ? ff_now_ms() - v->base_t : 0);
    long long d = v->dur_us / 1000;
    if (d > 0 && p > d) p = d;
    return p < 0 ? 0 : p;
}

static void vid_publish_locked(kl_ffvid *v) {
    pthread_mutex_lock(&v->fmu);
    unsigned char *t = v->front; size_t tc = v->front_cap;
    v->front = v->pend; v->front_cap = v->pend_cap;
    v->pend = t; v->pend_cap = tc;
    v->fw = v->pw; v->fh = v->ph;
    v->front_bytes = (size_t)v->pw * (size_t)v->ph * 4;
    v->serial++;
    pthread_mutex_unlock(&v->fmu);
    v->have_pending = 0;
}

static void *vid_thread(void *arg) {
    kl_ffvid *v = arg;
    pthread_mutex_lock(&v->mu);
    while (!v->quit) {
        if (v->seek_req >= 0) {
            long long s = v->seek_req;
            v->seek_req = -1;
            pthread_mutex_unlock(&v->mu);
            ff.seek(v->f, s * 1000);
            pthread_mutex_lock(&v->mu);
            v->have_pending = 0;
            v->eof = 0;
            v->complete = 0;
            v->show_next = 1;
            continue;
        }
        if (!v->have_pending && !v->eof) {
            pthread_mutex_unlock(&v->mu);
            const unsigned char *px = NULL;
            int w = 0, h = 0, hw = 0;
            long long pts = 0;
            int rc = ff.video_next(v->f, &px, &w, &h, &pts, &hw);
            pthread_mutex_lock(&v->mu);
            if (v->seek_req >= 0) continue;      // stale: the seek wins
            if (rc == 1 && px && w > 0 && h > 0) {
                size_t n = (size_t)w * (size_t)h * 4;
                if (n > v->pend_cap) {
                    unsigned char *nb = realloc(v->pend, n);
                    if (!nb) { v->eof = 1; continue; }
                    v->pend = nb;
                    v->pend_cap = n;
                }
                memcpy(v->pend, px, n);
                v->pw = w; v->ph = h;
                v->pend_pts = pts < 0 ? 0 : pts;
                v->have_pending = 1;
                if (hw) v->hw_frames++; else v->sw_frames++;
                if (v->hw_frames + v->sw_frames == 1)
                    fprintf(stderr, "  [ff] first picture %dx%d, %s\n", w, h,
                            hw ? "VideoToolbox" : "FFmpeg's own decoder");
            } else {
                v->eof = 1;
                if (rc < 0) fprintf(stderr, "  [ff] the picture ended on a decode error\n");
            }
            continue;
        }
        if (v->have_pending) {
            long long pos = vid_pos_locked(v);
            long long due = v->pend_pts / 1000;
            if (v->show_next || v->serial == 0 || due <= pos) {
                v->show_next = 0;
                vid_publish_locked(v);
                continue;
            }
            if (!v->playing) { pthread_cond_wait(&v->cv, &v->mu); continue; }
            long long wait = due - pos;
            if (wait > 20) wait = 20;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += wait * 1000000;
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&v->cv, &v->mu, &ts);
            continue;
        }
        // Everything shown: done, until a seek or close.
        v->complete = 1;
        pthread_cond_wait(&v->cv, &v->mu);
    }
    pthread_mutex_unlock(&v->mu);
    return NULL;
}

kl_ffvid *kl_ffvid_open(const char *path, char *err, size_t cap) {
    if (!kl_ff_available()) {
        if (err && cap) snprintf(err, cap, "FFmpeg is not here");
        return NULL;
    }
    char e[256] = "";
    klff *f = ff_open(path, KLFF_VIDEO, e, (int)sizeof e);
    if (!f) {
        if (err && cap) snprintf(err, cap, "FFmpeg: %s", e);
        return NULL;
    }
    kl_ffvid *v = calloc(1, sizeof *v);
    if (!v) { ff.close(f); return NULL; }
    v->f = f;
    char codec[32] = "";
    double fps = 0;
    ff.info(f, &v->w, &v->h, &v->dur_us, &fps, codec, (int)sizeof codec, NULL, NULL);
    v->fps = (float)fps;
    snprintf(v->mime, sizeof v->mime, "video/%s", *codec ? codec : "unknown");
    v->seek_req = -1;
    pthread_mutex_init(&v->mu, NULL);
    pthread_mutex_init(&v->fmu, NULL);
    pthread_cond_init(&v->cv, NULL);
    if (pthread_create(&v->thr, NULL, vid_thread, v) != 0) {
        pthread_mutex_destroy(&v->mu);
        pthread_mutex_destroy(&v->fmu);
        pthread_cond_destroy(&v->cv);
        ff.close(f);
        free(v);
        if (err && cap) snprintf(err, cap, "no thread");
        return NULL;
    }
    fprintf(stderr, "  [ff] %s: %s %dx%d, %.3f s\n", path, v->mime, v->w, v->h,
            (double)v->dur_us / 1e6);
    return v;
}

void kl_ffvid_close(kl_ffvid *v) {
    if (!v) return;
    pthread_mutex_lock(&v->mu);
    v->quit = 1;
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->mu);
    pthread_join(v->thr, NULL);
    if (v->hw_frames + v->sw_frames)
        fprintf(stderr, "  [ff] picture closed: %lu frames by VideoToolbox, %lu by FFmpeg\n",
                v->hw_frames, v->sw_frames);
    ff.close(v->f);
    pthread_mutex_destroy(&v->mu);
    pthread_mutex_destroy(&v->fmu);
    pthread_cond_destroy(&v->cv);
    free(v->pend);
    free(v->front);
    free(v);
}

int kl_ffvid_info(kl_ffvid *v, int *w, int *h, const char **mime,
                  long long *duration_us, float *fps) {
    if (!v) return 0;
    if (w) *w = v->w;
    if (h) *h = v->h;
    if (mime) *mime = v->mime;
    if (duration_us) *duration_us = v->dur_us;
    if (fps) *fps = v->fps;
    return 1;
}

void kl_ffvid_play(kl_ffvid *v, int on) {
    if (!v) return;
    pthread_mutex_lock(&v->mu);
    if (on && !v->playing) { v->base_t = ff_now_ms(); v->playing = 1; }
    else if (!on && v->playing) { v->base_pos = vid_pos_locked(v); v->playing = 0; }
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->mu);
}

void kl_ffvid_seek(kl_ffvid *v, long long ms) {
    if (!v) return;
    if (ms < 0) ms = 0;
    pthread_mutex_lock(&v->mu);
    v->seek_req = ms;
    v->base_pos = ms;
    v->base_t = ff_now_ms();
    v->complete = 0;
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->mu);
}

long long kl_ffvid_position_ms(kl_ffvid *v) {
    if (!v) return 0;
    pthread_mutex_lock(&v->mu);
    long long p = vid_pos_locked(v);
    pthread_mutex_unlock(&v->mu);
    return p;
}

int kl_ffvid_complete(kl_ffvid *v) {
    if (!v) return 0;
    pthread_mutex_lock(&v->mu);
    int c = v->complete;
    pthread_mutex_unlock(&v->mu);
    return c;
}

const void *kl_ffvid_frame(kl_ffvid *v, int *w, int *h, size_t *bytes,
                           unsigned long long *serial) {
    if (!v) return NULL;
    if (w) *w = v->fw;
    if (h) *h = v->fh;
    if (bytes) *bytes = v->front_bytes;
    if (serial) *serial = v->serial;
    return v->front;
}
void kl_ffvid_frame_lock(kl_ffvid *v) { if (v) pthread_mutex_lock(&v->fmu); }
void kl_ffvid_frame_unlock(kl_ffvid *v) { if (v) pthread_mutex_unlock(&v->fmu); }

// ---- sound ------------------------------------------------------------------
//
// kl_avdemux's audio cursor: time_us names the chunk under the cursor (-1 at
// the end), read copies it, advance steps over it. A chunk is at most 1024
// PCM frames, the unit kl_bsplayer.c's pump reads in. Called under that
// pump's lock, so nothing here locks.

#define FFA_CHUNK 1024

struct kl_ffaud {
    klff *f;
    int rate, ch;
    const short *buf;            // the decoder's current piece
    int frames, off;             // its length and the cursor in it
    long long pts;               // of buf[0], us
    int eof;
    int last;                    // frames the last read handed out
};

kl_ffaud *kl_ffaud_open(const char *path) {
    if (!kl_ff_available()) return NULL;
    char e[256] = "";
    // How long the open took and what it fetched from a server: a 4K film
    // on pCloud held the game ~8 s here (2026-11-27), cause not yet seen.
    long long t0 = ff_now_ms();
    unsigned long rq0 = 0, rs0 = 0;
    unsigned long long by0 = 0;
    kl_remote_stats(&rq0, &by0, &rs0);
    klff *f = ff_open(path, KLFF_AUDIO, e, (int)sizeof e);
    unsigned long rq1 = 0, rs1 = 0;
    unsigned long long by1 = 0;
    kl_remote_stats(&rq1, &by1, &rs1);
    if (kl_remote_is(path))
        fprintf(stderr, "  [ff] sound open: %lld ms, %lu request(s), %.1f MB from the server\n",
                ff_now_ms() - t0, rq1 - rq0, (double)(by1 - by0) / 1e6);
    if (!f) {
        fprintf(stderr, "  [ff] %s: no sound (%s)\n", path, e);
        return NULL;
    }
    kl_ffaud *a = calloc(1, sizeof *a);
    if (!a) { ff.close(f); return NULL; }
    a->f = f;
    char codec[32] = "";
    ff.info(f, NULL, NULL, NULL, NULL, codec, (int)sizeof codec, &a->rate, &a->ch);
    fprintf(stderr, "  [ff] sound: %s, %d Hz, %d ch\n", codec, a->rate, a->ch);
    return a;
}

void kl_ffaud_close(kl_ffaud *a) {
    if (!a) return;
    ff.close(a->f);
    free(a);
}

int kl_ffaud_info(kl_ffaud *a, int *rate, int *channels) {
    if (!a) return 0;
    if (rate) *rate = a->rate;
    if (channels) *channels = a->ch;
    return 1;
}

// A piece under the cursor, decoding the next when the current one is used up.
static int ffa_fill(kl_ffaud *a) {
    while (!a->eof && (!a->buf || a->off >= a->frames)) {
        const short *p = NULL;
        int n = 0;
        long long pts = 0;
        int rc = ff.audio_next(a->f, &p, &n, &pts);
        if (rc != 1) { a->eof = 1; a->buf = NULL; break; }
        // A piece without a time follows on from the last one.
        long long next = a->buf ? a->pts + (long long)a->frames * 1000000 / (a->rate ? a->rate : 48000) : 0;
        a->pts = pts >= 0 ? pts : next;
        a->buf = p;
        a->frames = n;
        a->off = 0;
    }
    return !a->eof;
}

long long kl_ffaud_time_us(kl_ffaud *a) {
    if (!a || !ffa_fill(a)) return -1;
    return a->pts + (long long)a->off * 1000000 / (a->rate ? a->rate : 48000);
}

long kl_ffaud_read(kl_ffaud *a, unsigned char *buf, unsigned long cap) {
    if (!a || !buf || !ffa_fill(a)) return -1;
    int fb = 2 * a->ch;
    int n = a->frames - a->off;
    if (n > FFA_CHUNK) n = FFA_CHUNK;
    if ((unsigned long)n * (unsigned long)fb > cap) n = (int)(cap / (unsigned long)fb);
    memcpy(buf, a->buf + (size_t)a->off * (size_t)a->ch, (size_t)n * (size_t)fb);
    a->last = n;
    return (long)n * fb;
}

int kl_ffaud_advance(kl_ffaud *a) {
    if (!a) return 0;
    a->off += a->last ? a->last : FFA_CHUNK;
    a->last = 0;
    return ffa_fill(a);
}

int kl_ffaud_tracks(kl_ffaud *a, char *buf, size_t cap) {
    if (!a || !buf || !cap || !ff.audio_tracks) return -1;
    return ff.audio_tracks(a->f, buf, cap > 0x7fffffff ? 0x7fffffff : (int)cap);
}

int kl_ffaud_select(kl_ffaud *a, int nth) {
    if (!a || !ff.audio_select) return -1;
    if (ff.audio_select(a->f, nth) != 0) return -1;
    char codec[32] = "";
    ff.info(a->f, NULL, NULL, NULL, NULL, codec, (int)sizeof codec, &a->rate, &a->ch);
    fprintf(stderr, "  [ff] sound track %d: %s, %d Hz, %d ch\n", nth, codec, a->rate, a->ch);
    a->buf = NULL;
    a->frames = a->off = a->last = 0;
    a->eof = 0;
    return 0;
}

int kl_ffaud_seek_us(kl_ffaud *a, long long us) {
    if (!a) return -1;
    int rc = ff.seek(a->f, us);
    a->buf = NULL;
    a->frames = a->off = a->last = 0;
    a->eof = 0;
    return rc;
}
