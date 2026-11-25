// A WebM (Matroska) reader for the one shape Android games ship VP8 in: a
// single V_VP8 video track in SimpleBlocks or BlockGroups. AVFoundation does
// not open WebM at all, so kl_avdemux hands such a file here and serves the
// frames to AMediaExtractor exactly as it serves an MP4's. See kl_webm.c.
//
// For every title, not only the one it was written for (Mixture, community
// fix 115, toolchain 932): any guest that opens a WebM clip gets it, as does
// the extractor lock in kl_mediandk.c from the same fix. 932's commit named
// Mixture alone as the game it affects, which held back the update for every
// other game built before it; this note is the commit that says otherwise.
#ifndef KL_WEBM_H
#define KL_WEBM_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_webm kl_webm;

// NULL when the file is not WebM/Matroska, or carries no V_VP8 track (then
// the reason is logged once per call).
kl_webm  *kl_webm_open(const char *path);
// The same for a WebM stored inside another file (size 0: to its end).
kl_webm  *kl_webm_open_range(const char *path, long long offset, long long size);
void      kl_webm_close(kl_webm *m);
// The track's codec as an Android MIME type ("video/x-vnd.on2.vp8").
const char *kl_webm_mime(const kl_webm *m);
void      kl_webm_info(const kl_webm *m, int *w, int *h, long long *duration_us,
                       float *fps);
// The current frame. Time is -1 and read is -1 at end of stream.
long long kl_webm_time_us(const kl_webm *m);
int       kl_webm_keyframe(const kl_webm *m);
long      kl_webm_read(const kl_webm *m, unsigned char *buf, unsigned long cap);
int       kl_webm_advance(kl_webm *m);              // 0 at end of stream
int       kl_webm_seek_us(kl_webm *m, long long us); // to the keyframe at or before

#ifdef __cplusplus
}
#endif
#endif
