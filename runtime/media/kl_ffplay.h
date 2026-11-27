// Films through KleptonFFmpeg.framework (runtime/media/klff.h), for what
// AVFoundation cannot open: Bigscreen Beta's MKV, AVI, WebM, TS/M2TS, WMV,
// OGV. kl_bsplayer.c takes this path only when kl_avdec/kl_avdemux refuse a
// file, so every film that played before plays exactly as before.
//
// The framework is dlopened on first use and never linked; without it
// (another game, a toolchain built without `make ffmpeg`, KL_FFMPEG=0)
// kl_ff_available() is 0 and nothing here does anything.
//
// The two halves mirror the AVFoundation pair they stand in for:
//   kl_ffvid  ~ kl_avdec   a decode thread that paces BGRA frames on the wall
//                          clock and publishes the newest under a serial
//   kl_ffaud  ~ kl_avdemux a sound cursor in 1024-frame s16 chunks
#ifndef KL_FFPLAY_H
#define KL_FFPLAY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// 1 when the framework is loaded (it is loaded by this call).
int kl_ff_available(void);

typedef struct kl_ffvid kl_ffvid;
kl_ffvid *kl_ffvid_open(const char *path, char *err, size_t cap);
void      kl_ffvid_close(kl_ffvid *v);
int       kl_ffvid_info(kl_ffvid *v, int *w, int *h, const char **mime,
                        long long *duration_us, float *fps);
void      kl_ffvid_play(kl_ffvid *v, int on);
void      kl_ffvid_seek(kl_ffvid *v, long long ms);
long long kl_ffvid_position_ms(kl_ffvid *v);
int       kl_ffvid_complete(kl_ffvid *v);
const void *kl_ffvid_frame(kl_ffvid *v, int *w, int *h, size_t *bytes,
                           unsigned long long *serial);
void      kl_ffvid_frame_lock(kl_ffvid *v);
void      kl_ffvid_frame_unlock(kl_ffvid *v);

typedef struct kl_ffaud kl_ffaud;
kl_ffaud *kl_ffaud_open(const char *path);      // NULL: no sound FFmpeg decodes
void      kl_ffaud_close(kl_ffaud *a);
int       kl_ffaud_info(kl_ffaud *a, int *rate, int *channels);
long long kl_ffaud_time_us(kl_ffaud *a);          // -1 at the end
long      kl_ffaud_read(kl_ffaud *a, unsigned char *buf, unsigned long cap);
int       kl_ffaud_advance(kl_ffaud *a);
int       kl_ffaud_seek_us(kl_ffaud *a, long long us);
// The file's audio streams, one line each (klff.h's klff_audio_tracks): how
// many, -1 when this FFmpeg build cannot list them.
int       kl_ffaud_tracks(kl_ffaud *a, char *buf, size_t cap);
// Read the nth audio stream from now on (seek afterwards); 0 or -1. info
// then reports the new stream's rate and channels.
int       kl_ffaud_select(kl_ffaud *a, int nth);

// Subtitles (text only): a source is a film's subtitle stream or a file
// beside it. tracks lists a file's subtitle streams (klff_sub_tracks' lines),
// 0 for none, -1 without a framework that reads them. A cue is plain text,
// lines split by '\n', with its start and end in us: next gives 1, 0 at the
// end. seek drops cues that ended before the target.
typedef struct kl_ffsub kl_ffsub;
int       kl_ffsub_tracks(const char *path, char *buf, size_t cap);
kl_ffsub *kl_ffsub_open(const char *path, int nth);
void      kl_ffsub_close(kl_ffsub *s);
int       kl_ffsub_next(kl_ffsub *s, char *text, size_t cap, long long *start_us, long long *end_us);
int       kl_ffsub_seek_us(kl_ffsub *s, long long us);

#ifdef __cplusplus
}
#endif
#endif
