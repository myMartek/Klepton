// One RGBA8 texture from tightly packed pixels, with the pixel-unpack state
// pinned for the duration of the upload.
//
// The guest shares our GL context, so GL_UNPACK_ROW_LENGTH, the skip values,
// the alignment and a bound PIXEL_UNPACK_BUFFER are all ambient state that
// glTexImage2D reads — whatever its own last upload left behind. Pinning them
// makes this upload independent of that, and the restore hands the guest back
// exactly what it had.
//
// This is correctness, not the thumbnail bug. The stripes were the sampler wrap
// mode: the guest samples u outside 0..1, and CLAMP_TO_EDGE collapsed the card
// onto one texel column where the reference relies on GL's default REPEAT (see
// klovrp_bsinfo_thumb_upload). What this path did cause was one
// GL_INVALID_VALUE per upload — it restored an alignment it had read from a
// pname the shim answered with 0 (2026-10-31).
#include "kl_gltex.h"

#define GL_TEXTURE_2D_            0x0DE1u
#define GL_RGBA8_                 0x8058u
#define GL_RGBA_                  0x1908u
#define GL_UNSIGNED_BYTE_         0x1401u
#define GL_MIN_FILTER_            0x2801u
#define GL_MAG_FILTER_            0x2800u
#define GL_WRAP_S_                0x2802u
#define GL_WRAP_T_                0x2803u
#define GL_LINEAR_                0x2601u
#define GL_TEXTURE_BINDING_2D_    0x8069u
#define GL_UNPACK_ROW_LENGTH_     0x0CF2u
#define GL_UNPACK_SKIP_ROWS_      0x0CF3u
#define GL_UNPACK_SKIP_PIXELS_    0x0CF4u
#define GL_UNPACK_ALIGNMENT_      0x0CF5u
#define GL_PIXEL_UNPACK_BUFFER_   0x88ECu
#define GL_PIXEL_UNPACK_BUFFER_BINDING_ 0x88EFu

uint32_t kl_gltex_upload_rgba8(const kl_gltex_ops *ops, const void *rgba,
                               int w, int h, int wrap) {
    if (!ops || !rgba || w <= 0 || h <= 0) return 0;
    if (!ops->gen_textures || !ops->bind_texture || !ops->tex_parameteri ||
        !ops->tex_image2d || !ops->pixel_storei) return 0;

    // The ambient state is the guest's, not ours: save it, pin it tight for
    // the upload, put it back. A pending guest error is read once so a stale
    // one cannot make this upload look failed.
    int32_t save_tex = 0, rowlen = 0, skip_rows = 0, skip_px = 0, align = 4;
    int32_t pbo = 0;
    if (ops->get_integerv) {
        ops->get_integerv(GL_TEXTURE_BINDING_2D_, &save_tex);
        ops->get_integerv(GL_UNPACK_ROW_LENGTH_, &rowlen);
        ops->get_integerv(GL_UNPACK_SKIP_ROWS_, &skip_rows);
        ops->get_integerv(GL_UNPACK_SKIP_PIXELS_, &skip_px);
        ops->get_integerv(GL_UNPACK_ALIGNMENT_, &align);
        ops->get_integerv(GL_PIXEL_UNPACK_BUFFER_BINDING_, &pbo);
    }
    // A value we cannot trust is not restored verbatim. On device the
    // pixel-store pnames were not in the shim's state table, so the query
    // answered 0 and putting that back was GL_INVALID_VALUE — reported by the
    // guest as "OPENGL NATIVE PLUG-IN ERROR: GL_INVALID_VALUE" — and, because
    // GL rejects it, left the guest at the alignment this upload had set (1)
    // instead of the 4 it had. Answer the default in that case (2026-10-31).
    if (align != 1 && align != 2 && align != 4 && align != 8) align = 4;
    if (rowlen < 0) rowlen = 0;
    if (skip_rows < 0) skip_rows = 0;
    if (skip_px < 0) skip_px = 0;
    uint32_t before = ops->get_error ? ops->get_error() : 0;

    // With a pixel-unpack buffer bound, the pixel pointer is an offset into
    // that buffer: the upload would read the wrong memory or be refused.
    if (pbo && ops->bind_buffer)
        ops->bind_buffer(GL_PIXEL_UNPACK_BUFFER_, 0);
    ops->pixel_storei(GL_UNPACK_ROW_LENGTH_, 0);
    ops->pixel_storei(GL_UNPACK_SKIP_ROWS_, 0);
    ops->pixel_storei(GL_UNPACK_SKIP_PIXELS_, 0);
    ops->pixel_storei(GL_UNPACK_ALIGNMENT_, 1);

    uint32_t tex = 0;
    ops->gen_textures(1, &tex);
    if (tex) {
        ops->bind_texture(GL_TEXTURE_2D_, tex);
        ops->tex_parameteri(GL_TEXTURE_2D_, GL_MIN_FILTER_, GL_LINEAR_);
        ops->tex_parameteri(GL_TEXTURE_2D_, GL_MAG_FILTER_, GL_LINEAR_);
        // `wrap` 0 means "leave the sampler default", which is what the
        // reference path does — and its default is REPEAT, not CLAMP.
        if (wrap) {
            ops->tex_parameteri(GL_TEXTURE_2D_, GL_WRAP_S_, wrap);
            ops->tex_parameteri(GL_TEXTURE_2D_, GL_WRAP_T_, wrap);
        }
        ops->tex_image2d(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, GL_RGBA_,
                         GL_UNSIGNED_BYTE_, rgba);
    }

    uint32_t err = ops->get_error ? ops->get_error() : 0;

    ops->bind_texture(GL_TEXTURE_2D_, (uint32_t)save_tex);
    ops->pixel_storei(GL_UNPACK_ROW_LENGTH_, rowlen);
    ops->pixel_storei(GL_UNPACK_SKIP_ROWS_, skip_rows);
    ops->pixel_storei(GL_UNPACK_SKIP_PIXELS_, skip_px);
    ops->pixel_storei(GL_UNPACK_ALIGNMENT_, align);
    if (pbo && ops->bind_buffer)
        ops->bind_buffer(GL_PIXEL_UNPACK_BUFFER_, (uint32_t)pbo);

    if (tex && err && err != before) {
        if (ops->delete_textures) ops->delete_textures(1, &tex);
        return 0;
    }
    return tex;
}
