// A VP8 decoder (RFC 6386), for the WebM clips Android games play through
// MediaCodec and Apple's frameworks cannot decode. A plain-C port of the
// libvpx decoder's logic, single-threaded and without error concealment. See
// kl_vp8.c.
#ifndef KL_VP8_H
#define KL_VP8_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_vp8 kl_vp8;

kl_vp8 *kl_vp8_new(void);
void    kl_vp8_free(kl_vp8 *d);
// Decode one compressed frame. 1: a frame to show is ready (kl_vp8_frame);
// 0: decoded but not for display (an alt-ref frame); -1: not decodable (no
// key frame yet, or a corrupt frame — the next key frame recovers).
int     kl_vp8_decode(kl_vp8 *d, const uint8_t *data, size_t len);
// The frame the last successful kl_vp8_decode showed: 8-bit 4:2:0 planes,
// valid until the next decode.
void    kl_vp8_frame(const kl_vp8 *d, const uint8_t **y, const uint8_t **u,
                     const uint8_t **v, int *y_stride, int *uv_stride,
                     int *width, int *height);

#ifdef __cplusplus
}
#endif
#endif
