// kl_bsplayer — Bigscreen's local-file session (MP4 only).
//
// A thin shell around two existing, tested pieces: kl_avdec (the all-in-one
// file player: open/play/seek/BGRA frames, runtime/media/kl_avdec.h) for the
// picture and transport, and kl_avdemux (track facts) for the mime. Bigscreen
// needs a local-file SESSION, not Unity's NDK extractor+codec pair, so the
// demuxer is facts-only here and every frame comes out of kl_avdec.
//
// Only what AVFoundation demuxes plays: MP4/MOV with H.264/HEVC (+AAC). MKV,
// DTS and anything else answer NULL with a reason — the guest's own
// "cannot play this file" path, never a hang.
#ifndef KL_BSPLAYER_H
#define KL_BSPLAYER_H

#include <stddef.h>

typedef struct kl_bsplayer kl_bsplayer;

// Open a local movie. NULL + human-readable `reason` (always terminated when
// cap > 0) if the file is missing, unreadable, or carries no AVFoundation
// video track. Opening reads the header only, never the whole file.
kl_bsplayer *kl_bsplayer_open(const char *path, char *reason, size_t cap);
// Track facts off the asset (`mime` is "video/avc"/"video/hevc", static).
// Any out-param may be NULL. 0 = ok, -1 = bad player.
int kl_bsplayer_info(kl_bsplayer *p, int *w, int *h, const char **mime,
                     long long *dur_us);
// NULL-safe and idempotent.
void kl_bsplayer_close(kl_bsplayer *p);

// Synchronously decode the first frame: plays the session, waits for the
// first published frame (up to ~10 s), pauses again and hands out a tightly
// packed BGRA copy the caller frees. 0 = ok, -1 = none (bad player, timeout,
// out of memory). NULL-safe.
int kl_bsplayer_decode_first(kl_bsplayer *p, unsigned char **bgra, int *w,
                             int *h);

// Frame delivery into the guest texture. The uploader copies w*h*4 BGRA bytes
// (synchronously, before it returns); the default is none. `ctx` is opaque.
// Returns 0 on ok.
typedef int (*kl_bsplayer_upload_fn)(void *ctx, const unsigned char *bgra,
                                     int w, int h, unsigned long long serial);
void kl_bsplayer_set_uploader(kl_bsplayer *p, kl_bsplayer_upload_fn fn,
                              void *ctx);

// Latch the newest decoder frame into the guest texture via the uploader:
// 0 = latched, 1 = no new frame since the last latch (or none yet),
// -1 = bad player, or a new frame with no uploader / failed upload.
// Copies under the decoder's frame lock, so the pump may keep running.
int kl_bsplayer_present(kl_bsplayer *p);

// Transport + events. play/pause/resume/stop are idempotent (a repeated call
// fires nothing new); stop also rewinds to 0. seek_to_us clamps to
// [0, duration] and returns the clamped target in us. Volume is clamped to
// [0, 1], applied as gain in the audio pump, and readable back.
// Events go to the injected listener, each at most once per arm: READY on
// the first play, PLAYING on play/resume, PAUSED on pause/stop, ENDED when
// a played stream runs out (latched in kl_avdec), ERROR is reserved.
typedef enum {
    KL_BS_EV_READY,
    KL_BS_EV_PLAYING,
    KL_BS_EV_PAUSED,
    KL_BS_EV_ENDED,
    KL_BS_EV_ERROR
} kl_bsplayer_event;
typedef void (*kl_bsplayer_listener_fn)(void *ctx, kl_bsplayer_event ev);
void kl_bsplayer_set_listener(kl_bsplayer *p, kl_bsplayer_listener_fn fn,
                              void *ctx);
void kl_bsplayer_play(kl_bsplayer *p);
void kl_bsplayer_pause(kl_bsplayer *p);
void kl_bsplayer_resume(kl_bsplayer *p);
void kl_bsplayer_stop(kl_bsplayer *p);
long long kl_bsplayer_seek_to_us(kl_bsplayer *p, long long us);
void kl_bsplayer_set_volume(kl_bsplayer *p, float v);
float kl_bsplayer_volume(kl_bsplayer *p);
// Film sound: facts off the demuxer's audio track (1 = has sound, fills
// rate/channels; 0 = silent clip, outs untouched). The PCM is 16-bit
// interleaved, at most two channels, in 1024-frame chunks.
int kl_bsplayer_audio_info(kl_bsplayer *p, int *rate, int *channels);
// Where the PCM goes. The pump thread drives the sink: open once per play
// with the film's facts, write device-paced chunks with the volume gain
// already applied, play/pause/flush on the transport edges, close at the
// end. NULL clears back to no output (the default: stored volume, silence).
typedef struct {
    int (*open)(void *ctx, unsigned rate, unsigned ch);
    size_t (*write)(void *ctx, const void *pcm, size_t bytes);
    void (*play)(void *ctx);
    void (*pause)(void *ctx);
    void (*flush)(void *ctx);
    void (*close)(void *ctx);
} kl_bsplayer_audio_sink;
void kl_bsplayer_set_audio_sink(kl_bsplayer *p,
                                const kl_bsplayer_audio_sink *sink, void *ctx);
// Playback position in ms (kept out of the original plan as un-pinnable on
// the host; the C# timeline polls it every frame, so it is pinned by use).
long long kl_bsplayer_position_ms(kl_bsplayer *p);
// Where the SOUND is, in the film's own timeline: the demuxer's audio cursor,
// i.e. the next sample the pump would hand the sink. -1 when the track has run
// out. Paired with position_ms it is the A/V offset the pump keeps in check:
// a device that stops taking audio (the system UI: the scene is silenced and
// the unit is stopped) must not move this away from the picture, and if it
// does, the pump realigns it — see kl_bsplayer_av_realigns.
long long kl_bsplayer_audio_position_us(kl_bsplayer *p);
// How many times the pump has realigned the sound to the picture this session.
// One per interruption that got through, not one per chunk: a healthy run is 0.
unsigned long kl_bsplayer_av_realigns(kl_bsplayer *p);
// Pump once: fire whatever is due (ENDED when the played stream ran out).
// 0 = ok, -1 = bad player.
int kl_bsplayer_update(kl_bsplayer *p);

#endif
