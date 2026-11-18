// Bigscreen Beta (x2497738113633933) initialises its media player while its
// loading screen runs and registers its handlers through
// BigMediaPlayerCallbacks.Register*Callback(String, String) — the DRM licence
// pair first, then the track and playback callbacks. The class declares twelve
// of these registrations in the title's dex. The first ended the run by name
// (the loading screen, then straight back to the Home screen, which is how
// this title was seen); the second stopped the next run one call later. There
// is no Android media service and no DRM licence stream on this host to report
// any of these events from, so every registration is accepted — as the
// WebView3D settings beside it are — and nothing is ever reported: no licence
// error, no track or playback change, and nothing about ownership, is
// invented. The rest of the block is taken in the same breath so the run
// reaches the surface behind the registrations instead of the next name in
// them; behind it the player manager hands its activity to
// BigMediaPlayerAPI.SetMediaPlayerContext — the context its media stack would
// be built around. This host has no media service to build one against, so
// the context is received and attached to nothing: the same answer the
// registrations get. The manager then creates its player and sets it up:
// CreateBigMediaPlayer names the first handle — 0 — one this host never
// builds a player behind, and InitiateMediaPlayer and the settings that
// follow (autoplay, debug, bitrate, height, the per-frame Update) are taken
// in the same breath, each accepted once and backing nothing. Next the app's
// own web-UI machinery comes up: com/bigscreenvr/webview/BigWebView wraps
// android/webkit/WebView, and its context hand-off is answered exactly as
// the media one was — received and attached to nothing. No browser is
// embedded on this host (the android/webkit/WebView family the wrapper
// speaks to records documents and draws nothing), so the client
// CreateBigWebClient names is a handle in the guest's bookkeeping only and
// SetupClient is accepted in the same breath. The wrapper's own native
// library (libbig-web-view) then starts its pump and the game starts asking
// after state: Render every frame, IsFullScreen for a view that is never
// shown, the pointer, scroll and cancellation events a panel would take,
// and the fullscreen exit. None of it has a browser to run against here —
// the pump is accepted and draws nothing, IsFullScreen answers false
// (nothing is drawn, so nothing is ever fullscreen), and the input is
// taken and dropped. Last the texture the panels would be drawn with: the
// Java body mints one external texture (glGenTextures, LINEAR/CLAMP) and
// keeps its name in a field that reads -1 until then. That is transcribed
// here — one real, empty texture in the app's own GL context, handed out
// by name, with nothing ever attached to it so a panel samples it empty —
// and the getter mirrors the field: -1 until the mint has run. Nothing
// renders and nothing reports through any of it. When the loading screen
// ends the app updates its environment's media source and drives the player
// behind it: the run named SetCurrentTime one call after that update and
// ended there (the loading screen, its end, then the environment transition,
// which is how this title was seen; on device, 2026-10-16). Since 2026-10-24
// the handle-0 LOCAL-FILE player backs a real kl_bsplayer session (open on
// StartVideoFromLocalFile(EGL), pump on Update, transport/seek/volume live;
// stream/RTSP/protected spellings stay accepted-and-backing-nothing), and the
// mirror hook in kl_ovrp.c composites its frames onto the film quad. For
// that one title, and for these calls.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "guest/kl_driver.h"
#include "klepton.h"
#include "kl_env.h"
#include "kl_egl.h"
#include "kl_ovrp.h"
#include "kl_jni_int.h"
#include "kl_bsplayer.h"
#include "kl_jni_publicdir.h"
#include "../media/kl_audio.h"
#include "../media/kl_bsfbs.h"

#define BIGSCREEN_CALLBACKS "com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks"
#define BIGSCREEN_REGISTER  "(Ljava/lang/String;Ljava/lang/String;)V"
#define BIGSCREEN_API       "com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI"
#define BIGSCREEN_WEBVIEW   "com/bigscreenvr/webview/BigWebView"

// Film sound -> the platform mixer (kl_audio multi-source: the film mixes
// with the room's own UI sounds instead of stealing the output). The tag is
// the session pointer: stable from open to close, unique per film.
static int bs_sink_open(void *ctx, unsigned rate, unsigned ch) {
    return kl_audio_open_src_fmt(ctx, "bsfilm", rate, ch, KL_AUDIO_S16);
}
static size_t bs_sink_write(void *ctx, const void *pcm, size_t bytes) {
    return kl_audio_write_src(ctx, pcm, bytes);
}
static void bs_sink_play(void *ctx) { kl_audio_play_src(ctx); }
static void bs_sink_pause(void *ctx) { kl_audio_pause_src(ctx); }
static void bs_sink_flush(void *ctx) { kl_audio_flush_src(ctx); }
static void bs_sink_close(void *ctx) { kl_audio_close_src(ctx); }
static const kl_bsplayer_audio_sink g_bs_sink = {
    bs_sink_open, bs_sink_write, bs_sink_play,
    bs_sink_pause, bs_sink_flush, bs_sink_close,
};

// ---- Local-file session (film playback from Documents/android-files/Movies/)
//
// Bigscreen names its film with StartVideoFromLocalFile(EGL) (I,String[,Surface]);
// the path is a guest spelling ("/storage/emulated/0/Movies/…") that the libc
// shim maps onto the container. One session at a time (handle 0, as the guest
// counts them): a second Start closes the first. Transport and Update drive
// the kl_bsplayer session; the newest decoded frame is kept here as BGRA and
// the mirror hook in kl_ovrp.c uploads it into the film quad's texture, from
// which the normal mirror pass composites it — no new compositor path.
static kl_bsplayer *g_film;
static pthread_mutex_t g_film_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_film_bgla;
static size_t g_film_bgla_n;
static int g_film_w, g_film_h;
static unsigned long long g_film_serial;
static int g_film_ready, g_film_playing, g_film_ended;

static void bs_film_event(void *ctx, kl_bsplayer_event ev) {
    (void)ctx;
    if (ev == KL_BS_EV_ENDED) g_film_ended = 1;
}

static int bs_film_upload(void *ctx, const unsigned char *bgra, int w, int h,
                          unsigned long long serial) {
    (void)ctx;
    pthread_mutex_lock(&g_film_mu);
    size_t n = (size_t)w * h * 4;
    if (n != g_film_bgla_n) {
        free(g_film_bgla);
        g_film_bgla = n ? malloc(n) : NULL;
        g_film_bgla_n = g_film_bgla ? n : 0;
    }
    if (g_film_bgla && n && n <= g_film_bgla_n) {
        memcpy(g_film_bgla, bgra, n);
        g_film_w = w;
        g_film_h = h;
        g_film_serial = serial;
        KLJ_LOG_ONCE("BigMediaPlayerAPI — first film frame latched %dx%d (serial %llu)",
                     w, h, serial);
    }
    pthread_mutex_unlock(&g_film_mu);
    return (g_film_bgla || n == 0) ? 0 : -1;
}

// Newest held frame without consuming it (the mirror hook polls every frame
// but uploads each serial once). NULL when none was ever latched; the buffer
// stays owned here and valid until the next latched frame.
const unsigned char *kl_bigscreen_film_frame(int *w, int *h,
                                             unsigned long long *serial) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (serial) *serial = 0;
    if (!g_film) return NULL;
    pthread_mutex_lock(&g_film_mu);
    const unsigned char *f = g_film_bgla;
    if (f) {
        if (w) *w = g_film_w;
        if (h) *h = g_film_h;
        if (serial) *serial = g_film_serial;
    }
    pthread_mutex_unlock(&g_film_mu);
    return f;
}

// ---- Callback endpoints (UnitySendMessage targets) ----
//
// Register*(gameobject, function) stores an API$BigCallback; the real Java
// layer forwards events via UnityPlayer.UnitySendMessage(gameobject, function,
// payload) — MediaInfoSuccess carries a base64 FlatBuffer, the rest an empty
// message. Observed registrations pass (name, null), replicated verbatim.
typedef struct { char *go; char *fn; } bs_cb;
static bs_cb g_cb_success, g_cb_failed, g_cb_ready,
    g_cb_playing, g_cb_ended, g_cb_error;

static void bs_cb_store(bs_cb *slot, const char *go,
                        const char *fn, const char *what) {
    free(slot->go);
    free(slot->fn);
    slot->go = go ? strdup(go) : NULL;
    slot->fn = fn ? strdup(fn) : NULL;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.%s(\"%s\", \"%s\")", what,
                 go ? go : "(null)", fn ? fn : "(null)");
}

typedef void (*bs_send_fn)(void *, void *, void *, void *, void *);
static bs_send_fn bs_guest_send;
static int bs_guest_send_resolved;

static void bs_send(void *env, bs_cb *slot, const char *msg, const char *what) {
    if (!bs_guest_send_resolved) {
        bs_guest_send_resolved = 1;
        bs_guest_send = klj_find_guest_native("com/unity3d/player/UnityPlayer",
                                              "nativeUnitySendMessage");
        KLJ_LOG_ONCE("BigMediaPlayerAPI — guest nativeUnitySendMessage %s",
                     bs_guest_send ? "resolved" : "MISSING (callbacks stay silent)");
    }
    if (!bs_guest_send || !slot->go) {
        KLJ_LOG_ONCE("BigMediaPlayerAPI — %s not fired (no endpoint)", what);
        return;
    }
    size_t n = msg ? strlen(msg) : 0;
    void *arr = klj_new_array('B', NULL, (int)n);
    klj_array *ad = arr ? klj_arr(arr) : NULL;
    if (ad && ad->data && n) memcpy(ad->data, msg, n);
    bs_guest_send(env, NULL, kl_jni_new_string(slot->go),
                  kl_jni_new_string(slot->fn ? slot->fn : ""), arr);
    KLJ_LOG("BigMediaPlayerAPI — %s fired to (\"%s\", %zu B)", what, slot->go, n);
}

// ---- MediaInfo flow (file UI: duration, thumbnail, Play) ----
//
// RequestMediaInfo(uri, position, width, height) hands out an incrementing id
// (the real code caches by key uri_pos_w_h and re-fires synchronously on a
// hit). On a miss a detached worker demuxes facts + decodes a thumbnail frame
// into BGRA; the frame-boundary thread (which holds GL) uploads it; the Update
// pump encodes the MediaInfo flatbuffer and fires it via UnitySendMessage.
// Anything missing (demux fail, no GL, no endpoint) degrades to
// MediaInfoFailedCallback(id) — never silence, never a guess.
struct bsinfo {
    int id;
    char key[1024];
    char host[1024];
    float position;
    int cw, ch;                 // clamped thumbnail size
    long long dur_ms;
    int mw, mh;                 // media size
    unsigned char *thumb;
    int tw, th;
    uint32_t tex;
    int fired;
    int failed;
    char *payload;              // fired base64, for cache re-fire
    struct bsinfo *next;
};
static struct bsinfo *g_infos;
static pthread_mutex_t g_infos_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_next_info_id;

static struct bsinfo *bsinfo_by_key(const char *key) {
    for (struct bsinfo *e = g_infos; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

static void *bsinfo_worker(void *arg) {
    struct bsinfo *e = arg;
    char reason[256];
    kl_bsplayer *p = kl_bsplayer_open(e->host, reason, sizeof reason);
    if (!p) {
        pthread_mutex_lock(&g_infos_mu);
        e->failed = 1;
        pthread_mutex_unlock(&g_infos_mu);
        KLJ_LOG("BigMediaPlayerAPI — info worker: cannot open \"%s\": %s",
                e->host, reason);
        return NULL;
    }
    int w = 0, h = 0;
    const char *mime = NULL;
    long long dur = 0;
    kl_bsplayer_info(p, &w, &h, &mime, &dur);
    if (w > 0 && h > 0) {
        long long target = (long long)(e->position * (float)dur);
        if (target < 0) target = 0;
        if (target > dur) target = dur;
        kl_bsplayer_seek_to_us(p, target);
    }
    unsigned char *bgra = NULL;
    int fw = 0, fh = 0;
    int rc = kl_bsplayer_decode_first(p, &bgra, &fw, &fh);
    kl_bsplayer_close(p);
    // The guest builds its Texture2D at the requested clamped size, so scale
    // here — same contract as the original Java layer's
    // createScaledBitmap(..., clampedWidth, clampedHeight, true). Uploading
    // media resolution would still sample correctly, but reports a size the
    // texture does not have and costs ~8 MB per thumbnail.
    int ow = e->cw, oh = e->ch;
    if (rc == 0 && bgra && fw > 0 && fh > 0 &&
        (fw != e->cw || fh != e->ch)) {
        unsigned char *scaled = malloc((size_t)e->cw * (size_t)e->ch * 4);
        if (scaled && kl_bsinfo_scale_bgra(bgra, fw, fh, scaled,
                                           e->cw, e->ch) == 0) {
            free(bgra);
            bgra = scaled;
            fw = e->cw;
            fh = e->ch;
        } else {
            // Scale failed (OOM): keep the frame, but report the size that
            // is actually uploaded rather than a lie.
            free(scaled);
            ow = fw;
            oh = fh;
            KLJ_LOG("BigMediaPlayerAPI — info worker: scale to %dx%d failed, "
                    "using native %dx%d", e->cw, e->ch, fw, fh);
        }
    }
    // KL_BS_THUMB_TEST=1 replaces the decoded frame with a coordinate ramp.
    // The upload is proven byte-exact (KL_BS_THUMB_CHECK) yet the cards still
    // stripe, so the open question is what the GUEST does with the texture
    // after the id is handed over — and a ramp answers it from the screen:
    // one smooth u/v ramp means it samples correctly, repeats mean it tiles,
    // a red/green swap means the channels are transposed (2026-10-31).
    if (kl_env_on("KL_BS_THUMB_TEST", 0)) {
        int pw = e->cw, ph = e->ch;
        unsigned char *ramp = malloc((size_t)pw * (size_t)ph * 4);
        if (ramp && kl_bsinfo_test_pattern(ramp, pw, ph) == 0) {
            free(bgra);
            bgra = ramp;
            fw = pw;
            fh = ph;
            KLJ_LOG("BigMediaPlayerAPI — info worker: KL_BS_THUMB_TEST ramp "
                    "%dx%d instead of the decoded frame", pw, ph);
        } else {
            free(ramp);
            KLJ_LOG("BigMediaPlayerAPI — info worker: KL_BS_THUMB_TEST ramp "
                    "alloc failed, keeping the frame");
        }
    }
    pthread_mutex_lock(&g_infos_mu);
    if (rc == 0 && bgra) {
        e->thumb = bgra;
        e->tw = fw;
        e->th = fh;
        e->cw = ow;
        e->ch = oh;
        e->dur_ms = dur / 1000;
        e->mw = w;
        e->mh = h;
    } else {
        free(bgra);
        e->failed = 1;
        KLJ_LOG("BigMediaPlayerAPI — info worker: no frame for \"%s\"", e->host);
    }
    pthread_mutex_unlock(&g_infos_mu);
    return NULL;
}

// Frame-boundary step (GL is current here): upload the thumbnail of the first
// entry that has pixels but no texture yet. Every entry gets its own GL
// texture — the guest wraps the id in a per-entry Texture2D, so sharing one
// id made every clip alias the last upload (all thumbnails showed Sintel's
// snow frame, 2026-10-30).
void kl_bigscreen_info_boundary(void) {
    pthread_mutex_lock(&g_infos_mu);
    struct bsinfo *e = NULL;
    for (struct bsinfo *x = g_infos; x; x = x->next)
        if (x->thumb && !x->tex && !x->failed) { e = x; break; }
    unsigned char *bgra = e ? e->thumb : NULL;
    int w = e ? e->tw : 0, h = e ? e->th : 0;
    int id = e ? e->id : 0;
    pthread_mutex_unlock(&g_infos_mu);
    if (!e) return;
    uint32_t tex = klovrp_bsinfo_thumb_upload(bgra, w, h, id);
    if (tex) {
        pthread_mutex_lock(&g_infos_mu);
        e->tex = tex;
        free(e->thumb);   // the texture owns the pixels now
        e->thumb = NULL;
        pthread_mutex_unlock(&g_infos_mu);
    }
}

static void bsinfo_fire(void *env, struct bsinfo *e) {
    unsigned char *fbs = NULL;
    size_t n = 0;
    kl_bsinfo_encode(e->id, e->dur_ms, e->mw, e->mh, e->cw, e->ch,
                     (int)e->tex, &fbs, &n);
    char *b64 = fbs ? kl_bsinfo_base64(fbs, n) : NULL;
    free(fbs);
    if (b64) {
        free(e->payload);
        e->payload = b64;
        e->fired = 1;
        KLJ_LOG_ONCE("BigMediaPlayerAPI — MediaInfo id %d payload: %s", e->id, b64);
        bs_send(env, &g_cb_success, b64, "MediaInfoSuccessCallback");
    } else if (!e->failed) {
        e->failed = 1;
    }
}

// Update-pump half: fire whatever is due. Entries with a texture fire success;
// failed entries fire MediaInfoFailedCallback once.
static void bsinfo_pump(void *env) {
    pthread_mutex_lock(&g_infos_mu);
    struct bsinfo *fire_ok = NULL, *fire_fail = NULL;
    for (struct bsinfo *e = g_infos; e; e = e->next) {
        if (!e->fired && !e->failed && e->tex) fire_ok = e;
        if (e->failed && !e->fired) fire_fail = e;
    }
    pthread_mutex_unlock(&g_infos_mu);
    // One per pump to keep ordering deterministic; the guest re-polls.
    if (fire_ok) {
        bsinfo_fire(env, fire_ok);
        return;
    }
    if (fire_fail) {
        fire_fail->fired = 1;
        char num[32];
        snprintf(num, sizeof num, "%d", fire_fail->id);
        KLJ_LOG("BigMediaPlayerAPI — MediaInfoFailedCallback(%d)", fire_fail->id);
        bs_send(env, &g_cb_failed, num, "MediaInfoFailedCallback");
    }
}

static klj_val media_request_media_info(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    // argv[0] is the uri here: unlike the (I,...) calls beside this one there
    // is no leading handle, so the first signature arg is at index 0 (reading
    // a[1] took the float bits as a pointer and faulted in klj_str, 2026-10-26).
    const char *uri = n > 0 && a[0].l ? klj_str(a[0].l) : "";
    float pos = n > 1 ? (float)a[1].d : 0;
    int w = n > 2 ? (int)a[2].j : 0;
    int h = n > 3 ? (int)a[3].j : 0;
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    int cw = w > 2048 ? 2048 : w < 100 ? 100 : w;
    int ch = h > 2048 ? 2048 : h < 100 ? 100 : h;
    char key[1024];
    snprintf(key, sizeof key, "%s_%g_%d_%d", uri, (double)pos, w, h);
    pthread_mutex_lock(&g_infos_mu);
    struct bsinfo *hit = bsinfo_by_key(key);
    if (hit && hit->payload) {
        int id = hit->id;
        char *again = strdup(hit->payload);
        pthread_mutex_unlock(&g_infos_mu);
        KLJ_LOG("BigMediaPlayerAPI.RequestMediaInfo — cache hit id %d", id);
        bs_send(env, &g_cb_success, again, "MediaInfoSuccessCallback(cached)");
        free(again);
        return (klj_val){.j = (uint64_t)id};
    }
    struct bsinfo *e = calloc(1, sizeof *e);
    int id = 0;
    if (e) {
        id = ++g_next_info_id;
        snprintf(e->key, sizeof e->key, "%s", key);
        char kp[1024];
        const char *host = kl_guest_path(uri, kp, sizeof kp);
        snprintf(e->host, sizeof e->host, "%s", host ? host : "");
        e->id = id;
        e->position = pos;
        e->cw = cw;
        e->ch = ch;
        e->next = g_infos;
        g_infos = e;
        KLJ_LOG("BigMediaPlayerAPI.RequestMediaInfo(\"%s\", %f, %d, %d) — id %d",
                uri, (double)pos, w, h, id);
    }
    pthread_mutex_unlock(&g_infos_mu);
    if (e) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, bsinfo_worker, e) != 0) {
            pthread_mutex_lock(&g_infos_mu);
            e->failed = 1;
            pthread_mutex_unlock(&g_infos_mu);
        }
        pthread_attr_destroy(&at);
    }
    return (klj_val){.j = (uint64_t)id};
}

static void bs_film_close(void) {
    if (!g_film) return;
    kl_bsplayer_close(g_film);
    g_film = NULL;
    g_film_ready = g_film_playing = g_film_ended = 0;
    KLJ_LOG("BigMediaPlayerAPI — film session closed");
}

static void bs_film_open(const char *path) {
    bs_film_close();
    if (!path || !*path) return;
    // Guest spelling ("/storage/emulated/0/Movies/…") names nothing on the
    // host: translate to the container path first, exactly as UE4's
    // MediaPlayer14 does (kl_jni_ue4.c). Without this the open refused with
    // "no such file" and the guest stayed in its mark-only state (2026-10-24).
    char kp[1024];
    const char *host = kl_guest_path(path, kp, sizeof kp);
    char reason[256];
    g_film = kl_bsplayer_open(host, reason, sizeof reason);
    if (!g_film) {
        KLJ_LOG("BigMediaPlayerAPI — film session refused \"%s\" (host \"%s\"): %s",
                path, host, reason);
        return;
    }
    kl_bsplayer_set_uploader(g_film, bs_film_upload, NULL);
    kl_bsplayer_set_listener(g_film, bs_film_event, NULL);
    kl_bsplayer_set_audio_sink(g_film, &g_bs_sink, g_film);
    g_film_ready = g_film_playing = g_film_ended = 0;
    int w = 0, h = 0;
    const char *mime = NULL;
    long long dur = 0;
    kl_bsplayer_info(g_film, &w, &h, &mime, &dur);
    KLJ_LOG("BigMediaPlayerAPI — film session open \"%s\": %s %dx%d, %lld us",
            path, mime ? mime : "?", w, h, dur);
    kl_bsplayer_play(g_film);
}


// One accepter per registration: the call stores the guest's handler proxy,
// and this host has no media service to ever raise it. Each says so once, by
// name, in the boot log; all of them return void.
static klj_val register_drm_error_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.RegisterDRMLicenseErrorCallback — registration accepted (no DRM licence stream on this host)");
    return (klj_val){0};
}
static klj_val register_expired_drm_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.RegisterExpiredDRMLicenseCallback — registration accepted (no DRM licence stream on this host)");
    return (klj_val){0};
}
static klj_val register_audio_tracks_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.RegisterAudioTracksChangedCallback — registration accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val register_media_info_failed_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_failed,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterMediaInfoFailedCallback");
    return (klj_val){0};
}
static klj_val register_media_info_success_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_success,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterMediaInfoSuccessCallback");
    return (klj_val){0};
}
static klj_val register_playback_buffering_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.RegisterPlaybackBufferingCallback — registration accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val register_playback_ended_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_ended,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterPlaybackEndedCallback");
    return (klj_val){0};
}
static klj_val register_playback_error_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_error,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterPlaybackErrorCallback");
    return (klj_val){0};
}
static klj_val register_playback_ready_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_ready,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterPlaybackReadyCallback");
    return (klj_val){0};
}
static klj_val register_playing_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_playing,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterPlayingCallback");
    return (klj_val){0};
}
static klj_val register_subtitle_cue_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.RegisterSubtitleCueCallback — registration accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val register_text_tracks_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerCallbacks.RegisterTextTracksChangedCallback — registration accepted (no media session on this host)");
    return (klj_val){0};
}

// The manager's context hand-off: the call would store which activity the
// player is built around. There is no media service on this host to build one
// against, so the context is received and attached to nothing — said once, as
// the registrations are.
static klj_val set_media_player_context(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.SetMediaPlayerContext — context received (no media session on this host to attach it to)");
    return (klj_val){0};
}

// The manager's player: created once, then set up. The handle that
// CreateBigMediaPlayer names selects a player only in the guest's own
// bookkeeping — no media service on this host builds one behind it — so the
// first handle, 0, is handed out and the initiation, settings and per-frame
// pump beside it are accepted the way the registrations are: said once each,
// backing nothing, reporting nothing.
static klj_val create_big_media_player(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.CreateBigMediaPlayer — player handle 0 handed out (no media session on this host behind it)");
    return (klj_val){.j = 0};
}
static klj_val initiate_media_player(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.InitiateMediaPlayer — initiation accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val set_autoplay(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.SetAutoplay — setting accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val set_debug_mode(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.SetDebugMode — setting accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val set_maximum_video_bitrate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.SetMaximumVideoBitrate — setting accepted (no media session on this host)");
    return (klj_val){0};
}
static klj_val set_maximum_video_height(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.SetMaximumVideoHeight — setting accepted (no media session on this host)");
    return (klj_val){0};
}
// (Real RequestMediaInfo lives with the MediaInfo flow above; this probe is
// superseded and removed to keep one definition.)
static klj_val media_is_frame_available(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int handle = n > 0 ? (int)a[0].j : -1;
    // The guest polls this after Start until the first frame is ready, then
    // builds its player UI (duration, Play) around it: 775 polls answered 0
    // kept the 2026-10-24 session in its mark-only state. True once the pump
    // has latched a frame. The handle is NOT gated: the guest probes with -1
    // (device log 2026-10-24) and every transport call beside this one already
    // ignores handles — this host holds exactly one film session.
    int avail = 0;
    if (g_film) {
        pthread_mutex_lock(&g_film_mu);
        avail = g_film_bgla != NULL && g_film_serial != 0;
        pthread_mutex_unlock(&g_film_mu);
    }
    static int last = -1;
    if (avail != last) {
        last = avail;
        KLJ_LOG("BigMediaPlayerAPI.IsFrameAvailable(handle %d) — %d", handle, avail);
    }
    return (klj_val){.j = (uint64_t)(avail != 0)};
}
static klj_val update_media_players(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // The per-frame pump: advance events and latch the newest decoded frame
    // for the mirror hook (kl_ovrp.c). All guest calls here arrive on the
    // Unity thread; the frame buffer itself is mutex-guarded.
    KLJ_LOG_ONCE("BigMediaPlayerAPI.Update — pump running");
    if (g_film) {
        kl_bsplayer_update(g_film);
        kl_bsplayer_present(g_film);
        // Playback events in real order: ready once the first frame is
        // latched, playing on the next pump, ended when the stream runs out.
        int w = 0, h = 0;
        unsigned long long serial = 0;
        if (!g_film_ready && kl_bigscreen_film_frame(&w, &h, &serial) && serial) {
            g_film_ready = 1;
            KLJ_LOG("BigMediaPlayerAPI — first film frame latched, firing PlaybackReady");
            bs_send(env, &g_cb_ready, "", "PlaybackReadyCallback");
        } else if (g_film_ready && !g_film_playing) {
            g_film_playing = 1;
            bs_send(env, &g_cb_playing, "", "PlayingCallback");
        }
        if (g_film_ended) {
            g_film_ended = 0;
            bs_send(env, &g_cb_ended, "", "PlaybackEndedCallback");
        }
    }
    bsinfo_pump(env);
    return (klj_val){0};
}

// Duration of the open session in ms (the C# timeline polls this; 0 before
// the first open).
static klj_val media_get_duration(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    long long ms = 0;
    if (g_film) {
        int w = 0, h = 0;
        long long dur = 0;
        kl_bsplayer_info(g_film, &w, &h, NULL, &dur);
        if (dur > 0) ms = dur / 1000;
    }
    return (klj_val){.j = (uint64_t)ms};
}

// ---- Room A/V sync, as the guest sees it (KL_BS_TRACE=1) ----
//
// The guest polls GetCurrentPosition whenever the player's timeline is open —
// ~6 calls a second in the 14:57 run, and 0 in a run that never opens a player
// (01:08) — so in a room the same loop is the one that would correct a drift
// between two headsets. These lines are the answer to "did it correct, or did
// it never look": a solo run's timeline polls too, which is why the trace is a
// line a second rather than every call.
// A seek is the correction: kl_bsplayer_seek_to_us moves picture AND sound, so
// a room-level correction cannot leave this headset locally out of sync.
static int bs_trace_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_BS_TRACE", 0);
    return on;
}

static long long bs_trace_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The poll is the one call that can arrive per frame; one line a second is
// enough to show the loop is alive and where it thinks the film is.
static void bs_trace_poll(long long pos_ms) {
    if (!bs_trace_on()) return;
    static long long last;
    long long now = bs_trace_now_ms();
    if (last && now - last < 1000) return;
    last = now;
    KLJ_LOG("BigMediaPlayerAPI.GetCurrentPosition — %lld ms (room sync polling)", pos_ms);
}

static void bs_trace_ask(const char *what, long long target_ms) {
    if (!bs_trace_on()) return;
    long long pos = g_film ? kl_bsplayer_position_ms(g_film) : 0;
    KLJ_LOG("BigMediaPlayerAPI.%s — room asked for %lld ms, picture was %lld ms (%+lld ms)",
            what, target_ms, pos, target_ms - pos);
}


// Position of the open session in ms (timeline + seek slider; 0 idle).
static klj_val media_get_current_position(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    long long ms = g_film ? kl_bsplayer_position_ms(g_film) : 0;
    if (ms < 0) ms = 0;
    bs_trace_poll(ms);
    return (klj_val){.j = (uint64_t)ms};
}

// The C# video quad is sized from these two, not from GetVideoSize: answering
// 0 left a degenerate quad and the film never painted despite a live texture.
static klj_val media_get_video_texture_width(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int w = 0, h = 0;
    if (g_film) kl_bsplayer_info(g_film, &w, &h, NULL, NULL);
    if (w < 0) w = 0;
    return (klj_val){.j = (uint64_t)w};
}
static klj_val media_get_video_texture_height(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int w = 0, h = 0;
    if (g_film) kl_bsplayer_info(g_film, &w, &h, NULL, NULL);
    if (h < 0) h = 0;
    return (klj_val){.j = (uint64_t)h};
}

// The guest samples the film itself: once IsFrameAvailable is true it asks
// every frame for the video size and the GL texture holding the newest frame
// (743 polls each in the 2026-10-24 session). Handles are ignored like
// everywhere else here — one film session. Size is {w, h} ({0, 0} before the
// first open); the texture is the shared film texture uploaded on each new
// serial, 0 until the first upload lands.
static klj_val media_get_video_size(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int w = 0, h = 0;
    if (g_film) kl_bsplayer_info(g_film, &w, &h, NULL, NULL);
    void *arr = klj_new_array('I', NULL, 2);
    klj_array *ad = arr ? klj_arr(arr) : NULL;
    if (ad && ad->data) {
        ((int *)ad->data)[0] = w > 0 ? w : 0;
        ((int *)ad->data)[1] = h > 0 ? h : 0;
    }
    KLJ_LOG_ONCE("BigMediaPlayerAPI.GetVideoSize — [%d, %d]", w, h);
    return (klj_val){.l = arr};
}
static klj_val media_get_video_texture_id(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int w = 0, h = 0;
    unsigned long long serial = 0;
    const unsigned char *bgra = kl_bigscreen_film_frame(&w, &h, &serial);
    uint32_t tex = 0;
    if (bgra && serial && w > 0 && h > 0) {
        tex = klovrp_bsfilm_upload(bgra, w, h, serial);
        if (tex) klovrp_bsfilm_note_guest_draws();
    }
    static uint32_t said;
    if (tex && tex != said) {
        said = tex;
        KLJ_LOG("BigMediaPlayerAPI.GetVideoTextureId — %u (%dx%d, serial %llu)",
                tex, w, h, serial);
    }
    return (klj_val){.j = (uint64_t)tex};
}

// The media driver's surface, reached as soon as the loading screen ends and
// the environment's source is updated: the seek first (which ended the run
// that reached it), the volume and transport controls beside it, the start
// calls in every spelling the app may pick from, and the shutdown. The
// player behind the handle is the one this host never builds, so each call
// is accepted once, by name, and backs nothing — nothing seeks, plays,
// stops or is released — and nothing is ever reported back through the
// callbacks the manager registered.
static klj_val media_set_current_time(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    double sec = n > 1 ? a[1].d : 0;
    bs_trace_ask("SetCurrentTime", (long long)(sec * 1e3));
    if (g_film) kl_bsplayer_seek_to_us(g_film, (long long)(sec * 1e6));
    KLJ_LOG("BigMediaPlayerAPI.SetCurrentTime — seek %.1fs", sec);
    return (klj_val){0};
}
static klj_val media_set_volume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    float v = n > 1 ? (float)a[1].d : 1;
    if (g_film) kl_bsplayer_set_volume(g_film, v);
    KLJ_LOG("BigMediaPlayerAPI.SetVolume — volume %.2f", v);
    return (klj_val){0};
}
static klj_val media_play(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (bs_trace_on() && g_film)
        KLJ_LOG("BigMediaPlayerAPI.Play — picture at %lld ms", kl_bsplayer_position_ms(g_film));
    if (g_film) kl_bsplayer_play(g_film);
    return (klj_val){0};
}
static klj_val media_pause(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (bs_trace_on() && g_film)
        KLJ_LOG("BigMediaPlayerAPI.Pause — picture at %lld ms", kl_bsplayer_position_ms(g_film));
    if (g_film) kl_bsplayer_pause(g_film);
    return (klj_val){0};
}
static klj_val media_resume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (bs_trace_on() && g_film)
        KLJ_LOG("BigMediaPlayerAPI.Resume — picture at %lld ms", kl_bsplayer_position_ms(g_film));
    if (g_film) kl_bsplayer_resume(g_film);
    return (klj_val){0};
}
static klj_val media_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (bs_trace_on() && g_film)
        KLJ_LOG("BigMediaPlayerAPI.Stop — picture at %lld ms", kl_bsplayer_position_ms(g_film));
    if (g_film) kl_bsplayer_stop(g_film);
    return (klj_val){0};
}
static klj_val media_shutdown(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    bs_film_close();
    return (klj_val){0};
}
static klj_val media_stop_and_release_all(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    bs_film_close();
    return (klj_val){0};
}
static klj_val media_start_stream(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.StartStream — start accepted (no media session on this host; no stream begins)");
    return (klj_val){0};
}
static klj_val media_start_video_from_local_file(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *path = n > 1 && a[1].l ? klj_str(a[1].l) : NULL;
    KLJ_LOG("BigMediaPlayerAPI.StartVideoFromLocalFile — path \"%s\"",
            path ? path : "(null)");
    bs_film_open(path);
    return (klj_val){0};
}
static klj_val media_start_video_from_local_file_egl(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *path = n > 1 && a[1].l ? klj_str(a[1].l) : NULL;
    KLJ_LOG("BigMediaPlayerAPI.StartVideoFromLocalFileEGL — path \"%s\"",
            path ? path : "(null)");
    bs_film_open(path);
    return (klj_val){0};
}
static klj_val media_start_rtsp_stream(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.StartRTSPStream — start accepted (no media session on this host; no stream begins)");
    return (klj_val){0};
}
static klj_val media_start_protected_media_session(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.StartProtectedMediaSession — start accepted (no DRM licence stream and no media session on this host)");
    return (klj_val){0};
}

// The wrapper's context hand-off, then its client: created and set up. The
// wrapper speaks to android/webkit/WebView, whose family on this host records
// documents and draws nothing — so the context is received and attached to
// nothing (the same answer the media context got), the client
// CreateBigWebClient names exists only in the guest's bookkeeping (handle 0,
// like the player), and SetupClient is accepted in the same breath with the
// non-drawing panel this host minted. Nothing is ever reported through any
// of it.
static klj_val set_webview_context(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.SetContext — context received (no browser is embedded on this host)");
    return (klj_val){0};
}
static klj_val create_big_web_client(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.CreateBigWebClient — web client handle 0 handed out (nothing renders on this host)");
    return (klj_val){.j = 0};
}
static klj_val setup_big_web_client(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.SetupClient — client accepted (no browser is embedded on this host)");
    return (klj_val){0};
}

// The wrapper's texture, the one the panels would be drawn with. The Java
// body mints it once — glGenTextures, then LINEAR/CLAMP on an external (OES)
// binding — and keeps its name in m_sharedTextureHandle, whose reading is -1
// until then; GetSharedTextureHandle is that field read, and Render opens
// with the mint. Transcribed the same way, through the gateway the guest's
// own GL comes from (kl_ovrp.c's pattern, including the no-context answer):
// the name is real and the texture is empty — nothing ever attaches an image
// to it, there is no browser to render into it — so a panel samples it
// empty. The guest's own binding is put back afterwards; this runs in the
// middle of its GL use. The texture is set up through TEXTURE_2D rather than
// the OES target: they are one binding point on this host (the guest's
// external binds land on 2D — klfb_detarget), and asking this backend for
// the OES target directly would only raise INVALID_ENUM.
static int g_shared_texture = -1;   // the wrapper's m_sharedTextureHandle

static void webview_ensure_shared_texture(void) {
    if (g_shared_texture != -1) return;    // the Java guard: once minted, kept
    if (!kl_egl_current_context()) {
        KLJ_LOG_ONCE("BigWebView.SetupExternalTexture — no GL context on this thread; the shared texture stays uncreated (-1)");
        return;
    }
    static void (*gl_GenTextures)(int32_t, uint32_t *);
    static void (*gl_BindTexture)(uint32_t, uint32_t);
    static void (*gl_TexParameteri)(uint32_t, uint32_t, int32_t);
    static void (*gl_GetIntegerv)(uint32_t, int32_t *);
    if (!gl_GenTextures) {
        gl_GenTextures   = kl_egl_sym("glGenTextures");
        gl_BindTexture   = kl_egl_sym("glBindTexture");
        gl_TexParameteri = kl_egl_sym("glTexParameteri");
        gl_GetIntegerv   = kl_egl_sym("glGetIntegerv");
    }
    uint32_t name = 0;
    if (gl_GenTextures) gl_GenTextures(1, &name);
    if (!name || !gl_BindTexture || !gl_TexParameteri) {
        KLJ_LOG_ONCE("BigWebView.SetupExternalTexture — glGenTextures produced no name; the shared texture stays uncreated (-1)");
        return;
    }
    int32_t prev = 0;
    if (gl_GetIntegerv) gl_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &prev);
    gl_BindTexture(0x0DE1 /* TEXTURE_2D */, name);
    gl_TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
    gl_TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2601);
    gl_TexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
    gl_TexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
    gl_BindTexture(0x0DE1, (uint32_t)prev);
    g_shared_texture = (int)name;
    KLJ_LOG_ONCE("BigWebView.SetupExternalTexture — shared external texture %d created (nothing renders into it on this host)", g_shared_texture);
}
static klj_val webview_setup_external_texture(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    webview_ensure_shared_texture();
    return (klj_val){0};
}
static klj_val webview_get_shared_texture(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (g_shared_texture == -1)
        KLJ_LOG_ONCE("BigWebView.GetSharedTextureHandle — -1 handed out (no shared texture has been created)");
    else
        KLJ_LOG_ONCE("BigWebView.GetSharedTextureHandle — %d handed out (the shared external texture; empty, nothing renders into it)", g_shared_texture);
    return (klj_val){.j = (uint64_t)g_shared_texture};
}

// The wrapper's pump and state/input surface, reached as soon as the client
// exists: libbig-web-view starts calling Render every frame, the game asks
// whether a view is fullscreen, and the pointer, scroll, cancellation and
// fullscreen-exit calls follow the panels. There is no browser to run
// against and no panel that is drawn, so each is accepted once and answers
// what this host can honestly say: nothing to draw, never fullscreen, and
// input with nowhere to land. Nothing is reported back.
static klj_val webview_render(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    webview_ensure_shared_texture();   // the Java Render()'s opening SetupExternalTexture()
    KLJ_LOG_ONCE("BigWebView.Render — pump accepted (no browser is embedded on this host; nothing to draw)");
    return (klj_val){0};
}
static klj_val webview_is_fullscreen(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.IsFullScreen — false (nothing renders on this host, so nothing is ever fullscreen)");
    return (klj_val){.j = 0};
}
static klj_val webview_pointer_move(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.PointerMove — pointer accepted (no panel is drawn on this host)");
    return (klj_val){0};
}
static klj_val webview_pointer_down(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.PointerDown — pointer accepted (no panel is drawn on this host)");
    return (klj_val){0};
}
static klj_val webview_pointer_drag(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.PointerDrag — pointer accepted (no panel is drawn on this host)");
    return (klj_val){0};
}
static klj_val webview_pointer_up(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.PointerUp — pointer accepted (no panel is drawn on this host)");
    return (klj_val){0};
}
static klj_val webview_scroll(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.Scroll — scroll accepted (no panel is drawn on this host)");
    return (klj_val){0};
}
static klj_val webview_cancel_input(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.CancelInput — cancellation accepted (no panel is drawn on this host)");
    return (klj_val){0};
}
static klj_val webview_exit_fullscreen(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.ExitFullScreen — accepted (nothing here is ever fullscreen)");
    return (klj_val){0};
}

// The registrations in the order the dex declares them, then the media
// manager's and web wrapper's calls beside them; the run takes whatever
// subset it reaches.
static const klj_binding bindings[] = {
    {BIGSCREEN_CALLBACKS, "RegisterDRMLicenseErrorCallback",     BIGSCREEN_REGISTER, register_drm_error_cb},
    {BIGSCREEN_CALLBACKS, "RegisterExpiredDRMLicenseCallback",   BIGSCREEN_REGISTER, register_expired_drm_cb},
    {BIGSCREEN_CALLBACKS, "RegisterAudioTracksChangedCallback",  BIGSCREEN_REGISTER, register_audio_tracks_cb},
    {BIGSCREEN_CALLBACKS, "RegisterMediaInfoFailedCallback",     BIGSCREEN_REGISTER, register_media_info_failed_cb},
    {BIGSCREEN_CALLBACKS, "RegisterMediaInfoSuccessCallback",    BIGSCREEN_REGISTER, register_media_info_success_cb},
    {BIGSCREEN_CALLBACKS, "RegisterPlaybackBufferingCallback",   BIGSCREEN_REGISTER, register_playback_buffering_cb},
    {BIGSCREEN_CALLBACKS, "RegisterPlaybackEndedCallback",       BIGSCREEN_REGISTER, register_playback_ended_cb},
    {BIGSCREEN_CALLBACKS, "RegisterPlaybackErrorCallback",       BIGSCREEN_REGISTER, register_playback_error_cb},
    {BIGSCREEN_CALLBACKS, "RegisterPlaybackReadyCallback",       BIGSCREEN_REGISTER, register_playback_ready_cb},
    {BIGSCREEN_CALLBACKS, "RegisterPlayingCallback",             BIGSCREEN_REGISTER, register_playing_cb},
    {BIGSCREEN_CALLBACKS, "RegisterSubtitleCueCallback",         BIGSCREEN_REGISTER, register_subtitle_cue_cb},
    {BIGSCREEN_CALLBACKS, "RegisterTextTracksChangedCallback",   BIGSCREEN_REGISTER, register_text_tracks_cb},
    // The player manager hands its activity to the player API. Unity's
    // reflected call spells the class name dotted, the way it read it; the
    // canonical JNI spelling is taken beside the observed one.
    {BIGSCREEN_API, "SetMediaPlayerContext", "(Lcom.unity3d.player.UnityPlayerActivity;)V", set_media_player_context},
    {BIGSCREEN_API, "SetMediaPlayerContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", set_media_player_context},
    // The manager's player: created, initiated, configured and pumped. Each
    // call selects a player that exists only in the guest's bookkeeping;
    // every one is accepted.
    {BIGSCREEN_API, "CreateBigMediaPlayer", "()I", create_big_media_player},
    {BIGSCREEN_API, "InitiateMediaPlayer", "(I)V", initiate_media_player},
    {BIGSCREEN_API, "SetAutoplay", "(IZ)V", set_autoplay},
    {BIGSCREEN_API, "SetDebugMode", "(Z)V", set_debug_mode},
    {BIGSCREEN_API, "SetMaximumVideoBitrate", "(II)V", set_maximum_video_bitrate},
    {BIGSCREEN_API, "SetMaximumVideoHeight", "(II)V", set_maximum_video_height},
    {BIGSCREEN_API, "Update", "()V", update_media_players},
    // Readiness probe the guest polls after Start until the first frame lands;
    // the player UI (duration, Play) is built around a true answer.
    {BIGSCREEN_API, "IsFrameAvailable", "(I)Z", media_is_frame_available},
    // Self-render surface: size and GL texture of the newest film frame.
    {BIGSCREEN_API, "GetVideoSize", "(I)[I", media_get_video_size},
    {BIGSCREEN_API, "GetVideoTextureId", "(I)I", media_get_video_texture_id},
    {BIGSCREEN_API, "GetDuration", "(I)J", media_get_duration},
    {BIGSCREEN_API, "GetCurrentPosition", "(I)J", media_get_current_position},
    {BIGSCREEN_API, "GetVideoTextureWidth", "(I)I", media_get_video_texture_width},
    {BIGSCREEN_API, "GetVideoTextureHeight", "(I)I", media_get_video_texture_height},
    {BIGSCREEN_API, "RequestMediaInfo", "(Ljava/lang/String;FII)I", media_request_media_info},
    // The drive surface beside those settings: the seek that ended the run
    // that reached it, the volume and transport controls, the starts in all
    // their spellings, and the shutdown. All void, all accepted once.
    {BIGSCREEN_API, "SetCurrentTime", "(IF)V", media_set_current_time},
    {BIGSCREEN_API, "SetVolume", "(IF)V", media_set_volume},
    {BIGSCREEN_API, "Play", "(I)V", media_play},
    {BIGSCREEN_API, "Pause", "(I)V", media_pause},
    {BIGSCREEN_API, "Resume", "(I)V", media_resume},
    {BIGSCREEN_API, "Stop", "(I)V", media_stop},
    {BIGSCREEN_API, "Shutdown", "(I)V", media_shutdown},
    {BIGSCREEN_API, "StopAndReleaseAll", "()V", media_stop_and_release_all},
    {BIGSCREEN_API, "StartStream", "(ILjava/lang/String;)V", media_start_stream},
    {BIGSCREEN_API, "StartStream", "(ILjava/lang/String;Ljava/lang/String;)V", media_start_stream},
    {BIGSCREEN_API, "StartStream", "(ILjava/lang/String;Ljava/lang/String;Landroid/view/Surface;)V", media_start_stream},
    {BIGSCREEN_API, "StartVideoFromLocalFile", "(ILjava/lang/String;)V", media_start_video_from_local_file},
    {BIGSCREEN_API, "StartVideoFromLocalFileEGL", "(ILjava/lang/String;)V", media_start_video_from_local_file_egl},
    {BIGSCREEN_API, "StartVideoFromLocalFileEGL", "(ILjava/lang/String;Landroid/view/Surface;)V", media_start_video_from_local_file_egl},
    {BIGSCREEN_API, "StartRTSPStream", "(ILjava/lang/String;)V", media_start_rtsp_stream},
    {BIGSCREEN_API, "StartProtectedMediaSession", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", media_start_protected_media_session},
    {BIGSCREEN_API, "StartProtectedMediaSession", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/view/Surface;)V", media_start_protected_media_session},
    // The web wrapper: its context, then the client it creates and sets up.
    // The browser behind it is not embedded here; the panel this host mints
    // records documents and draws nothing.
    {BIGSCREEN_WEBVIEW, "SetContext", "(Lcom.unity3d.player.UnityPlayerActivity;)V", set_webview_context},
    {BIGSCREEN_WEBVIEW, "SetContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", set_webview_context},
    {BIGSCREEN_WEBVIEW, "CreateBigWebClient", "()I", create_big_web_client},
    {BIGSCREEN_WEBVIEW, "SetupClient", "(ILandroid/webkit/WebView;II)V", setup_big_web_client},
    // The pump, the state question and the input the native library and the
    // game raise as soon as the client exists. Accepted like everything
    // else: nothing is drawn, so nothing is ever fullscreen and no event
    // has a panel to land on.
    {BIGSCREEN_WEBVIEW, "Render", "()V", webview_render},
    {BIGSCREEN_WEBVIEW, "IsFullScreen", "(I)Z", webview_is_fullscreen},
    {BIGSCREEN_WEBVIEW, "PointerMove", "(IIII)V", webview_pointer_move},
    {BIGSCREEN_WEBVIEW, "PointerDown", "(IIIIII)V", webview_pointer_down},
    {BIGSCREEN_WEBVIEW, "PointerDrag", "(IIIII)V", webview_pointer_drag},
    {BIGSCREEN_WEBVIEW, "PointerUp", "(IIIIII)V", webview_pointer_up},
    {BIGSCREEN_WEBVIEW, "Scroll", "(IIIIII)V", webview_scroll},
    {BIGSCREEN_WEBVIEW, "CancelInput", "(III)V", webview_cancel_input},
    {BIGSCREEN_WEBVIEW, "ExitFullScreen", "(I)V", webview_exit_fullscreen},
    // The texture the panels would be drawn with: minted once through the
    // guest's own GL, handed out by name, and left empty.
    {BIGSCREEN_WEBVIEW, "SetupExternalTexture", "()V", webview_setup_external_texture},
    {BIGSCREEN_WEBVIEW, "GetSharedTextureHandle", "()I", webview_get_shared_texture},
    {NULL, NULL, NULL, NULL}
};

const klj_binding *klj_bigscreen_binding(const char *cls, const char *name, const char *sig) {
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "x2497738113633933")) return NULL;
    for (const klj_binding *b = bindings; b->cls; b++)
        if (!strcmp(cls, b->cls) && !strcmp(name, b->name) && !strcmp(sig, b->sig)) return b;
    return NULL;
}
