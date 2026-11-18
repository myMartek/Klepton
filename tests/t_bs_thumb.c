// Bigscreen MediaInfo thumbnails: the GL upload must not inherit the ambient
// pixel-unpack state the guest's last texture upload left behind (2026-10-31).
//
// The guest shares the GL context, so GL_UNPACK_ROW_LENGTH, the skip values,
// the alignment and a bound pixel-unpack buffer are all ambient state that
// glTexImage2D reads. The upload has to be independent of them, and the guest
// has to get its own values back — including when the query that read them is
// not trustworthy, which is what wrote GL_INVALID_VALUE into its stream.
//
// There is no ANGLE on a bare checkout (vendor/out/Debug is built by
// `make angle-all`), so the contract is tested against a fake GL: save, set
// tight, upload, restore — and fail the entry instead of handing the guest a
// texture the upload did not reach.
#include <stdint.h>
#include <stdio.h>

#include "../runtime/gfx/kl_gltex.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

// ---- the slice of GL this test speaks ----
#define TEX2D_      0x0DE1u
#define RGBA8_      0x8058u
#define RGBA_FMT_   0x1908u
#define UBYTE_      0x1401u
#define MIN_FILTER_ 0x2801u
#define MAG_FILTER_ 0x2800u
#define WRAP_S_     0x2802u
#define WRAP_T_     0x2803u
#define LINEAR_     0x2601u
#define CLAMP_EDGE_ 0x812Fu
#define REPEAT_     0x2901u
#define TEX_BIND_   0x8069u
#define ROW_LENGTH_ 0x0CF2u
#define SKIP_ROWS_  0x0CF3u
#define SKIP_PIXELS_ 0x0CF4u
#define ALIGNMENT_  0x0CF5u
#define PBO_BIND_   0x88EFu
#define PBO_TARGET_ 0x88ECu
#define GL_INVALID_OPERATION_ 0x0502u

// Ambient state, as a guest leaves it: a 1920-wide upload in flight.
static int32_t  g_rowlen, g_skip_rows, g_skip_px, g_align;
static uint32_t g_pbo, g_bound_tex;

// What the upload saw, snapshotted at the moment of glTexImage2D.
static int32_t  g_at_rowlen, g_at_skip_rows, g_at_skip_px, g_at_align;
static uint32_t g_at_pbo;

static int32_t  g_img_level, g_img_internal, g_img_w, g_img_h, g_img_border;
static uint32_t g_img_format, g_img_type;
static const void *g_img_pixels;
static int      g_img_calls;

static uint32_t g_gen_name = 77;
static uint32_t g_deleted;
static int      g_delete_calls;

static int32_t  g_params[8][2];
static int      g_param_calls;
static int      g_bad_store;     // a pixel-store write GL would reject

static uint32_t g_error_after;   // returned by the second glGetError call
static int      g_err_calls;

static void reset(void) {
    g_rowlen = 1920; g_skip_rows = 3; g_skip_px = 5; g_align = 8;
    g_pbo = 42; g_bound_tex = 9;
    g_at_rowlen = g_at_skip_rows = g_at_skip_px = g_at_align = -1;
    g_at_pbo = 0;
    g_img_calls = 0;
    g_img_pixels = NULL;
    g_gen_name = 77;
    g_deleted = 0; g_delete_calls = 0;
    g_param_calls = 0;
    g_error_after = 0; g_err_calls = 0;
    g_bad_store = 0;
}

static void f_gen(int32_t n, uint32_t *names) {
    for (int32_t i = 0; i < n; i++) names[i] = g_gen_name;
}
static void f_delete(int32_t n, const uint32_t *names) {
    g_delete_calls++;
    if (n > 0 && names) g_deleted = names[0];
}
static void f_bind_tex(uint32_t target, uint32_t tex) {
    if (target == TEX2D_) g_bound_tex = tex;
}
static void f_param(uint32_t target, uint32_t pname, int32_t value) {
    (void)target;
    if (g_param_calls < 8) {
        g_params[g_param_calls][0] = (int32_t)pname;
        g_params[g_param_calls][1] = value;
        g_param_calls++;
    }
}
static void f_image(uint32_t target, int32_t level, int32_t internal,
                    int32_t w, int32_t h, int32_t border, uint32_t format,
                    uint32_t type, const void *pixels) {
    (void)target;
    g_img_calls++;
    g_img_level = level; g_img_internal = internal;
    g_img_w = w; g_img_h = h; g_img_border = border;
    g_img_format = format; g_img_type = type; g_img_pixels = pixels;
    g_at_rowlen = g_rowlen; g_at_skip_rows = g_skip_rows;
    g_at_skip_px = g_skip_px; g_at_align = g_align; g_at_pbo = g_pbo;
}
static void f_store(uint32_t pname, int32_t value) {    // GL semantics, because the restore path depends on them: an alignment
    // that is not 1/2/4/8 is INVALID_VALUE and the state does NOT change.
    // Writing back a value read from a query we do not trust is therefore not
    // a harmless no-op — it is a GL error injected into the guest's stream.
    switch (pname) {
    case ROW_LENGTH_:  if (value < 0) { g_bad_store++; return; } g_rowlen = value; break;
    case SKIP_ROWS_:   if (value < 0) { g_bad_store++; return; } g_skip_rows = value; break;
    case SKIP_PIXELS_: if (value < 0) { g_bad_store++; return; } g_skip_px = value; break;
    case ALIGNMENT_:
        if (value != 1 && value != 2 && value != 4 && value != 8) { g_bad_store++; return; }
        g_align = value; break;
    }
}
static void f_get(uint32_t pname, int32_t *out) {
    switch (pname) {
    case TEX_BIND_:  *out = (int32_t)g_bound_tex; break;
    case ROW_LENGTH_: *out = g_rowlen; break;
    case SKIP_ROWS_:  *out = g_skip_rows; break;
    case SKIP_PIXELS_: *out = g_skip_px; break;
    case ALIGNMENT_:  *out = g_align; break;
    case PBO_BIND_:   *out = (int32_t)g_pbo; break;
    default: *out = 0; break;
    }
}
static void f_bindbuf(uint32_t target, uint32_t buffer) {
    if (target == PBO_TARGET_) g_pbo = buffer;
}
static uint32_t f_err(void) {
    g_err_calls++;
    // A pending error from the guest is one call; ours is the next.
    return g_err_calls >= 2 ? g_error_after : 0;
}

static const kl_gltex_ops g_ops = {
    f_gen, f_delete, f_bind_tex, f_param, f_image, f_store, f_get, f_bindbuf, f_err,
};

int main(void) {
    printf("=== bigscreen thumbnail upload ===\n");

    // ---- 1. tight state at the upload, ambient state restored after ----
    reset();
    static const unsigned char px[226 * 128 * 4];
    uint32_t tex = kl_gltex_upload_rgba8(&g_ops, px, 226, 128, CLAMP_EDGE_);
    ok(tex == 77, "returns the generated texture name");
    ok(g_img_calls == 1, "uploads exactly once");
    ok(g_at_rowlen == 0, "UNPACK_ROW_LENGTH is 0 at the upload");
    ok(g_at_skip_rows == 0, "UNPACK_SKIP_ROWS is 0 at the upload");
    ok(g_at_skip_px == 0, "UNPACK_SKIP_PIXELS is 0 at the upload");
    ok(g_at_align == 1, "UNPACK_ALIGNMENT is 1 at the upload");
    ok(g_at_pbo == 0, "no pixel-unpack buffer is bound at the upload");
    ok(g_img_internal == (int32_t)RGBA8_ && g_img_format == RGBA_FMT_ &&
       g_img_type == UBYTE_ && g_img_level == 0 && g_img_border == 0,
       "uploads RGBA8 from UNSIGNED_BYTE, level 0, no border");
    ok(g_img_w == 226 && g_img_h == 128 && g_img_pixels == px,
       "uploads the caller's 226x128 buffer");
    ok(g_rowlen == 1920 && g_skip_rows == 3 && g_skip_px == 5 && g_align == 8,
       "restores the ambient UNPACK state");
    ok(g_pbo == 42, "restores the ambient pixel-unpack buffer");
    ok(g_bound_tex == 9, "restores the texture binding");
    ok(g_delete_calls == 0, "keeps the texture on success");
    ok(g_param_calls == 4, "sets the four sampler parameters");
    ok(g_param_calls == 4 &&
       g_params[0][0] == (int32_t)MIN_FILTER_ && g_params[0][1] == (int32_t)LINEAR_ &&
       g_params[1][0] == (int32_t)MAG_FILTER_ && g_params[1][1] == (int32_t)LINEAR_ &&
       g_params[2][0] == (int32_t)WRAP_S_ && g_params[2][1] == (int32_t)CLAMP_EDGE_ &&
       g_params[3][0] == (int32_t)WRAP_T_ && g_params[3][1] == (int32_t)CLAMP_EDGE_,
       "LINEAR min/mag, CLAMP_TO_EDGE s/t");

    // ---- 2. a failed upload is deleted, not handed to the guest ----
    reset();
    g_error_after = GL_INVALID_OPERATION_;
    tex = kl_gltex_upload_rgba8(&g_ops, px, 226, 128, CLAMP_EDGE_);
    ok(tex == 0, "returns 0 when the upload raises a GL error");
    ok(g_delete_calls == 1 && g_deleted == 77, "deletes the half-made texture");
    ok(g_rowlen == 1920 && g_align == 8 && g_pbo == 42 && g_bound_tex == 9,
       "still restores the ambient state on failure");

    // ---- 3. a name of 0 means the entry retries later ----
    reset();
    g_gen_name = 0;
    tex = kl_gltex_upload_rgba8(&g_ops, px, 226, 128, CLAMP_EDGE_);
    ok(tex == 0, "returns 0 when glGenTextures yields no name");
    ok(g_img_calls == 0, "does not upload into a nameless texture");
    ok(g_rowlen == 1920 && g_align == 8 && g_pbo == 42,
       "restores the ambient state with no name");

    // ---- 4. an untrustworthy saved value is not written back ----
    // What the device actually answered (2026-10-31): the pixel-store pnames
    // were not in the shim's state table, so glGetIntegerv returned 0 for
    // UNPACK_ALIGNMENT. Restoring that verbatim is GL_INVALID_VALUE — Unity
    // reported it as "OPENGL NATIVE PLUG-IN ERROR: GL_INVALID_VALUE" — and
    // because the write is rejected the guest is left at the alignment we set
    // (1) instead of the one it had (4). Answer with the default instead.
    reset();
    g_align = 0;                       // what the shim says when it cannot tell
    tex = kl_gltex_upload_rgba8(&g_ops, px, 226, 128, CLAMP_EDGE_);
    ok(tex == 77, "uploads normally when the saved alignment is untrustworthy");
    ok(g_at_align == 1, "still uploads tightly packed");
    ok(g_bad_store == 0, "never writes a pixel-store value GL would reject");
    ok(g_align == 4, "restores the GL default alignment, not 0");

    // ---- 5. the wrap mode is the caller's; 0 leaves the sampler default ----
    // The reference (Java) path sets no wrap at all, so GL's default REPEAT is
    // what its thumbnails actually have. Ours pinned CLAMP_TO_EDGE, and that is
    // the one difference that turns "guest samples outside 0..1" from a wrapped
    // picture into a single texel column stretched across the card.
    reset();
    tex = kl_gltex_upload_rgba8(&g_ops, px, 226, 128, 0);
    ok(tex == 77, "uploads when asked to leave the sampler default");
    ok(g_param_calls == 2, "wrap 0 sets only the two filters");
    reset();
    tex = kl_gltex_upload_rgba8(&g_ops, px, 226, 128, REPEAT_);
    ok(tex == 77, "uploads with REPEAT");
    ok(g_param_calls == 4 && g_params[2][0] == (int32_t)WRAP_S_ &&
       g_params[2][1] == (int32_t)REPEAT_ && g_params[3][0] == (int32_t)WRAP_T_ &&
       g_params[3][1] == (int32_t)REPEAT_, "REPEAT on both axes");

    printf("%s bigscreen thumbnail upload\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
