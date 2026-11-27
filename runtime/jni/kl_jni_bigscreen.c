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
#include "../media/kl_web.h"
#include "kl_glfb.h"

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
    g_cb_playing, g_cb_ended, g_cb_error, g_cb_atracks, g_cb_ttracks, g_cb_subcue;
static int g_film_tracks_fired;     // AudioTracksChanged sent for this film
static int g_film_texts_fired;      // TextTracksChanged sent for this film
static int g_text_sel = -1;         // the subtitle track shown, -1 none

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
    kl_bsplayer *p = kl_bsplayer_open2(e->host, KL_BS_NO_SOUND, reason, sizeof reason);
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
    // The sound FFmpeg gives opens on the player's own thread: on pCloud it
    // took ~8 s of this (the game's) thread for a 4K film (2026-11-27).
    g_film = kl_bsplayer_open2(host, KL_BS_SOUND_ASYNC, reason, sizeof reason);
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
    // The Audio Tracks menu: FFmpeg reads the streams off the Unity thread
    // (a pCloud film is a few range requests away); the pump below says
    // AudioTracksChanged once the list is there and the picture is up.
    g_film_tracks_fired = 0;
    g_film_texts_fired = 0;
    g_text_sel = -1;
    kl_bsplayer_audio_tracks_prefetch(g_film);   // the subtitle list too
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
    (void)env; (void)self;
    bs_cb_store(&g_cb_atracks,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterAudioTracksChangedCallback");
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
    (void)env; (void)self;
    bs_cb_store(&g_cb_subcue,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterSubtitleCueCallback");
    return (klj_val){0};
}
static klj_val register_text_tracks_cb(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bs_cb_store(&g_cb_ttracks,
                n > 0 && a[0].l ? klj_str(a[0].l) : NULL,
                n > 1 && a[1].l ? klj_str(a[1].l) : NULL,
                "RegisterTextTracksChangedCallback");
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
static void bs_vuplex_pump(void *env);   // the browser probe, below

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
        // The Java player says this from ExoPlayer's onTracksChanged with an
        // empty message; the game then asks GetAudioTracks.
        if (g_film_ready && !g_film_tracks_fired &&
            kl_bsplayer_audio_tracks_peek(g_film, NULL, 0) >= 0) {
            g_film_tracks_fired = 1;
            bs_send(env, &g_cb_atracks, "", "AudioTracksChangedCallback");
        }
        if (g_film_ready && !g_film_texts_fired &&
            kl_bsplayer_text_tracks_peek(g_film, NULL, 0) >= 0) {
            g_film_texts_fired = 1;
            bs_send(env, &g_cb_ttracks, "", "TextTracksChangedCallback");
        }
        // Subtitle cues due at the picture: the Java player sends each cue of
        // ExoPlayer's onCues as a base64 SubtitleCue (start, length, text);
        // the game shows it for its length.
        static char cue[4096];
        long long cs = 0, ce = 0;
        for (int i = 0; i < 4 && kl_bsplayer_sub_poll(g_film, cue, sizeof cue, &cs, &ce); i++) {
            unsigned char *fb = NULL;
            size_t fbn = 0;
            char *b64 = NULL;
            if (kl_bscue_encode((int)(cs / 1000), (int)((ce - cs) / 1000), cue, &fb, &fbn) == 0)
                b64 = kl_bsinfo_base64(fb, fbn);
            free(fb);
            if (b64) bs_send(env, &g_cb_subcue, b64, "SubtitleCueCallback");
            free(b64);
        }
    }
    bsinfo_pump(env);
    if (klj_bigscreen_web_probe()) bs_vuplex_pump(env);
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
// The browser's input goes through this wrapper, not through Vuplex's own
// pointer calls: each panel's client (CreateBigWebClient) is tied to a web
// view by SetupClient(client, view.getNativeWebView(), w, h), and the
// pointer and scroll calls name the client. Below, in the browser section.
static void bsweb_client_setup(int client, void *native_view, int w, int h);
static void bsweb_client_input(int client, int kind, int x, int y, int button,
                               int dx, int dy, int target);
enum { BSW_MOVE, BSW_DOWN, BSW_DRAG, BSW_UP, BSW_SCROLL, BSW_CANCEL };

static klj_val create_big_web_client(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // One handle per client, as BigWebClient.GetId counts them: the three
    // panels a room makes must not share one.
    static int next = 1;
    int id = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    KLJ_LOG("BigWebView.CreateBigWebClient — web client %d", id);
    return (klj_val){.j = (uint64_t)id};
}
static klj_val setup_big_web_client(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("BigWebView.SetupClient(client %d, %s, %dx%d)",
            n > 0 ? (int)(int32_t)a[0].j : -1,
            n > 1 && a[1].l ? (klj_class_name(a[1].l) ? klj_class_name(a[1].l) : "object") : "null",
            n > 2 ? (int)(int32_t)a[2].j : 0, n > 3 ? (int)(int32_t)a[3].j : 0);
    if (n >= 4) bsweb_client_setup((int)(int32_t)a[0].j, a[1].l,
                                   (int)(int32_t)a[2].j, (int)(int32_t)a[3].j);
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
// Argument order from the dex: PointerMove(client, x, y, target),
// PointerDown/Up(client, x, y, button, ?, target), PointerDrag(client, x, y,
// button, target), Scroll(client, dx, dy, x, y, target), CancelInput(client,
// button, target). x/y are panel pixels; target picks the panel's web view
// or its fullscreen view (the latter does not exist here).
static klj_val webview_pointer_move(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bsweb_client_input((n > 0 ? (int)(int32_t)a[0].j : 0), BSW_MOVE, (n > 1 ? (int)(int32_t)a[1].j : 0), (n > 2 ? (int)(int32_t)a[2].j : 0), 0, 0, 0, (n > 3 ? (int)(int32_t)a[3].j : 0));
    return (klj_val){0};
}
static klj_val webview_pointer_down(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bsweb_client_input((n > 0 ? (int)(int32_t)a[0].j : 0), BSW_DOWN, (n > 1 ? (int)(int32_t)a[1].j : 0), (n > 2 ? (int)(int32_t)a[2].j : 0), (n > 3 ? (int)(int32_t)a[3].j : 0), (n > 4 ? (int)(int32_t)a[4].j : 0), 0, (n > 5 ? (int)(int32_t)a[5].j : 0));
    return (klj_val){0};
}
static klj_val webview_pointer_drag(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bsweb_client_input((n > 0 ? (int)(int32_t)a[0].j : 0), BSW_DRAG, (n > 1 ? (int)(int32_t)a[1].j : 0), (n > 2 ? (int)(int32_t)a[2].j : 0), (n > 3 ? (int)(int32_t)a[3].j : 0), 0, 0, (n > 4 ? (int)(int32_t)a[4].j : 0));
    return (klj_val){0};
}
static klj_val webview_pointer_up(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bsweb_client_input((n > 0 ? (int)(int32_t)a[0].j : 0), BSW_UP, (n > 1 ? (int)(int32_t)a[1].j : 0), (n > 2 ? (int)(int32_t)a[2].j : 0), (n > 3 ? (int)(int32_t)a[3].j : 0), (n > 4 ? (int)(int32_t)a[4].j : 0), 0, (n > 5 ? (int)(int32_t)a[5].j : 0));
    return (klj_val){0};
}
static klj_val webview_scroll(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bsweb_client_input((n > 0 ? (int)(int32_t)a[0].j : 0), BSW_SCROLL, (n > 3 ? (int)(int32_t)a[3].j : 0), (n > 4 ? (int)(int32_t)a[4].j : 0), 0, (n > 1 ? (int)(int32_t)a[1].j : 0), (n > 2 ? (int)(int32_t)a[2].j : 0), (n > 5 ? (int)(int32_t)a[5].j : 0));
    return (klj_val){0};
}
static klj_val webview_cancel_input(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    bsweb_client_input((n > 0 ? (int)(int32_t)a[0].j : 0), BSW_CANCEL, 0, 0, (n > 1 ? (int)(int32_t)a[1].j : 0), 0, 0, (n > 2 ? (int)(int32_t)a[2].j : 0));
    return (klj_val){0};
}
static klj_val webview_exit_fullscreen(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigWebView.ExitFullScreen — accepted (nothing here is ever fullscreen)");
    return (klj_val){0};
}

// ---- Audio Tracks menu (2026-11-27) ----
//
// BigMediaPlayer.GetAudioTracks answers a base64 TrackInfoList built from
// ExoPlayer's formats: ids 0..n-1 in stream order, the language as ExoPlayer
// normalises it (two letters where there are two), and for audio a label from
// DebugUtils.buildTrackName (language, channels, bitrate, codec). Here
// FFmpeg's stream list stands in for ExoPlayer's (kl_bsplayer_audio_tracks),
// so a DTS track AVFoundation hides is listed and playable too, and
// SelectAudioTrack(id) plays stream id from where the picture is.

static const struct { const char *b, *t, *two, *name; } bs_langs[] = {
    {"ger", "deu", "de", "German"},   {"eng", "eng", "en", "English"},
    {"fre", "fra", "fr", "French"},   {"spa", "spa", "es", "Spanish"},
    {"ita", "ita", "it", "Italian"},  {"jpn", "jpn", "ja", "Japanese"},
    {"por", "por", "pt", "Portuguese"}, {"rus", "rus", "ru", "Russian"},
    {"chi", "zho", "zh", "Chinese"},  {"kor", "kor", "ko", "Korean"},
    {"dut", "nld", "nl", "Dutch"},    {"pol", "pol", "pl", "Polish"},
    {"tur", "tur", "tr", "Turkish"},  {"swe", "swe", "sv", "Swedish"},
    {"dan", "dan", "da", "Danish"},   {"nor", "nor", "no", "Norwegian"},
    {"fin", "fin", "fi", "Finnish"},  {"cze", "ces", "cs", "Czech"},
    {"hun", "hun", "hu", "Hungarian"}, {"gre", "ell", "el", "Greek"},
    {"hin", "hin", "hi", "Hindi"},    {"ara", "ara", "ar", "Arabic"},
};

// "ger" -> "de", "German"; "und"/"" -> "", NULL; unknown codes pass through.
static const char *bs_lang(const char *code, const char **name) {
    *name = NULL;
    if (!code || !*code || !strcmp(code, "und")) return "";
    for (size_t i = 0; i < sizeof bs_langs / sizeof bs_langs[0]; i++)
        if (!strcmp(code, bs_langs[i].b) || !strcmp(code, bs_langs[i].t) ||
            !strcmp(code, bs_langs[i].two)) {
            *name = bs_langs[i].name;
            return bs_langs[i].two;
        }
    return code;
}

static klj_val media_get_audio_tracks(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    enum { MAXT = 32 };
    static char list[8192];
    int count = g_film ? kl_bsplayer_audio_tracks_peek(g_film, list, sizeof list) : -1;
    if (count < 0) { count = 0; list[0] = 0; }
    if (count > MAXT) count = MAXT;
    int ids[MAXT];
    const char *langs[MAXT], *labels[MAXT];
    static char labbuf[MAXT][200];
    char *line = list;
    int k = 0;
    for (; k < count && line && *line; k++) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *f[6] = {0};
        char *q = line;
        for (int i = 0; i < 6 && q; i++) {
            f[i] = q;
            q = strchr(q, '\t');
            if (q) *q++ = 0;
        }
        const char *lname = NULL;
        langs[k] = bs_lang(f[0], &lname);
        int ch = f[3] ? atoi(f[3]) : 0;
        const char *layout = ch == 1 ? "mono" : ch == 2 ? "stereo" : ch == 6 ? "5.1" : ch == 8 ? "7.1" : "";
        char chbuf[16] = "";
        if (!*layout && ch > 0) { snprintf(chbuf, sizeof chbuf, "%d ch", ch); layout = chbuf; }
        const char *codec = f[2] && *f[2] ? f[2] : "";
        char who[48];
        if (lname) snprintf(who, sizeof who, "%s", lname);
        else if (*langs[k]) snprintf(who, sizeof who, "%s", langs[k]);
        else snprintf(who, sizeof who, "Track %d", k + 1);
        if (f[1] && *f[1])
            snprintf(labbuf[k], sizeof labbuf[k], "%s", f[1]);   // the file's own title
        else
            snprintf(labbuf[k], sizeof labbuf[k], "%s%s%s%s%s", who,
                     *codec ? ", " : "", codec, *layout ? ", " : "", layout);
        labels[k] = labbuf[k];
        ids[k] = k;
        line = end ? end + 1 : NULL;
    }
    unsigned char *fb = NULL;
    size_t fbn = 0;
    char *b64 = NULL;
    if (kl_bstracks_encode(k, ids, langs, labels, &fb, &fbn) == 0)
        b64 = kl_bsinfo_base64(fb, fbn);
    free(fb);
    KLJ_LOG("BigMediaPlayerAPI.GetAudioTracks — %d track(s)%s%s", k,
            k ? ", first: " : "", k ? labels[0] : "");
    klj_val r = {.l = kl_jni_new_string(b64 ? b64 : "")};
    free(b64);
    return r;
}

static klj_val media_select_audio_track(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = n > 1 ? (int)(int32_t)a[1].j : -1;
    int rc = g_film ? kl_bsplayer_select_audio(g_film, id) : -1;
    KLJ_LOG("BigMediaPlayerAPI.SelectAudioTrack — track %d %s", id,
            rc == 0 ? "playing" : "refused");
    return (klj_val){0};
}

static klj_val media_deselect_all_audio_tracks(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG_ONCE("BigMediaPlayerAPI.DeselectAllAudioTracks — accepted (the sound keeps its track)");
    return (klj_val){0};
}

// ---- Subtitles menu (2026-11-27) ----
//
// GetTextTracks is the same TrackInfoList as the audio one, from ExoPlayer's
// text formats; the Java side labels a text track with Format.label alone
// (its title). Here: the title, else the language, and "(file)" for a
// subtitle file beside the film. SelectTextTrack(player, id, on) shows or
// hides one; the cues then go out through SubtitleCueCallback (the pump in
// update_media_players).
static klj_val media_get_text_tracks(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    enum { MAXT = 32 };
    static char list[8192];
    int count = g_film ? kl_bsplayer_text_tracks_peek(g_film, list, sizeof list) : -1;
    if (count < 0) { count = 0; list[0] = 0; }
    if (count > MAXT) count = MAXT;
    int ids[MAXT];
    const char *langs[MAXT], *labels[MAXT];
    static char labbuf[MAXT][200];
    char *line = list;
    int k = 0;
    for (; k < count && line && *line; k++) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *f[5] = {0};
        char *q = line;
        for (int i = 0; i < 5 && q; i++) {
            f[i] = q;
            q = strchr(q, '\t');
            if (q) *q++ = 0;
        }
        const char *lname = NULL;
        langs[k] = bs_lang(f[0], &lname);
        int flags = f[3] ? atoi(f[3]) : 0, file = f[4] ? atoi(f[4]) : 0;
        char who[160];
        if (f[1] && *f[1]) snprintf(who, sizeof who, "%s", f[1]);
        else if (lname) snprintf(who, sizeof who, "%s", lname);
        else if (*langs[k]) snprintf(who, sizeof who, "%s", langs[k]);
        else snprintf(who, sizeof who, "Subtitles %d", k + 1);
        snprintf(labbuf[k], sizeof labbuf[k], "%s%s%s", who,
                 (flags & 2) ? " (forced)" : "", file ? " (file)" : "");
        labels[k] = labbuf[k];
        ids[k] = k;
        line = end ? end + 1 : NULL;
    }
    unsigned char *fb = NULL;
    size_t fbn = 0;
    char *b64 = NULL;
    if (kl_bstracks_encode(k, ids, langs, labels, &fb, &fbn) == 0)
        b64 = kl_bsinfo_base64(fb, fbn);
    free(fb);
    KLJ_LOG("BigMediaPlayerAPI.GetTextTracks — %d track(s)%s%s", k,
            k ? ", first: " : "", k ? labels[0] : "");
    klj_val r = {.l = kl_jni_new_string(b64 ? b64 : "")};
    free(b64);
    return r;
}

static klj_val media_select_text_track(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = n > 1 ? (int)(int32_t)a[1].j : -1;
    int on = n > 2 ? (int)(a[2].j & 0xff) != 0 : 1;
    int rc = -1;
    if (g_film) {
        if (on) rc = kl_bsplayer_select_text(g_film, id);
        else if (id == g_text_sel) rc = kl_bsplayer_select_text(g_film, -1);
        else rc = 0;
        if (rc == 0) g_text_sel = on ? id : (id == g_text_sel ? -1 : g_text_sel);
    }
    KLJ_LOG("BigMediaPlayerAPI.SelectTextTrack — track %d %s%s", id, on ? "on" : "off",
            rc == 0 ? "" : " (refused)");
    return (klj_val){0};
}

static klj_val media_deselect_all_text_tracks(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (g_film) kl_bsplayer_select_text(g_film, -1);
    g_text_sel = -1;
    KLJ_LOG("BigMediaPlayerAPI.DeselectAllTextTracks — subtitles off");
    return (klj_val){0};
}

// ---- Browser: Vuplex 3D WebView on WKWebView (2026-11-27) ----
//
// Bigscreen's in-room browser (BigBrowser) is Vuplex 3D WebView on Android:
// its C# drives com.vuplex.webview.WebView3D through JNI and samples each
// view's picture from a GL texture its native plugin created. Here every
// WebView3D is a WKWebView (kl_web.m): the C# calls below land on it, its
// snapshots are uploaded into the view's texture from render(), and its page
// events go back to C# the way Vuplex's Java sends them —
// UnitySendMessage(<view id>, "Handle…", arg) — from the Update pump, which
// has the guest's JNIEnv. What the game asks for that has no answer yet is
// logged with its arguments (first 12 calls per method, then every 600th)
// and answered harmlessly, as the probe before this did.
//
// The probe runs (2026-11-27) showed what Bigscreen uses: its own browser UI
// (https://www.bigscreenvr.com/browser/next), its cloud browser (a page on
// *.bigscreencloud.com that streams a remote browser, after setCookie with
// the account's access token), YouTube directly, and the APK's own pages
// (jar:file:…apk!/assets/web/youtube_busy.html). KL_BS_WEB=0 puts back the
// old answer: no web view available.
int klj_bigscreen_web_probe(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = t && !strcmp(t, "x2497738113633933") && kl_env_on("KL_BS_WEB", 1) &&
             kl_env_on("KL_BS_WEB_PROBE", 1);
    }
    return on;
}

// One argument as text, by its type in the signature.
static const char *probe_arg(const char **sigp, klj_val v, char *buf, size_t cap) {
    const char *t = *sigp;
    switch (*t) {
    case 'Z': snprintf(buf, cap, "%s", (v.j & 0xff) ? "true" : "false"); (*sigp)++; break;
    case 'B': case 'C': case 'S': case 'I':
        snprintf(buf, cap, "%d", (int)(int32_t)v.j); (*sigp)++; break;
    case 'J': snprintf(buf, cap, "%lld", (long long)v.j); (*sigp)++; break;
    case 'F': case 'D': snprintf(buf, cap, "%g", v.d); (*sigp)++; break;
    case '[': {
        const char *e = t;
        while (*e == '[') e++;
        if (*e == 'L') { while (*e && *e != ';') e++; }
        if (*e) e++;
        snprintf(buf, cap, "%s[%.*s]", v.l ? "" : "null", (int)(e - t), t);
        *sigp = e;
        break;
    }
    case 'L': {
        const char *e = strchr(t, ';');
        size_t len = e ? (size_t)(e - t + 1) : strlen(t);
        if (!v.l) snprintf(buf, cap, "null");
        else if (len == 18 && !strncmp(t, "Ljava/lang/String;", 18)) {
            const char *str = klj_str(v.l);
            // The cookie carries the account's access token: its value
            // never reaches the log.
            if (str && strstr(str, "\"Value\""))
                snprintf(buf, cap, "\"%.60s…\" (%zu chars)", str, strlen(str));
            else
                snprintf(buf, cap, "\"%.300s\"", str ? str : "");
        } else {
            const char *cn = klj_class_name(v.l);
            snprintf(buf, cap, "<%s>", cn ? cn : "object");
        }
        *sigp = t + len;
        break;
    }
    default: snprintf(buf, cap, "?"); (*sigp)++; break;
    }
    return buf;
}

static void probe_log(void *self, const char *cls, const char *name, const char *sig,
                      const klj_val *a, int n, const char *note) {
    enum { MAXM = 256 };
    static char *seen[MAXM];
    static unsigned long count[MAXM];
    static int nseen;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    char key[256];
    snprintf(key, sizeof key, "%s.%s%s", cls, name, sig);
    pthread_mutex_lock(&mu);
    int i = 0;
    for (; i < nseen; i++) if (!strcmp(seen[i], key)) break;
    if (i == nseen && nseen < MAXM) seen[nseen++] = strdup(key);
    unsigned long c = i < MAXM ? ++count[i] : 0;
    pthread_mutex_unlock(&mu);
    if (c > 12 && c % 600 != 0) return;
    char line[2048];
    size_t len = 0;
    const char *p = strchr(sig, '(');
    p = p ? p + 1 : sig;
    for (int k = 0; k < n && *p && *p != ')'; k++) {
        char arg[400];
        probe_arg(&p, a[k], arg, sizeof arg);
        int w = snprintf(line + len, sizeof line - len, "%s%s", k ? ", " : "", arg);
        if (w < 0 || (size_t)w >= sizeof line - len) break;
        len += (size_t)w;
    }
    (void)self;
    KLJ_LOG("[web] #%lu %s.%s%s (%s)%s%s", c, strrchr(cls, '/') ? strrchr(cls, '/') + 1 : cls,
            name, sig, len ? line : "", note ? " — " : "", note ? note : "");
}

// The Vuplex page bridge: a script Vuplex's Java injects after every page
// load (window.vuplex, focus/keyboard managers, title and URL messages).
// It is Vuplex's code and ships inside the game's classes.dex as one string
// constant, so it is read from there rather than carried here.
static char *g_vx_bundle;
static int g_vx_bundle_tried;

static const char *vx_bundle(void) {
    if (g_vx_bundle_tried) return g_vx_bundle;
    g_vx_bundle_tried = 1;
    static const char head[] = "if(typeof Object.assign!=\"function\"";
    for (int k = 1; k <= 9 && !g_vx_bundle; k++) {
        char name[32];
        if (k == 1) snprintf(name, sizeof name, "classes.dex");
        else snprintf(name, sizeof name, "classes%d.dex", k);
        size_t len = 0;
        uint8_t *dex = klj_apk_entry(name, &len);
        if (!dex) { if (k > 1) break; continue; }
        for (size_t i = 0; i + sizeof head < len; i++) {
            if (dex[i] != 'i' || memcmp(dex + i, head, sizeof head - 1)) continue;
            size_t e = i;
            while (e < len && dex[e]) e++;
            if (e - i > 1000 && memmem(dex + i, e - i, "_vuplexSystem", 13)) {
                g_vx_bundle = malloc(e - i + 1);
                if (g_vx_bundle) { memcpy(g_vx_bundle, dex + i, e - i); g_vx_bundle[e - i] = 0; }
                KLJ_LOG("[web] Vuplex page bridge read from %s (%zu bytes)", name, e - i);
                break;
            }
        }
        free(dex);
    }
    if (!g_vx_bundle) KLJ_LOG("[web] Vuplex page bridge not found in the APK — pages get no window.vuplex");
    return g_vx_bundle;
}

static void *vx_asset(const char *name, size_t *len) { return klj_apk_entry(name, len); }

typedef struct bs_vx {
    struct bs_vx *next;
    char id[96];
    kl_web *web;
    int tex, w, h;
    int focus_events;            // setFocusedInputFieldEventsEnabled
    int init_sent;
    int px, py;                  // last pointer, for scroll(II)
    int client;                  // BigWebView client tied to this view (SetupClient)
    int down;                    // a button is held (from BigWebView input)
    int media;                   // the page plays video/audio (kl_audio_web_window held)
    char last_url[1024];
    // render thread
    uint64_t seq;
    uint8_t *buf;
    size_t cap;
    int tw, th;                  // size the texture was allocated at
    unsigned long uploads, renders, no_ctx, no_frame;
} bs_vx;

static bs_vx *g_vx;
// Recursive: a callback into the guest from the pump may come straight back
// with a WebView3D call.
static pthread_mutex_t g_vx_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static bs_vx *vx_of(void *self) {
    klj_object *o = self ? klj_as_object(self) : NULL;
    bs_vx *v = o ? o->data : NULL;
    if (!v) return NULL;
    pthread_mutex_lock(&g_vx_mu);
    bs_vx *it = g_vx;
    while (it && it != v) it = it->next;
    pthread_mutex_unlock(&g_vx_mu);
    return it;
}

static bs_vx *vx_valid(bs_vx *p) {
    if (!p) return NULL;
    pthread_mutex_lock(&g_vx_mu);
    bs_vx *it = g_vx;
    while (it && it != p) it = it->next;
    pthread_mutex_unlock(&g_vx_mu);
    return it;
}

static void bsweb_client_setup(int client, void *native_view, int w, int h) {
    klj_object *o = native_view ? klj_as_object(native_view) : NULL;
    bs_vx *v = vx_valid(o ? o->data : NULL);
    if (!v) {
        KLJ_LOG("[web] SetupClient(client %d): the native view is not one of ours — input for it is dropped", client);
        return;
    }
    pthread_mutex_lock(&g_vx_mu);
    for (bs_vx *it = g_vx; it; it = it->next) if (it->client == client) it->client = 0;
    v->client = client;
    pthread_mutex_unlock(&g_vx_mu);
    KLJ_LOG("[web] client %d -> %s (%dx%d panel, view %dx%d)", client, v->id, w, h, v->w, v->h);
}

static void bsweb_client_input(int client, int kind, int x, int y, int button,
                               int dx, int dy, int target) {
    pthread_mutex_lock(&g_vx_mu);
    bs_vx *v = g_vx;
    while (v && v->client != client) v = v->next;
    kl_web *web = v ? v->web : NULL;
    if (v && kind != BSW_SCROLL && kind != BSW_CANCEL) { v->px = x; v->py = y; }
    int was_down = v ? v->down : 0;
    if (v && kind == BSW_DOWN) v->down = 1;
    if (v && (kind == BSW_UP || kind == BSW_CANCEL)) v->down = 0;
    pthread_mutex_unlock(&g_vx_mu);
    static unsigned long logged[6];
    if (kind >= 0 && kind < 6 && (++logged[kind] <= 6 || logged[kind] % 500 == 0))
        KLJ_LOG("[web] input %s client %d (%s): x %d y %d button %d d %d,%d target %d #%lu",
                (const char *[]){"move", "down", "drag", "up", "scroll", "cancel"}[kind], client,
                v ? v->id : "no view", x, y, button, dx, dy, target, logged[kind]);
    if (!web) return;
    // Android's buttons as MotionEvent states: 1 primary, 2 secondary,
    // 4 tertiary; anything else is the primary.
    int b = button == 2 ? 2 : button == 4 ? 1 : 0;
    switch (kind) {
    case BSW_MOVE: case BSW_DRAG:
        kl_web_pointer(web, KL_WEB_PTR_MOVE, x, y, b);
        break;
    case BSW_DOWN:
        kl_web_pointer(web, KL_WEB_PTR_DOWN, x, y, b);
        break;
    case BSW_UP:
        kl_web_pointer(web, KL_WEB_PTR_UP, x, y, b);
        break;
    case BSW_CANCEL:
        if (was_down) kl_web_pointer(web, KL_WEB_PTR_UP, v->px, v->py, b);
        break;
    case BSW_SCROLL: {
        // Android scroll axes: notches, positive is up/left. Small values
        // are notches, larger ones taken as pixels already.
        int sx = (dx > -16 && dx < 16) ? -dx * 60 : -dx;
        int sy = (dy > -16 && dy < 16) ? -dy * 60 : -dy;
        kl_web_scroll(web, sx, sy, x, y);
        break;
    }
    }
}

static void vx_page_script(bs_vx *v) {
    const char *b = vx_bundle();
    if (!b || !v->web) return;
    size_t n = strlen(b) + 512;
    char *js = malloc(n);
    if (!js) return;
    snprintf(js, n, "%s;window.__initVuplex({ defaultBackgroundEnabled: true, "
             "overrideViewportMetaTag: false, enableFocusedInputFieldEvents: %s, "
             "enableInputTypeWorkaround: false, enableSelectElementManager: false, "
             "removeAnchorTargetAttributes: false });",
             b, v->focus_events ? "true" : "false");
    kl_web_set_page_script(v->web, js);
    free(js);
}

// JSON string body (no quotes) of s into out.
static void json_esc(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; s && *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void vx_send(void *env, bs_vx *v, const char *method, const char *arg) {
    bs_cb slot = { v->id, (char *)method };
    bs_send(env, &slot, arg ? arg : "", method);
}

static void vx_url_changed(void *env, bs_vx *v, const char *url) {
    if (!url || !strcmp(url, v->last_url)) return;
    snprintf(v->last_url, sizeof v->last_url, "%s", url);
    char esc[2400], msg[2600];
    json_esc(url, esc, sizeof esc);
    snprintf(msg, sizeof msg, "{\"urlAction\":{\"Type\":\"LOAD\",\"Url\":\"%s\"},"
             "\"type\":\"vuplex.webview.urlChanged\"}", esc);
    vx_send(env, v, "HandleMessageEmitted", msg);
}

static void bs_vuplex_pump(void *env) {
    pthread_mutex_lock(&g_vx_mu);
    for (bs_vx *v = g_vx; v; v = v->next) {
        if (!v->init_sent) {
            v->init_sent = 1;
            KLJ_LOG("[web] %s ready -> HandleInitFinished", v->id);
            vx_send(env, v, "HandleInitFinished", "");
        }
        if (!v->web) continue;
        kl_web_ev k; char *arg; int id;
        int budget = 64;
        while (budget-- > 0 && kl_web_next_event(v->web, &k, &arg, &id)) {
            switch (k) {
            case KL_WEB_EV_LOAD_STARTED:
                vx_url_changed(env, v, arg);
                vx_send(env, v, "HandleLoadStarted", "");
                break;
            case KL_WEB_EV_LOAD_PROGRESS:
                vx_send(env, v, "HandleLoadProgressUpdate", arg);
                break;
            case KL_WEB_EV_LOAD_FINISHED:
                vx_url_changed(env, v, arg);
                vx_send(env, v, "HandleLoadFinished", "");
                break;
            case KL_WEB_EV_LOAD_FAILED:
                vx_send(env, v, "HandleLoadFailed", arg);
                break;
            case KL_WEB_EV_URL_CHANGED:
                vx_url_changed(env, v, arg);
                break;
            case KL_WEB_EV_MESSAGE:
                vx_send(env, v, "HandleMessageEmitted", arg);
                break;
            case KL_WEB_EV_MEDIA: {
                // A playing video takes the audio session; while it does, the
                // game's sound waits for the session's own end instead of
                // taking it back after 1.5 s, which stopped the video after a
                // second (2026-11-27) — the rule a web window already has.
                int on = arg && arg[0] == '1';
                if (on != v->media) { v->media = on; kl_audio_web_window(on); }
                break;
            }
            case KL_WEB_EV_TITLE_CHANGED:
            case KL_WEB_EV_JS_RESULT:
                break;    // the page bridge reports the title itself
            }
            free(arg);
        }
    }
    pthread_mutex_unlock(&g_vx_mu);
}

// render(): Vuplex's native plugin calls it on Unity's render thread every
// frame for every view; the newest snapshot goes into the view's texture.
// External (OES) textures are 2D ones on this host (klfb_detarget), so a
// plain TexImage/TexSubImage fills it; the guest's bindings are put back.
static void vx_upload(bs_vx *v) {
    if (v->tex <= 0) return;
    int w = 0, h = 0, got = 0;
    // Where render() goes, every 900 calls per view: an upload, no GL
    // context on the calling thread, or no newer snapshot.
    if (++v->renders % 900 == 0)
        KLJ_LOG("[web] %s: render #%lu — uploads %lu, no GL context %lu, no new frame %lu (luma %d)",
                v->id, v->renders, v->uploads, v->no_ctx, v->no_frame, kl_web_frame_luma(v->web));
    // A frame taken here without a context to upload it in would be lost:
    // the next one only comes with the next snapshot.
    if (!kl_egl_current_context()) { v->no_ctx++; return; }
    if (!v->web) {
        // KL_BS_WEB_FAKE=1: a test card instead of a page, once per view —
        // for a run with no WebKit (the Mac host) to see where the browser's
        // texture lands in the room.
        if (!kl_env_on("KL_BS_WEB_FAKE", 0) || v->seq) return;
        w = v->w; h = v->h;
        v->cap = (size_t)w * h * 4;
        v->buf = malloc(v->cap);
        if (!v->buf) { v->cap = 0; return; }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *p = v->buf + ((size_t)y * w + x) * 4;
                p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h);
                p[2] = ((x / 64 + y / 64) & 1) ? 255 : 40; p[3] = 255;
            }
        v->seq = 1;
        got = 1;
    } else {
        // Also tells kl_web the view is being drawn, which keeps its snapshots on.
        got = kl_web_frame(v->web, v->buf, v->cap, &w, &h, &v->seq);
        if (!got && w > 0 && h > 0 && (size_t)w * h * 4 > v->cap) {
            free(v->buf);
            v->cap = (size_t)w * h * 4;
            v->buf = malloc(v->cap);
            if (!v->buf) { v->cap = 0; return; }
            got = kl_web_frame(v->web, v->buf, v->cap, &w, &h, &v->seq);
        }
    }
    if (!got) { v->no_frame++; return; }
    static void (*gl_BindTexture)(uint32_t, uint32_t);
    static void (*gl_TexImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                 int32_t, uint32_t, uint32_t, const void *);
    static void (*gl_TexSubImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                    int32_t, uint32_t, uint32_t, const void *);
    static void (*gl_GetIntegerv)(uint32_t, int32_t *);
    static void (*gl_PixelStorei)(uint32_t, int32_t);
    static void (*gl_BindBuffer)(uint32_t, uint32_t);
    static void (*gl_TexParameteri)(uint32_t, uint32_t, int32_t);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        gl_BindTexture   = kl_egl_sym("glBindTexture");
        gl_TexImage2D    = kl_egl_sym("glTexImage2D");
        gl_TexSubImage2D = kl_egl_sym("glTexSubImage2D");
        gl_GetIntegerv   = kl_egl_sym("glGetIntegerv");
        gl_PixelStorei   = kl_egl_sym("glPixelStorei");
        gl_BindBuffer    = kl_egl_sym("glBindBuffer");
        gl_TexParameteri = kl_egl_sym("glTexParameteri");
    }
    if (!gl_BindTexture || !gl_TexImage2D || !gl_TexSubImage2D || !gl_GetIntegerv) return;
    if (kl_env_on("KL_BS_WEB_FLIP", 0)) {
        size_t row = (size_t)w * 4;
        uint8_t tmp[16384];
        for (int y = 0; y < h / 2 && row <= sizeof tmp; y++) {
            uint8_t *a = v->buf + (size_t)y * row, *b = v->buf + (size_t)(h - 1 - y) * row;
            memcpy(tmp, a, row); memcpy(a, b, row); memcpy(b, tmp, row);
        }
    }
    int32_t prev_tex = 0, prev_pbo = 0, prev_row = 0, prev_align = 4;
    gl_GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &prev_tex);
    gl_GetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &prev_pbo);
    gl_GetIntegerv(0x0CF2 /* UNPACK_ROW_LENGTH */, &prev_row);
    gl_GetIntegerv(0x0CF5 /* UNPACK_ALIGNMENT */, &prev_align);
    if (prev_pbo && gl_BindBuffer) gl_BindBuffer(0x88EC /* PIXEL_UNPACK_BUFFER */, 0);
    if (gl_PixelStorei) { gl_PixelStorei(0x0CF2, 0); gl_PixelStorei(0x0CF5, 4); }
    gl_BindTexture(0x0DE1 /* TEXTURE_2D */, (uint32_t)v->tex);
    if (v->tw != w || v->th != h) {
        gl_TexImage2D(0x0DE1, 0, 0x8058 /* RGBA8 */, w, h, 0, 0x1908 /* RGBA */,
                      0x1401 /* UNSIGNED_BYTE */, v->buf);
        if (gl_TexParameteri) {
            gl_TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
            gl_TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2601);
            gl_TexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
            gl_TexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
        }
        KLJ_LOG("[web] %s: texture %d sized %dx%d (luma %d)%s", v->id, v->tex, w, h,
                kl_web_frame_luma(v->web), v->web ? "" : " — test card");
        v->tw = w;
        v->th = h;
    } else {
        gl_TexSubImage2D(0x0DE1, 0, 0, 0, w, h, 0x1908, 0x1401, v->buf);
    }
    gl_BindTexture(0x0DE1, (uint32_t)prev_tex);
    if (gl_PixelStorei) { gl_PixelStorei(0x0CF2, prev_row); gl_PixelStorei(0x0CF5, prev_align); }
    if (prev_pbo && gl_BindBuffer) gl_BindBuffer(0x88EC, (uint32_t)prev_pbo);
    if (++v->uploads % 600 == 1)
        KLJ_LOG("[web] %s: upload #%lu into texture %d (luma %d)", v->id, v->uploads, v->tex,
                kl_web_frame_luma(v->web));
}

static int arg_i(const klj_val *a, int n, int k) { return k < n ? (int)(int32_t)a[k].j : 0; }
static int arg_z(const klj_val *a, int n, int k) { return k < n ? (int)(a[k].j & 0xff) : 0; }
static const char *arg_s(const klj_val *a, int n, int k) {
    return k < n && a[k].l ? klj_str(a[k].l) : NULL;
}

klj_val klj_bigscreen_probe_call(void *self, const char *cls, const char *name,
                                 const char *sig, const klj_val *a, int n) {
    int is3d = !strcmp(cls, "com/vuplex/webview/WebView3D") || !strcmp(cls, "com/vuplex/webview/WebView");
    // new WebView3D(String id, int texture, int w, int h, Z, Z, Z, BiConsumer, Object)
    if (is3d && !strcmp(name, "<init>") && n >= 4 && a[0].l) {
        bs_vx *v = calloc(1, sizeof *v);
        if (v) {
            const char *id = klj_str(a[0].l);
            snprintf(v->id, sizeof v->id, "%s", id ? id : "");
            v->tex = arg_i(a, n, 1);
            v->w = arg_i(a, n, 2);
            v->h = arg_i(a, n, 3);
            kl_web_set_asset_reader(vx_asset);
            char tag[40];
            snprintf(tag, sizeof tag, "vx-%.8s", v->id[0] == 'W' && strlen(v->id) > 8 ? v->id + 8 : v->id);
            v->web = kl_web_create(v->w, v->h, tag);
            if (v->tex > 0) kl_glfb_watch_texture((uint32_t)v->tex, 1);
            if (v->tex > 0) kl_bigscreen_web_texture((uint32_t)v->tex, 1);
            vx_page_script(v);
            pthread_mutex_lock(&g_vx_mu);
            v->next = g_vx;
            g_vx = v;
            pthread_mutex_unlock(&g_vx_mu);
            probe_log(self, cls, name, sig, a, n, v->web ? "web view made" : "NO web view");
        }
        return (klj_val){.l = klj_new_object_data(cls, v)};
    }
    if (is3d && !strcmp(name, "setCookie")) {
        const char *js = arg_s(a, n, 0);
        probe_log(self, cls, name, sig, a, n, "into the web views' cookie store");
        if (js) kl_web_set_cookie_json(js);
        return (klj_val){.j = 1};
    }
    if (is3d && !strcmp(name, "render")) {
        // Render thread, against destroy on the game thread: under the lock.
        pthread_mutex_lock(&g_vx_mu);
        bs_vx *rv = vx_of(self);
        if (rv) vx_upload(rv);
        pthread_mutex_unlock(&g_vx_mu);
        if (!rv) probe_log(self, cls, name, sig, a, n, "no view for this object");
        return (klj_val){0};
    }
    bs_vx *v = is3d ? vx_of(self) : NULL;
    if (v) {
        const char *note = NULL;
        int done = 1;
        if (0) {
        } else if (!strcmp(name, "loadUrl")) {
            kl_web_load_url(v->web, arg_s(a, n, 0));
        } else if (!strcmp(name, "loadHtml")) {
            kl_web_load_html(v->web, arg_s(a, n, 0), arg_s(a, n, 1));
        } else if (!strcmp(name, "setUserAgent")) {
            // (Z): mobile or not; the system's own string serves both here.
            kl_web_set_user_agent(v->web, sig[1] == 'Z' ? NULL : arg_s(a, n, 0));
        } else if (!strcmp(name, "resume")) {
            kl_web_set_active(v->web, 1);
        } else if (!strcmp(name, "pause")) {
            kl_web_set_active(v->web, 0);
        } else if (!strcmp(name, "setFocused")) {
            int on = arg_z(a, n, 0);
            kl_web_set_focused(v->web, on);
            kl_web_eval(v->web, on ? "window._vuplexInternal&&_vuplexInternal.focusManager.setFocused(true);"
                                   : "window._vuplexInternal&&_vuplexInternal.focusManager.setFocused(false);", 0);
        } else if (!strcmp(name, "setFocusedInputFieldEventsEnabled")) {
            v->focus_events = arg_z(a, n, 0);
            vx_page_script(v);
        } else if (!strcmp(name, "click")) {
            v->px = arg_i(a, n, 0); v->py = arg_i(a, n, 1);
            kl_web_pointer(v->web, KL_WEB_PTR_MOVE, v->px, v->py, 0);
            kl_web_pointer(v->web, KL_WEB_PTR_DOWN, v->px, v->py, 0);
            kl_web_pointer(v->web, KL_WEB_PTR_UP, v->px, v->py, 0);
        } else if (!strcmp(name, "movePointer")) {
            v->px = arg_i(a, n, 0); v->py = arg_i(a, n, 1);
            kl_web_pointer(v->web, KL_WEB_PTR_MOVE, v->px, v->py, 0);
            return (klj_val){0};
        } else if (!strcmp(name, "pointerDown")) {
            v->px = arg_i(a, n, 0); v->py = arg_i(a, n, 1);
            kl_web_pointer(v->web, KL_WEB_PTR_DOWN, v->px, v->py, arg_i(a, n, 2));
        } else if (!strcmp(name, "pointerUp")) {
            v->px = arg_i(a, n, 0); v->py = arg_i(a, n, 1);
            kl_web_pointer(v->web, KL_WEB_PTR_UP, v->px, v->py, arg_i(a, n, 2));
        } else if (!strcmp(name, "scroll")) {
            if (n >= 4) kl_web_scroll(v->web, arg_i(a, n, 0), arg_i(a, n, 1), arg_i(a, n, 2), arg_i(a, n, 3));
            else kl_web_scroll(v->web, arg_i(a, n, 0), arg_i(a, n, 1), v->px, v->py);
        } else if (!strcmp(name, "sendKey")) {
            const char *k = arg_s(a, n, 0);
            if (k) kl_web_key(v->web, k);
        } else if (!strcmp(name, "goBack")) {
            kl_web_go_back(v->web);
        } else if (!strcmp(name, "goForward")) {
            kl_web_go_forward(v->web);
        } else if (!strcmp(name, "reload")) {
            kl_web_reload(v->web);
        } else if (!strcmp(name, "stopLoad") || !strcmp(name, "stopLoading")) {
            kl_web_stop(v->web);
        } else if (!strcmp(name, "resize")) {
            v->w = arg_i(a, n, 0); v->h = arg_i(a, n, 1);
            kl_web_resize(v->web, v->w, v->h);
        } else if (!strcmp(name, "executeJavaScript")) {
            kl_web_eval(v->web, arg_s(a, n, 0), 0);
            note = "run; its result is not handed back yet";
        } else if (!strcmp(name, "destroy")) {
            pthread_mutex_lock(&g_vx_mu);
            for (bs_vx **pp = &g_vx; *pp; pp = &(*pp)->next)
                if (*pp == v) { *pp = v->next; break; }
            pthread_mutex_unlock(&g_vx_mu);
            klj_object *o = klj_as_object(self);
            if (o) o->data = NULL;
            if (v->media) { v->media = 0; kl_audio_web_window(0); }
            if (v->tex > 0) kl_glfb_watch_texture((uint32_t)v->tex, 0);
            if (v->tex > 0) kl_bigscreen_web_texture((uint32_t)v->tex, 0);
            kl_web_destroy(v->web);
            probe_log(self, cls, name, sig, a, n, "web view gone");
            free(v->buf);
            free(v);
            return (klj_val){0};
        } else {
            done = 0;
        }
        if (done) {
            probe_log(self, cls, name, sig, a, n, note);
            return (klj_val){0};
        }
    }
    probe_log(self, cls, name, sig, a, n, "not handled");
    // The answer: harmless and of the right kind.
    if (!strcmp(name, "<init>")) return (klj_val){.l = kl_jni_new_object(cls)};
    // BigBrowser hands this to BigWebView.SetupClient: an android.webkit.WebView
    // (declared Object), so the web wrapper's own family sees its class.
    // ...carrying the view, so SetupClient can tie its client to it.
    if (!strcmp(name, "getNativeWebView"))
        return (klj_val){.l = v ? klj_new_object_data("android/webkit/WebView", v)
                                : kl_jni_new_object("android/webkit/WebView")};
    const char *r = strchr(sig, ')');
    r = r ? r + 1 : "V";
    if (!strcmp(r, "Ljava/lang/String;")) {
        if (v && !strcmp(name, "getUrl")) return (klj_val){.l = kl_jni_new_string(v->last_url)};
        return (klj_val){.l = kl_jni_new_string("")};
    }
    if (*r == 'L') {
        char rcls[256];
        snprintf(rcls, sizeof rcls, "%.*s", (int)(strlen(r) - 2), r + 1);
        return (klj_val){.l = kl_jni_new_object(rcls)};
    }
    return (klj_val){0};
}

// UnityPlayer.UnitySendMessage(gameObject, method, message): Unity's own Java
// helper, which hands the three to the engine's nativeUnitySendMessage. Vuplex's
// native plugin reports its new texture this way right after the
// create-texture render event (probe 2, 2026-11-27); answered with nothing,
// the browser's Init waited forever. Bigscreen only.
static klj_val bs_unity_send_message(void *env, void *self, const klj_val *a, int n) {
    (void)self;
    const char *go = n > 0 && a[0].l ? klj_str(a[0].l) : "";
    const char *fn = n > 1 && a[1].l ? klj_str(a[1].l) : "";
    const char *msg = n > 2 && a[2].l ? klj_str(a[2].l) : "";
    static unsigned long count;
    if (++count <= 40 || count % 500 == 0)
        KLJ_LOG("UnityPlayer.UnitySendMessage(\"%s\", \"%s\", \"%.300s\") #%lu", go ? go : "",
                fn ? fn : "", msg ? msg : "", count);
    bs_cb slot = { (char *)go, (char *)fn };
    if (go && *go) bs_send(env, &slot, msg, "UnitySendMessage");
    return (klj_val){0};
}

static klj_val probe_is_webview_available(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int yes = klj_bigscreen_web_probe();
    KLJ_LOG("[web] com/vuplex/webview/WebView3D.isWebViewAvailable() -> %s%s",
            yes ? "true" : "false", yes ? " (web views are WKWebViews on this host)" : "");
    return (klj_val){.j = (uint64_t)yes};
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
    // The Audio Tracks menu: the list (a base64 TrackInfoList), a pick, and
    // the "none" the menu can ask for.
    {BIGSCREEN_API, "GetAudioTracks", "(I)Ljava/lang/String;", media_get_audio_tracks},
    {BIGSCREEN_API, "SelectAudioTrack", "(II)V", media_select_audio_track},
    {BIGSCREEN_API, "DeselectAllAudioTracks", "(I)V", media_deselect_all_audio_tracks},
    // The Subtitles menu: the list, a pick (on/off), and "none".
    {BIGSCREEN_API, "GetTextTracks", "(I)Ljava/lang/String;", media_get_text_tracks},
    {BIGSCREEN_API, "SelectTextTrack", "(IIZ)V", media_select_text_track},
    {BIGSCREEN_API, "DeselectAllTextTracks", "(I)V", media_deselect_all_text_tracks},
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
    // ...and as BigBrowser calls it, with Vuplex's getNativeWebView() typed as
    // the Object that method declares (browser probe, Mac run, 2026-11-27).
    {BIGSCREEN_WEBVIEW, "SetupClient", "(ILjava/lang/Object;II)V", setup_big_web_client},
    // Unity's reflection spells the class dotted, as with SetContext.
    {BIGSCREEN_WEBVIEW, "SetupClient", "(ILandroid.webkit.WebView;II)V", setup_big_web_client},
    {BIGSCREEN_WEBVIEW, "SetupClient", "(ILjava.lang.Object;II)V", setup_big_web_client},
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
    // The browser probe's answer to Vuplex's availability question.
    {"com/vuplex/webview/WebView3D", "isWebViewAvailable", "()Z", probe_is_webview_available},
    // Unity's Java helper, which Vuplex's native plugin calls into.
    {"com/unity3d/player/UnityPlayer", "UnitySendMessage",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", bs_unity_send_message},
    {NULL, NULL, NULL, NULL}
};

const klj_binding *klj_bigscreen_binding(const char *cls, const char *name, const char *sig) {
    const char *target = kl_driver_target_name();
    if (!target || strcmp(target, "x2497738113633933")) return NULL;
    for (const klj_binding *b = bindings; b->cls; b++)
        if (!strcmp(cls, b->cls) && !strcmp(name, b->name) && !strcmp(sig, b->sig)) return b;
    return NULL;
}
