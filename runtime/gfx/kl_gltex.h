// A fresh GL texture from tightly packed RGBA8 pixels, with the pixel-unpack
// state the guest shares with us pinned for the duration of the upload.
//
// The entry points are passed in rather than resolved here: the device path
// resolves them from EGL (kl_egl_sym), and the host test passes a fake table —
// a bare checkout has no ANGLE to link against.
#ifndef KL_GLTEX_H
#define KL_GLTEX_H

#include <stdint.h>

typedef struct kl_gltex_ops {
    void (*gen_textures)(int32_t n, uint32_t *names);
    void (*delete_textures)(int32_t n, const uint32_t *names);
    void (*bind_texture)(uint32_t target, uint32_t tex);
    void (*tex_parameteri)(uint32_t target, uint32_t pname, int32_t value);
    void (*tex_image2d)(uint32_t target, int32_t level, int32_t internalformat,
                        int32_t w, int32_t h, int32_t border, uint32_t format,
                        uint32_t type, const void *pixels);
    void (*pixel_storei)(uint32_t pname, int32_t value);
    void (*get_integerv)(uint32_t pname, int32_t *params);   // optional
    void (*bind_buffer)(uint32_t target, uint32_t buffer);   // optional
    uint32_t (*get_error)(void);                             // optional
} kl_gltex_ops;

// Uploads `rgba` (w*h, 4 bytes per pixel, tightly packed) into a new
// GL_TEXTURE_2D and returns its name. 0 when GL fails — the texture is deleted
// again, so the caller can retry instead of handing the guest a broken id.
//
// `wrap` is the GL wrap mode for both axes, or 0 to leave the sampler at its
// default. That default is REPEAT, and the reference implementation relies on
// it: the Java layer's thumbnail path (glGenTextures, filter, GLUtils
// .texImage2D) sets no wrap at all. The distinction matters — with the guest
// sampling outside 0..1 on one axis, CLAMP collapses the whole card onto a
// single texel column while REPEAT wraps and shows the picture (2026-10-31).
uint32_t kl_gltex_upload_rgba8(const kl_gltex_ops *ops, const void *rgba,
                               int w, int h, int wrap);

#endif
