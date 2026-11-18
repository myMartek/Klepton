// Bigscreen MediaInfo flatbuffer + base64 (host-side, no guest).
//
// The guest's file UI is built from a base64 FlatBuffers table delivered via
// UnitySendMessage: slots 0=requestId, 3=durationMs, 4=mediaWidth,
// 5=mediaHeight, 6=textureWidth, 7=textureHeight, 8=textureId (9-slot table;
// from the dex: MediaInfo.addRequestId/addDurationMs/…/addTextureId,
// startObject(9)). All scalars, so the layout is fixed: 4-byte root uoffset,
// 22-byte vtable, 2 pad bytes, 40-byte table (68 bytes total). The table's
// soffset is table_pos - vtable_pos = +24, since the vtable precedes it.
#ifndef KL_BSFBS_H
#define KL_BSFBS_H

#include <stddef.h>

// Encode the table; mallocs `*out` (`*len` bytes). 0 = ok, -1 = bad args.
int kl_bsinfo_encode(int request_id, long long duration_ms, int media_w,
                     int media_h, int tex_w, int tex_h, int tex_id,
                     unsigned char **out, size_t *len);
// Base64 (padded) of `n` bytes; malloc'd, caller frees. NULL on bad args.
char *kl_bsinfo_base64(const unsigned char *b, size_t n);

// Bilinear BGRA scale into a caller-provided `dw * dh * 4` buffer — the
// host-side equivalent of Android's createScaledBitmap(..., filter=true),
// which the original Java layer uses before uploading the thumbnail. The
// guest builds its Texture2D at the requested size, so the pixels must be
// scaled here rather than uploaded at media resolution. 0 = ok, -1 = bad
// args (dst is left untouched).
int kl_bsinfo_scale_bgra(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh);

// A synthetic coordinate ramp in BGRA, for `KL_BS_THUMB_TEST=1`. Every texel
// encodes its own position, so the picture the guest puts on screen says
// exactly what it did with the texture: R = y, G = x, B = 0, A = 255, with
// every 16th row white as a ruler. A correct sample is one smooth left-to-right
// green ramp with one red ramp down it; a tiled, flipped or transposed sample
// is counted off it directly. 0 = ok, -1 = bad args.
int kl_bsinfo_test_pattern(unsigned char *bgra, int w, int h);

#endif
