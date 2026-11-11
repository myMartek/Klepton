// kl_avpro.m — the Java half of AVPro Video's Android player, answered on the
// host with AVFoundation, for DeoVR Classic (x2382576078453818) only.
//
// AVPro Video 2/3 splits its Android player in two: libAVProVideo2Native.so
// (the GL side, which renders what a Surface receives into Unity's texture)
// and the Java class com/renderheads/AVPro/Video/Manager (ExoPlayer or
// MediaPlayer underneath, which decodes into that Surface). There is no Java
// here, so the Manager's player methods had no answer and DeoVR showed no
// video. This file answers them: an AVPlayer per player id, frames taken from
// an AVPlayerItemVideoOutput only when there is a new one, and published into
// the AImageReader behind the Surface the plugin's own CreateSurface made
// (kl_mediandk_reader_publish), which is where the plugin's GL side reads.
//
// Seeks are asynchronous, as AVPro expects: the state says IsSeeking, and
// reports the seek target as the current time, until AVFoundation's completion
// for the LATEST seek arrives (a seek generation tells it from the earlier,
// cancelled ones); the reader's queue is dropped at each seek so no frame from
// before it is shown after it.
//
// Sent as community fix 100 and confirmed by its sender on an Apple Vision Pro
// (playback with sound, scrubbing with previews, pause, resume, next). Only
// DeoVR's lookup ever sees the table (klj_avpro_table): VRChat and other games
// ship the same plugin, with other builds, and keep the old path. The native
// half's wrappers are in runtime/kl_dl.c (kl_avpro_interpose). KL_AVPRO_HOST=0
// turns all of it off for DeoVR as well.
//
// Compiled with ARC on the host and without it for visionOS (the xros archive
// rule passes no -fobjc-arc), so object references are kept as CF-retained
// void * (CFBridgingRetain / CFRelease), which means the same in both.

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_jni_int.h"
#include "kl_ndk.h"
#include "kl_mediandk.h"
#include "kl_driver.h"

#if __has_feature(objc_arc)
#define KLA_OWNED(x) (x)
#else
#define KLA_OWNED(x) [(x) autorelease]
#endif

extern void *klb_dlsym(void *handle, const char *name);

#define KLA_PLAYERS 16

typedef struct {
    int       in_use, lock_ready;
    int       id;
    void     *player;           // AVPlayer *, CF-retained
    void     *output;           // AVPlayerItemVideoOutput *, CF-retained
    void     *reader;           // the klm_reader behind the plugin's Surface
    int       width, height;
    float     duration, rate, volume;
    int       ready, playing, seeking;
    double    seek_target;
    int       seek_done_pulse, asset_info_pulse, force_frame;
    int64_t   last_pts_us;
    uint32_t  seek_gen, frames;
    pthread_mutex_t lock;       // recursive: an AVFoundation completion may run inline
} kla_player;

static kla_player g_players[KLA_PLAYERS];
static pthread_mutex_t g_players_mu = PTHREAD_MUTEX_INITIALIZER;

static kla_player *kla_get(int id) {
    if (id <= 0 || id >= KLA_PLAYERS) return NULL;
    kla_player *p = &g_players[id];
    return p->in_use ? p : NULL;
}
static int kla_id(const klj_val *a, int n) { return n > 0 ? (int)a[0].j : 0; }

// The first video track, waited for (at most 1.5 s). `tracksWithMediaType:` is
// unavailable on visionOS; see kl_avdec.m's first_track_of. Returned retained.
static void *kla_first_video_track(AVAsset *asset) {
    __block void *track = NULL;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack *> *tracks, NSError *err) {
        (void)err;
        if (tracks.count) track = (void *)CFBridgingRetain(tracks[0]);
        dispatch_semaphore_signal(sem);
    }];
    long late = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC));
    return late ? NULL : track;
}

// ---- MakePlayer / DestroyPlayer ----
static klj_val kla_MakePlayer(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    pthread_mutex_lock(&g_players_mu);
    int id = -1;
    for (int i = 1; i < KLA_PLAYERS; i++)
        if (!g_players[i].in_use) { id = i; break; }
    if (id < 0) {
        pthread_mutex_unlock(&g_players_mu);
        KLJ_LOG("AVPro: MakePlayer — all %d players in use", KLA_PLAYERS - 1);
        return (klj_val){.j = 0};
    }
    kla_player *p = &g_players[id];
    if (!p->lock_ready) {
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&p->lock, &at);
        pthread_mutexattr_destroy(&at);
        p->lock_ready = 1;
    }
    pthread_mutex_lock(&p->lock);
    p->id = id;
    p->player = p->output = p->reader = NULL;
    p->width = p->height = 0;
    p->duration = 0.0f; p->rate = 1.0f; p->volume = 1.0f;
    p->ready = p->playing = p->seeking = 0;
    p->seek_target = 0.0;
    p->seek_done_pulse = p->asset_info_pulse = p->force_frame = 0;
    p->last_pts_us = -1;
    p->seek_gen = p->frames = 0;
    p->in_use = 1;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&g_players_mu);
    KLJ_LOG("AVPro: MakePlayer -> %d (host AVFoundation player)", id);
    return (klj_val){.j = (uint64_t)id};
}

static void kla_close(void *env, kla_player *p) {
    pthread_mutex_lock(&p->lock);
    if (p->player) {
        [(__bridge AVPlayer *)p->player pause];
        CFRelease(p->player);
        p->player = NULL;
    }
    if (p->output) { CFRelease(p->output); p->output = NULL; }
    if (p->reader) {
        // The plugin's own DestroySurface first, so it is done with its reader
        // before the reader goes; deleting one it has deleted already is
        // ignored (klm_AImageReader_delete keeps a list of live readers for
        // DeoVR).
        static void (*destroy)(void *, void *, int);
        static int looked;
        if (!looked) {
            looked = 1;
            destroy = (void (*)(void *, void *, int))
                klb_dlsym(NULL, "Java_com_renderheads_AVPro_Video_Manager_DestroySurface");
        }
        if (destroy && env) destroy(env, NULL, p->id);
        kl_mediandk_reader_delete(p->reader);
        p->reader = NULL;
    }
    p->ready = p->playing = p->seeking = 0;
    p->seek_target = 0.0;
    p->seek_done_pulse = p->asset_info_pulse = p->force_frame = 0;
    p->last_pts_us = -1;
    p->seek_gen++;              // a completion still on its way is stale now
    p->width = p->height = 0;
    p->duration = 0.0f;
    p->frames = 0;
    pthread_mutex_unlock(&p->lock);
}

static klj_val kla_CloseVideo(void *env, void *self, const klj_val *a, int n) {
    (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    if (p) kla_close(env, p);
    return (klj_val){0};
}

static klj_val kla_DestroyPlayer(void *env, void *self, const klj_val *a, int n) {
    (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    if (!p) return (klj_val){0};
    kla_close(env, p);
    pthread_mutex_lock(&g_players_mu);
    p->in_use = 0;              // the lock stays: a late completion may still take it
    pthread_mutex_unlock(&g_players_mu);
    return (klj_val){0};
}

// ---- Player_OpenURL ----
// Under p->lock, inside an autorelease pool. 1 when the player is playing.
static int kla_open(void *env, kla_player *p, int id, const char *url, const char *headers) {
    NSURL *ns_url = nil;
    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) {
        ns_url = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    } else {
        char real[1024];
        const char *mapped = kl_guest_path(url, real, sizeof real);
        NSString *path = [NSString stringWithUTF8String:mapped ? mapped : url];
        ns_url = path && [[NSFileManager defaultManager] fileExistsAtPath:path]
                     ? [NSURL fileURLWithPath:path]
                     : [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    }
    if (!ns_url) {
        KLJ_LOG("AVPro: no URL can be made of '%s'", url);
        return 0;
    }

    // "Name: value" lines, as AVPro hands its HTTP headers over.
    NSMutableDictionary *options = [NSMutableDictionary dictionary];
    if (headers && *headers) {
        NSMutableDictionary *hdr = [NSMutableDictionary dictionary];
        NSArray *lines = [[NSString stringWithUTF8String:headers]
            componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
        NSCharacterSet *ws = [NSCharacterSet whitespaceCharacterSet];
        for (NSString *line in lines) {
            NSRange c = [line rangeOfString:@":"];
            if (c.location == NSNotFound) continue;
            NSString *k = [[line substringToIndex:c.location] stringByTrimmingCharactersInSet:ws];
            NSString *v = [[line substringFromIndex:c.location + 1] stringByTrimmingCharactersInSet:ws];
            if (k.length && v.length) hdr[k] = v;
        }
        if (hdr.count) options[@"AVURLAssetHTTPHeaderFieldsKey"] = hdr;
    }

    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:ns_url options:options];
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
    NSDictionary *attrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    AVPlayerItemVideoOutput *output =
        KLA_OWNED([[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs]);
    [item addOutput:output];
    AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
    player.actionAtItemEnd = AVPlayerActionAtItemEndNone;
    player.volume = p->volume;
    p->player = (void *)CFBridgingRetain(player);
    p->output = (void *)CFBridgingRetain(output);

    int w = 1920, h = 1080;
    void *vt = kla_first_video_track(asset);
    if (vt) {
        CGSize sz = ((__bridge AVAssetTrack *)vt).naturalSize;
        if (sz.width > 0 && sz.height > 0) { w = (int)sz.width; h = (int)sz.height; }
        CFRelease(vt);
    }
    CMTime dur = asset.duration;   // as kl_avdec.m reads it
    p->width = w; p->height = h;
    p->duration = CMTIME_IS_NUMERIC(dur) ? (float)CMTimeGetSeconds(dur) : 0.0f;

    // The plugin's own CreateSurface makes the GL texture, the reader and the
    // Surface it reads from; frames go into that reader.
    static void *(*create)(void *, void *, int, int, int, int);
    static int looked;
    if (!looked) {
        looked = 1;
        create = (void *(*)(void *, void *, int, int, int, int))
            klb_dlsym(NULL, "Java_com_renderheads_AVPro_Video_Manager_CreateSurface");
    }
    void *surface = create ? create(env, NULL, id, w, h, 0) : NULL;
    void *win = surface ? kl_jni_surface_window(surface) : NULL;
    p->reader = win ? kl_ndk_window_owner(win) : NULL;
    KLJ_LOG("AVPro: player %d %dx%d, %.2f s, surface %p, reader %p", id, w, h,
            (double)p->duration, surface, p->reader);
    if (!surface) {
        KLJ_LOG("AVPro: %s", create ? "CreateSurface returned NULL"
                                    : "CreateSurface is not exported");
        return 0;
    }

    p->asset_info_pulse = 3;
    p->force_frame = 1;
    p->last_pts_us = -1;
    p->ready = p->playing = 1;
    [player play];
    if (p->rate > 0.0f) player.rate = p->rate;
    return 1;
}

static klj_val kla_OpenURL(void *env, void *self, const klj_val *a, int n) {
    (void)self;
    int id = kla_id(a, n);
    const char *url = n > 1 ? klj_str(a[1].l) : NULL;
    const char *headers = n > 2 ? klj_str(a[2].l) : NULL;
    kla_player *p = kla_get(id);
    KLJ_LOG("AVPro: Player_OpenURL(%d, '%s')", id, url ? url : "null");
    if (!p || !url || !*url) return (klj_val){.j = 0};
    kla_close(env, p);
    pthread_mutex_lock(&p->lock);
    int ok;
    @autoreleasepool { ok = kla_open(env, p, id, url, headers); }
    pthread_mutex_unlock(&p->lock);
    return (klj_val){.j = (uint64_t)ok};
}

// ---- Player_Update: hand the reader a frame when the output has a new one ----
static klj_val kla_Update(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = kla_id(a, n);
    kla_player *p = kla_get(id);
    if (!p) return (klj_val){0};
    pthread_mutex_lock(&p->lock);
    if (p->player && p->output && p->reader) {
        @autoreleasepool {
            AVPlayer *player = (__bridge AVPlayer *)p->player;
            AVPlayerItemVideoOutput *output = (__bridge AVPlayerItemVideoOutput *)p->output;
            AVPlayerItem *item = player.currentItem;
            if (item) {
                CMTime t = item.currentTime;
                // Only a NEW frame is copied: at 8K a frame is ~134 MB, and
                // copying one per update whether or not it changed was the
                // memory and stutter the sender saw.
                if (p->force_frame || [output hasNewPixelBufferForItemTime:t]) {
                    CVPixelBufferRef pb = [output copyPixelBufferForItemTime:t itemTimeForDisplay:NULL];
                    if (pb) {
                        int force = p->force_frame;
                        p->force_frame = 0;
                        int64_t pts = (int64_t)(CMTimeGetSeconds(t) * 1e6);
                        if (force || pts != p->last_pts_us || !p->frames) {
                            p->last_pts_us = pts;
                            int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
                            if (w > 0 && h > 0 && (w != p->width || h != p->height)) {
                                p->width = w; p->height = h;
                                p->asset_info_pulse = 2;
                            }
                            kl_mediandk_reader_publish(p->reader, pb, pts);
                            if (!p->frames++)
                                KLJ_LOG("AVPro: player %d first frame %dx%d", id, w, h);
                        }
                        CVPixelBufferRelease(pb);
                    }
                }
                if (p->width <= 0 || p->height <= 0) {
                    CGSize sz = item.presentationSize;
                    if (sz.width > 0 && sz.height > 0) { p->width = (int)sz.width; p->height = (int)sz.height; }
                }
                if (p->duration <= 0.0f) {
                    CMTime dur = item.duration;
                    if (CMTIME_IS_NUMERIC(dur)) p->duration = (float)CMTimeGetSeconds(dur);
                }
            }
        }
    }
    pthread_mutex_unlock(&p->lock);
    return (klj_val){0};
}

// ---- Player_GetState ----
// AVPro Video 3.2.8's state, as libAVProVideo2Native reads the double[]:
//   [0] flags: 0x1 ReadyToPlay, 0x2 Playing, 0x4 Paused, 0x10 Seeking,
//       0x20 Buffering, 0x200 FinishedSeeking, 0x10000 UpdatedAssetInfo,
//       0x40000 / 0x80000 UpdatedBuffered / SeekableTimeRanges,
//       0x1000000 HasVideo, 0x2000000 HasAudio. (0x20000 UpdatedTexture and
//       0x200000 UpdatedTextureTransform are the plugin's own.)
//   [1] current time, [2] duration, [3] video track, [4] audio track,
//   [5] text track, [6] buffered ranges, [7] seekable ranges,
//   [8] audio capture samples, [9] variant.
static klj_val kla_GetState(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    void *arr = klj_new_array('D', NULL, 16);
    double *st = (double *)klj_arr(arr)->data;
    for (int i = 0; i < 16; i++) st[i] = 0.0;
    uint32_t flags = 0;
    double pos = 0.0, dur = 0.0;
    int ready = 0;
    if (p) {
        pthread_mutex_lock(&p->lock);
        ready = p->ready;
        if (ready) {
            flags |= 0x1u | 0x1000000u | 0x2000000u;
            if (p->seeking) {
                flags |= 0x10u;
            } else if (p->seek_done_pulse > 0) {
                flags |= 0x200u | 0x40000u | 0x80000u;
                p->seek_done_pulse--;
            }
            if (p->asset_info_pulse > 0) {
                flags |= 0x10000u | 0x40000u | 0x80000u;
                p->asset_info_pulse--;
            }
            AVPlayer *player = (__bridge AVPlayer *)p->player;
            flags |= (p->playing && (!player || player.rate > 0.0f || p->rate > 0.0f)) ? 0x2u : 0x4u;
        } else {
            flags |= 0x20u;
        }
        if (p->seeking) {
            pos = p->seek_target;   // where it is going, not where it was
        } else if (p->player) {
            @autoreleasepool {
                CMTime t = ((__bridge AVPlayer *)p->player).currentItem.currentTime;
                if (CMTIME_IS_NUMERIC(t)) pos = CMTimeGetSeconds(t);
            }
        }
        dur = (double)p->duration;
        pthread_mutex_unlock(&p->lock);
    }
    st[0] = (double)flags;
    st[1] = pos;
    st[2] = dur;
    st[3] = ready ? 0.0 : -1.0;
    st[4] = ready ? 0.0 : -1.0;
    st[5] = -1.0;
    st[6] = ready ? 1.0 : 0.0;
    st[7] = ready ? 1.0 : 0.0;
    st[8] = 0.0;
    st[9] = -1.0;
    return (klj_val){.l = arr};
}

// ---- Player_GetAssetInfo: [1] duration, [2] width, [3] height, [4] fps,
//      [5..8] video / audio / subtitle / text track counts ----
static klj_val kla_GetAssetInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    void *arr = klj_new_array('D', NULL, 16);
    double *info = (double *)klj_arr(arr)->data;
    for (int i = 0; i < 16; i++) info[i] = 0.0;
    info[1] = p ? (double)p->duration : 0.0;
    info[2] = p && p->width > 0 ? (double)p->width : 1920.0;
    info[3] = p && p->height > 0 ? (double)p->height : 1080.0;
    info[4] = 60.0;
    info[5] = 1.0;
    info[6] = 1.0;
    return (klj_val){.l = arr};
}

static klj_val kla_GetSupportedFeatures(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0x7f};
}

// ---- Player_Seek(id, time, tolerance before, tolerance after) ----
static klj_val kla_Seek(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = kla_id(a, n);
    double t = n > 1 ? a[1].d : 0.0;
    double before = n > 2 ? a[2].d : 0.0, after = n > 3 ? a[3].d : 0.0;
    kla_player *p = kla_get(id);
    if (!p) return (klj_val){0};
    static int said;
    if (!said++) KLJ_LOG("AVPro: first seek (player %d, %.2f s)", id, t);
    pthread_mutex_lock(&p->lock);
    p->seeking = 1;
    p->seek_target = t;
    p->seek_done_pulse = 0;
    if (p->reader) kl_mediandk_reader_flush(p->reader);
    if (p->player) {
        @autoreleasepool {
            AVPlayer *player = (__bridge AVPlayer *)p->player;
            uint32_t gen = ++p->seek_gen;
            // 0.25 s either side when the guest gives no tolerance: a fast seek
            // to a nearby key frame, which is what scrubbing needs.
            CMTime tb = CMTimeMakeWithSeconds(before > 0.0 ? before : 0.25, 600);
            CMTime ta = CMTimeMakeWithSeconds(after > 0.0 ? after : 0.25, 600);
            [player seekToTime:CMTimeMakeWithSeconds(t, 600) toleranceBefore:tb toleranceAfter:ta
             completionHandler:^(BOOL finished) {
                (void)finished;
                pthread_mutex_lock(&p->lock);
                if (p->player == (__bridge void *)player && p->seek_gen == gen) {
                    p->seeking = 0;
                    p->seek_done_pulse = 2;
                    p->force_frame = 1;
                    p->asset_info_pulse = 1;
                    if (p->playing && p->rate > 0.0f) {
                        [player play];
                        player.rate = p->rate;
                    }
                }
                pthread_mutex_unlock(&p->lock);
            }];
        }
    } else {
        p->seeking = 0;
        p->seek_done_pulse = 2;
    }
    pthread_mutex_unlock(&p->lock);
    return (klj_val){0};
}

static klj_val kla_SetRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = kla_id(a, n);
    float rate = n > 1 ? (float)a[1].d : 1.0f;
    kla_player *p = kla_get(id);
    if (!p) return (klj_val){0};
    KLJ_LOG("AVPro: Player_SetRate(%d, %.2f)", id, (double)rate);
    pthread_mutex_lock(&p->lock);
    p->rate = rate;
    p->playing = rate > 0.0f;
    if (p->player) {
        AVPlayer *player = (__bridge AVPlayer *)p->player;
        if (rate > 0.0f) { [player play]; player.rate = rate; }
        else [player pause];
    }
    pthread_mutex_unlock(&p->lock);
    return (klj_val){0};
}

static klj_val kla_SetVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    if (!p) return (klj_val){0};
    pthread_mutex_lock(&p->lock);
    p->volume = n > 1 ? (float)a[1].d : 1.0f;
    if (p->player) ((__bridge AVPlayer *)p->player).volume = p->volume;
    pthread_mutex_unlock(&p->lock);
    return (klj_val){0};
}

// ---- the static queries ----
static klj_val kla_CanPlay(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};
}
static klj_val kla_IsPlaying(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    int playing = 0;
    if (p) {
        pthread_mutex_lock(&p->lock);
        playing = p->playing && p->player && ((__bridge AVPlayer *)p->player).rate > 0.0f;
        pthread_mutex_unlock(&p->lock);
    }
    return (klj_val){.j = (uint64_t)playing};
}
static klj_val kla_IsPaused(void *env, void *self, const klj_val *a, int n) {
    return (klj_val){.j = !kla_IsPlaying(env, self, a, n).j};
}
static klj_val kla_GetWidth(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    return (klj_val){.j = (uint64_t)(p && p->width > 0 ? p->width : 1920)};
}
static klj_val kla_GetHeight(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    return (klj_val){.j = (uint64_t)(p && p->height > 0 ? p->height : 1080)};
}
static klj_val kla_GetDuration(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    return (klj_val){.d = p ? (double)p->duration : 0.0};
}
static klj_val kla_GetDisplayRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.d = 60.0};
}
static klj_val kla_GetFrameCount(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    return (klj_val){.j = p ? (uint64_t)p->frames : 0};
}
static klj_val kla_two(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 2};
}

// ---- track info: Object[]{ Object[]{byte[] id, byte[] name}, double[16] } ----
static void *kla_bytes(const char *s) {
    size_t len = strlen(s);
    void *arr = klj_new_array('B', NULL, (int)len);
    if (arr) memcpy(klj_arr(arr)->data, s, len);
    return arr;
}
static klj_val kla_track_info(const char *tid, const char *name, const double *d, int nd) {
    void *names = klj_new_array('L', "java/lang/Object", 2);
    if (names) {
        ((void **)klj_arr(names)->data)[0] = kla_bytes(tid);
        ((void **)klj_arr(names)->data)[1] = kla_bytes(name);
    }
    void *vals = klj_new_array('D', NULL, 16);
    if (vals) {
        double *v = (double *)klj_arr(vals)->data;
        for (int i = 0; i < 16; i++) v[i] = i < nd ? d[i] : 0.0;
    }
    void *outer = klj_new_array('L', "java/lang/Object", 2);
    if (outer) {
        ((void **)klj_arr(outer)->data)[0] = names;
        ((void **)klj_arr(outer)->data)[1] = vals;
    }
    return (klj_val){.l = outer};
}
static klj_val kla_VideoTrackInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    int track = n > 1 ? (int)a[1].j : 0;
    if (!p || track != 0) return (klj_val){.l = NULL};
    // [1] bitrate, [2] width, [3] height, [4] fps, [5] rotation
    double d[6] = {0.0, 1e7, p->width > 0 ? p->width : 1920.0, p->height > 0 ? p->height : 1080.0, 60.0, 0.0};
    return kla_track_info("1", "Default", d, 6);
}
static klj_val kla_AudioTrackInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    int track = n > 1 ? (int)a[1].j : 0;
    if (!p || track != 0) return (klj_val){.l = NULL};
    // [1] bitrate, [2] channels, [3] sample rate
    double d[4] = {0.0, 128000.0, 2.0, 48000.0};
    return kla_track_info("1", "Default", d, 4);
}
static klj_val kla_TextTrackInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return kla_track_info("0", "None", NULL, 0);
}

static klj_val kla_TimeRanges(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    kla_player *p = kla_get(kla_id(a, n));
    void *arr = klj_new_array('D', NULL, 2);
    double *r = (double *)klj_arr(arr)->data;
    double dur = p ? (double)p->duration : 0.0;
    r[0] = 0.0;
    r[1] = dur > 0.0 ? dur : 1000.0;
    return (klj_val){.l = arr};
}

static klj_val kla_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){0};
}
static klj_val kla_one(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};
}
static klj_val kla_null(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

#define KLA_M "com/renderheads/AVPro/Video/Manager"
static const klj_binding klj_bind_avpro[] = {
    {KLA_M, "MakePlayer", "(IZIIIIZIIIIIDDZZ)I", kla_MakePlayer},
    {KLA_M, "DestroyPlayer", "(I)V", kla_DestroyPlayer},
    {KLA_M, "Player_OpenURL", "(ILjava/lang/String;Ljava/lang/String;II)Z", kla_OpenURL},
    {KLA_M, "Player_CloseVideo", "(I)V", kla_CloseVideo},
    {KLA_M, "Player_Update", "(I)V", kla_Update},
    {KLA_M, "Player_GetState", "(I)[D", kla_GetState},
    {KLA_M, "Player_GetAssetInfo", "(I)[D", kla_GetAssetInfo},
    {KLA_M, "Player_GetSupportedFeatures", "(I)I", kla_GetSupportedFeatures},
    {KLA_M, "Player_Seek", "(IDDD)V", kla_Seek},
    {KLA_M, "Player_SetRate", "(IF)V", kla_SetRate},
    {KLA_M, "Player_SetVolume", "(IF)V", kla_SetVolume},
    {KLA_M, "Player_SetFlags", "(II)V", kla_void},
    {KLA_M, "Player_SetPlayerSettings", "(IZIIIDDDZZ)V", kla_void},
    {KLA_M, "Player_SetKeyServerAuthToken", "(ILjava/lang/String;)V", kla_void},
    {KLA_M, "Player_SetDecryptionKey", "(I[BI)V", kla_void},
    {KLA_M, "Player_SetHeadRotation", "(IDDDD)V", kla_void},
    {KLA_M, "Player_SetPositionTrackingEnabled", "(IZ)V", kla_void},
    {KLA_M, "Player_SetAudioFocusEnabled", "(IZ)V", kla_void},
    {KLA_M, "Player_SetAudioFocusProperties", "(IDD)V", kla_void},
    {KLA_M, "Player_SetAudioFocusRotation", "(IDDDD)V", kla_void},
    {KLA_M, "Player_ResetAudioFocus", "(I)V", kla_void},
    {KLA_M, "Player_SetTrack", "(III)Z", kla_one},
    {KLA_M, "Player_SetVideoVariant", "(II)V", kla_void},
    {KLA_M, "Player_GetVideoTrackInfo", "(II)[Ljava/lang/Object;", kla_VideoTrackInfo},
    {KLA_M, "Player_GetAudioTrackInfo", "(II)[Ljava/lang/Object;", kla_AudioTrackInfo},
    {KLA_M, "Player_GetTextTrackInfo", "(II)[Ljava/lang/Object;", kla_TextTrackInfo},
    {KLA_M, "Player_GetVideoTrackVariantsInfo", "(II)[Ljava/lang/Object;", kla_null},
    {KLA_M, "Player_GetCurrentVideoTrackSingleVariantInfo", "(II)[Ljava/lang/Object;", kla_null},
    {KLA_M, "Player_GetSeekableTimeRanges", "(I)[D", kla_TimeRanges},
    {KLA_M, "Player_GetBufferedTimeRanges", "(I)[D", kla_TimeRanges},
    {KLA_M, "Player_GetCurrentTextCueAsByteArray", "(I)[B", kla_null},
    {KLA_M, "Player_GetCachedMediaStatus", "(ILjava/lang/String;)[D", kla_null},
    {KLA_M, "Player_AddMediaToCache", "(ILjava/lang/String;Ljava/lang/String;DII)V", kla_void},
    {KLA_M, "Player_CancelDownloadOfMediaToCache", "(ILjava/lang/String;)V", kla_void},
    {KLA_M, "Player_PauseDownloadOfMediaToCache", "(ILjava/lang/String;)V", kla_void},
    {KLA_M, "Player_ResumeDownloadOfMediaToCache", "(ILjava/lang/String;)V", kla_void},
    {KLA_M, "Player_RemoveMediaFromCache", "(ILjava/lang/String;)V", kla_void},
    {KLA_M, "SetContext", "(Landroid/content/Context;)V", kla_void},
    {KLA_M, "SetContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", kla_void},
    {KLA_M, "_CanPlay", "(I)Z", kla_CanPlay},
    {KLA_M, "_IsPlaying", "(I)Z", kla_IsPlaying},
    {KLA_M, "_IsPaused", "(I)Z", kla_IsPaused},
    {KLA_M, "_GetWidth", "(I)I", kla_GetWidth},
    {KLA_M, "_GetHeight", "(I)I", kla_GetHeight},
    {KLA_M, "_GetDuration", "(I)D", kla_GetDuration},
    {KLA_M, "_GetVideoDisplayRate", "(I)F", kla_GetDisplayRate},
    {KLA_M, "_GetFrameCount", "(I)I", kla_GetFrameCount},
    {KLA_M, "_GetCurrentAudioTrackNumChannels", "(I)I", kla_two},
    {KLA_M, "_GetCurrentVideoTrackBitrate", "(I)I", kla_null},
    {KLA_M, "_GetLastErrorCode", "(I)I", kla_null},
    {KLA_M, "RenderPlayer", "(I)V", kla_void},
    {KLA_M, "WaitForNewFramePlayer", "(I)V", kla_void},
    {KLA_M, "PauseAllPlayers", "()V", kla_void},
    {KLA_M, "PlayAllPlayers", "()V", kla_void},
    {KLA_M, "Deinitialise", "()V", kla_void},
    {KLA_M, "AudioCaptureBuffer_Initialise", "(II)V", kla_void},
    {KLA_M, "AudioCaptureBuffer_Destroy", "(I)V", kla_void},
    {KLA_M, "AudioCaptureBuffer_Reset", "(I)V", kla_void},
    {KLA_M, "AudioCaptureBuffer_OfferBytes", "(I[FI)Z", kla_null},
    {KLA_M, "AudioCaptureBuffer_Poll", "([FI)I", kla_null},
    {KLA_M, "AudioCaptureBuffer_GetSpaceUsed", "(I)I", kla_null},
    {NULL, NULL, NULL, NULL},
};

const klj_binding *klj_avpro_table(void) {
    return kl_driver_deovr_avpro() ? klj_bind_avpro : NULL;
}

// The frame size AVPPlayerGetTexture reports (kl_dl.c's wrapper).
int kl_avpro_get_dimensions(int player_id, int *w, int *h) {
    kla_player *p = kla_get(player_id);
    if (!p) return 0;
    pthread_mutex_lock(&p->lock);
    int width = p->width, height = p->height;
    pthread_mutex_unlock(&p->lock);
    if (w) *w = width;
    if (h) *h = height;
    return width > 0 && height > 0;
}
