// VP8 decoding (RFC 6386) — see kl_vp8.h.
//
// Mixture (x4691626700899382) plays seven WebM/VP8 clips through Unity's
// VideoPlayer, one of them as the texture on the player's hands before a
// character is chosen. Neither VideoToolbox nor AVFoundation decodes VP8, so
// without this the clip never opened and the hands flickered grey for as long
// as it was wanted (device logs, 2026-11-17/18).
//
// This is a straight C port of the libvpx decoder's logic (vp8/decoder,
// vp8/common, vpx_dsp intra predictors), keeping its arithmetic exactly so the
// output matches it: the same bool decoder, token reader, mode/MV parsing with
// near-MV search, six-tap and bilinear prediction, inverse WHT/DCT and normal
// and simple loop filters. What is left out: threads, SIMD, error concealment,
// post-processing, frame scaling (the header's scale bits are ignored, as a
// MediaCodec decoder ignores them too) and temporal layers.
//
// Structure as libvpx: every frame buffer is the 16-aligned frame plus a
// replicated border, intra prediction reads the 127 row / 129 column written
// into the border of the frame being decoded, and the loop filter runs over the
// whole frame after reconstruction — intra prediction uses unfiltered pixels,
// as libvpx's row-delayed filter guarantees. Motion compensation reads through
// klv_fetch, which clamps coordinates into the frame; libvpx's own MV clamping
// "to the UMV border" is equivalent to that by construction, so it is not
// repeated.
//
// Parts are Copyright (c) 2010 The WebM project authors, BSD-3-Clause; see
// third-party-licenses/libvpx-LICENSE.
#include "kl_vp8.h"
#include "kl_vp8_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KLV_BORDER   32          // luma; chroma has half
#define KLV_NFB      4

enum { DC_PRED, V_PRED, H_PRED, TM_PRED, B_PRED,
       NEARESTMV, NEARMV, ZEROMV, NEWMV, SPLITMV };
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED,
       B_VR_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED };
enum { INTRA_FRAME, LAST_FRAME, GOLDEN_FRAME, ALTREF_FRAME };

typedef struct { int16_t row, col; } klv_mv;
static inline int mv_eq(klv_mv a, klv_mv b) { return a.row == b.row && a.col == b.col; }
static inline int mv_zero(klv_mv a) { return !a.row && !a.col; }

typedef struct {
    uint8_t ymode, uvmode, ref, segment, skip, is4x4, partitioning;
    klv_mv  mv;
    uint8_t bmode[16];
    klv_mv  bmv[16];
} klv_mb;

typedef struct {
    uint8_t *mem;
    uint8_t *y, *u, *v;          // top-left of the frame proper
    int      refs;
} klv_fb;

typedef struct {
    uint8_t coef[4][8][3][11];
    uint8_t ymode[4], uvmode[3];
    uint8_t mvc[2][19];
} klv_probs;

typedef struct { const uint8_t *p, *end; uint32_t value, range; int bits; } klv_bd;

struct kl_vp8 {
    int      w, h, mbw, mbh;     // display size; macroblocks
    int      aw, ah;             // 16-aligned size
    int      ys, uvs;            // strides
    klv_fb   fb[KLV_NFB];
    int      last, golden, altref, shown;
    klv_mb  *mbs;                // (mbw + 1) * (mbh + 1), border row/column zero
    uint8_t *above_ctx;          // 9 per macroblock column
    klv_probs p, saved;
    int      have_key;

    // Per frame.
    int      key, version, filter_type, filter_level, sharpness;
    int      seg_enabled, seg_update_map, seg_abs;
    int8_t   seg_q[4], seg_lf[4];
    uint8_t  seg_tree[3];
    int      lf_delta_on;
    int8_t   ref_lf_delta[4], mode_lf_delta[4];
    int      sign_bias[4];
    int      qindex, dq_y1dc, dq_y2dc, dq_y2ac, dq_uvdc, dq_uvac;
    int      skip_enabled, prob_skip, prob_intra, prob_last, prob_gf;
    int      nparts;
    klv_bd   part[8];
};

// ---------------------------------------------------------------------------
// Bool decoder (RFC 6386 section 7; the reference implementation's form).

static void bd_init(klv_bd *d, const uint8_t *p, size_t n) {
    d->p = p; d->end = p + n;
    d->value = 0;
    for (int i = 0; i < 2; i++) d->value = (d->value << 8) | (d->p < d->end ? *d->p++ : 0);
    d->range = 255;
    d->bits = 0;
}

static inline int bd_bool(klv_bd *d, int prob) {
    uint32_t split = 1 + (((d->range - 1) * (uint32_t)prob) >> 8);
    uint32_t big = split << 8;
    int r;
    if (d->value >= big) { r = 1; d->range -= split; d->value -= big; }
    else                 { r = 0; d->range = split; }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bits == 8) {
            d->bits = 0;
            if (d->p < d->end) d->value |= *d->p++;
        }
    }
    return r;
}

static inline int bd_lit(klv_bd *d, int n) {
    int v = 0;
    while (n--) v = (v << 1) | bd_bool(d, 128);
    return v;
}

static int bd_tree(klv_bd *d, const int8_t *t, const uint8_t *p) {
    int i = 0;
    while ((i = t[i + bd_bool(d, p[i >> 1])]) > 0) {}
    return -i;
}

// ---------------------------------------------------------------------------
// Trees and fixed probabilities (libvpx entropymode.c, entropymv.c, ...).

static const int8_t t_ymode[8]    = { -DC_PRED, 2, 4, 6, -V_PRED, -H_PRED, -TM_PRED, -B_PRED };
static const int8_t t_kf_ymode[8] = { -B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED };
static const int8_t t_uvmode[6]   = { -DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED };
static const int8_t t_bmode[18]   = {
    -B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10,
    -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED };
static const int8_t t_small_mv[14] = { 2, 8, 4, 6, -0, -1, -2, -3, 10, 12, -4, -5, -6, -7 };

static const uint8_t p_ymode_default[4]  = { 112, 86, 140, 37 };
static const uint8_t p_kf_ymode[4]       = { 145, 156, 163, 128 };
static const uint8_t p_uvmode_default[3] = { 162, 101, 204 };
static const uint8_t p_kf_uvmode[3]      = { 142, 114, 183 };
static const uint8_t p_bmode[9]          = { 120, 90, 79, 133, 87, 85, 80, 111, 151 };

static const uint8_t mv_default[2][19] = {
    { 162, 128, 225, 146, 172, 147, 214, 39, 156,
      128, 129, 132, 75, 145, 178, 206, 239, 254, 254 },
    { 164, 128, 204, 170, 119, 235, 140, 230, 228,
      128, 130, 130, 74, 148, 180, 203, 236, 254, 254 } };
static const uint8_t mv_update[2][19] = {
    { 237, 246, 253, 253, 254, 254, 254, 254, 254,
      254, 254, 254, 254, 254, 250, 250, 252, 254, 254 },
    { 231, 243, 245, 253, 254, 254, 254, 254, 254,
      254, 254, 254, 254, 254, 251, 251, 254, 254, 254 } };
enum { MVP_SHORT = 0, MVP_SIGN = 1, MVP_SHORTTREE = 2, MVP_BITS = 9, MV_LONG_BITS = 10 };

static const int mode_contexts[6][4] = {
    { 7, 1, 1, 143 }, { 14, 18, 14, 107 }, { 135, 64, 57, 68 },
    { 60, 56, 128, 65 }, { 159, 134, 128, 34 }, { 234, 188, 128, 28 } };

static const uint8_t sub_mv_ref_prob[8][3] = {
    { 147, 136, 18 }, { 223, 1, 34 }, { 106, 145, 1 }, { 208, 1, 1 },
    { 179, 121, 1 }, { 223, 1, 34 }, { 179, 121, 1 }, { 208, 1, 1 } };
static const uint8_t mbsplit_fill_count[4] = { 8, 8, 4, 1 };
static const uint8_t mbsplit_fill_offset[4][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15 },
    { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
static const uint8_t mbsplit_offset[4][16] = {
    { 0, 8 }, { 0, 2 }, { 0, 2, 8, 10 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };

static const int dc_q[128] = {
    4, 5, 6, 7, 8, 9, 10, 10, 11, 12, 13, 14, 15, 16, 17, 17, 18, 19, 20, 20,
    21, 21, 22, 22, 23, 23, 24, 25, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
    37, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 46, 47, 48, 49, 50, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
    75, 76, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 91, 93, 95, 96,
    98, 100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 122, 124, 126, 128,
    130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157 };
static const int ac_q[128] = {
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
    44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 60, 62, 64, 66, 68,
    70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108,
    110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149, 152,
    155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209,
    213, 217, 221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284 };

static const uint8_t zigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t bands[17]  = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0 };
static const uint8_t cat3[] = { 173, 148, 140, 0 };
static const uint8_t cat4[] = { 176, 155, 140, 135, 0 };
static const uint8_t cat5[] = { 180, 157, 141, 134, 130, 0 };
static const uint8_t cat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0 };
static const uint8_t *const cat3456[] = { cat3, cat4, cat5, cat6 };

static const short subpel6[8][6] = {
    { 0, 0, 128, 0, 0, 0 }, { 0, -6, 123, 12, -1, 0 }, { 2, -11, 108, 36, -8, 1 },
    { 0, -9, 93, 50, -6, 0 }, { 3, -16, 77, 77, -16, 3 }, { 0, -6, 50, 93, -9, 0 },
    { 1, -8, 36, 108, -11, 2 }, { 0, -1, 12, 123, -6, 0 } };
static const short bilin[8][2] = {
    { 128, 0 }, { 112, 16 }, { 96, 32 }, { 80, 48 },
    { 64, 64 }, { 48, 80 }, { 32, 96 }, { 16, 112 } };

static inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------------------
// Frame buffers.

static void klv_free_buffers(kl_vp8 *d) {
    for (int i = 0; i < KLV_NFB; i++) { free(d->fb[i].mem); d->fb[i].mem = NULL; }
    free(d->mbs); d->mbs = NULL;
    free(d->above_ctx); d->above_ctx = NULL;
}

static int klv_alloc(kl_vp8 *d, int w, int h) {
    klv_free_buffers(d);
    d->w = w; d->h = h;
    d->aw = (w + 15) & ~15; d->ah = (h + 15) & ~15;
    d->mbw = d->aw / 16; d->mbh = d->ah / 16;
    d->ys = d->aw + 2 * KLV_BORDER;
    d->uvs = d->aw / 2 + KLV_BORDER;
    size_t ysz = (size_t)d->ys * (d->ah + 2 * KLV_BORDER);
    size_t csz = (size_t)d->uvs * (d->ah / 2 + KLV_BORDER);
    for (int i = 0; i < KLV_NFB; i++) {
        klv_fb *f = &d->fb[i];
        f->mem = calloc(1, ysz + 2 * csz);
        if (!f->mem) return 0;
        f->y = f->mem + (size_t)KLV_BORDER * d->ys + KLV_BORDER;
        f->u = f->mem + ysz + (size_t)(KLV_BORDER / 2) * d->uvs + KLV_BORDER / 2;
        f->v = f->mem + ysz + csz + (size_t)(KLV_BORDER / 2) * d->uvs + KLV_BORDER / 2;
        f->refs = 0;
    }
    d->mbs = calloc((size_t)(d->mbw + 1) * (d->mbh + 1), sizeof *d->mbs);
    d->above_ctx = calloc((size_t)d->mbw * 9, 1);
    if (!d->mbs || !d->above_ctx) return 0;
    d->last = d->golden = d->altref = 0;
    d->fb[0].refs = 3;
    return 1;
}

static inline klv_mb *mb_at(kl_vp8 *d, int r, int c) {
    return &d->mbs[(size_t)(r + 1) * (d->mbw + 1) + (c + 1)];
}

// Replicate the frame's edges into its border, as a reference for inter frames.
static void klv_extend(uint8_t *p, int stride, int w, int h, int b) {
    for (int r = 0; r < h; r++) {
        uint8_t *row = p + (size_t)r * stride;
        memset(row - b, row[0], b);
        memset(row + w, row[w - 1], b);
    }
    for (int r = 1; r <= b; r++) {
        memcpy(p - (size_t)r * stride - b, p - b, w + 2 * b);
        memcpy(p + (size_t)(h - 1 + r) * stride - b, p + (size_t)(h - 1) * stride - b, w + 2 * b);
    }
}

// ---------------------------------------------------------------------------
// Header.

static void klv_reset_probs(kl_vp8 *d) {
    memcpy(d->p.coef, klv_default_coef_probs, sizeof d->p.coef);
    memcpy(d->p.ymode, p_ymode_default, 4);
    memcpy(d->p.uvmode, p_uvmode_default, 3);
    memcpy(d->p.mvc, mv_default, sizeof d->p.mvc);
}

static int delta_q(klv_bd *bc) {
    int v = 0;
    if (bd_bool(bc, 128)) {
        v = bd_lit(bc, 4);
        if (bd_bool(bc, 128)) v = -v;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Modes and motion vectors (libvpx decodemv.c).

static int read_mv_component(klv_bd *bc, const uint8_t *p) {
    int x = 0;
    if (bd_bool(bc, p[MVP_SHORT])) {
        for (int i = 0; i < 3; i++) x += bd_bool(bc, p[MVP_BITS + i]) << i;
        for (int i = MV_LONG_BITS - 1; i > 3; i--) x += bd_bool(bc, p[MVP_BITS + i]) << i;
        if (!(x & 0xFFF0) || bd_bool(bc, p[MVP_BITS + 3])) x += 8;
    } else {
        x = bd_tree(bc, t_small_mv, p + MVP_SHORTTREE);
    }
    if (x && bd_bool(bc, p[MVP_SIGN])) x = -x;
    return x;
}

static void mv_bias(const kl_vp8 *d, int nb_ref, int ref, klv_mv *mv) {
    if (d->sign_bias[nb_ref] != d->sign_bias[ref]) { mv->row = -mv->row; mv->col = -mv->col; }
}

typedef struct { int left, right, top, bottom; } klv_edges;   // 1/8 pel

static void clamp_mv2(klv_mv *mv, const klv_edges *e) {
    const int m = 16 << 3;
    if (mv->col < e->left - m) mv->col = (int16_t)(e->left - m);
    else if (mv->col > e->right + m) mv->col = (int16_t)(e->right + m);
    if (mv->row < e->top - m) mv->row = (int16_t)(e->top - m);
    else if (mv->row > e->bottom + m) mv->row = (int16_t)(e->bottom + m);
}

static int above_bmode(kl_vp8 *d, klv_mb *mb, int b, int r, int c) {
    if (b >= 4) return mb->bmode[b - 4];
    klv_mb *a = mb_at(d, r - 1, c);
    switch (a->ymode) {
    case B_PRED: return a->bmode[b + 12];
    case V_PRED: return B_VE_PRED;
    case H_PRED: return B_HE_PRED;
    case TM_PRED: return B_TM_PRED;
    default: return B_DC_PRED;
    }
}

static int left_bmode(kl_vp8 *d, klv_mb *mb, int b, int r, int c) {
    if (b & 3) return mb->bmode[b - 1];
    klv_mb *l = mb_at(d, r, c - 1);
    switch (l->ymode) {
    case B_PRED: return l->bmode[b + 3];
    case V_PRED: return B_VE_PRED;
    case H_PRED: return B_HE_PRED;
    case TM_PRED: return B_TM_PRED;
    default: return B_DC_PRED;
    }
}

static void read_split_mv(kl_vp8 *d, klv_bd *bc, klv_mb *mb, const klv_mb *left,
                          const klv_mb *above, klv_mv best) {
    int s = 3, num = 16;
    if (bd_bool(bc, 110)) {
        s = 2; num = 4;
        if (bd_bool(bc, 111)) { s = bd_bool(bc, 150); num = 2; }
    }
    for (int j = 0; j < num; j++) {
        int k = mbsplit_offset[s][j];
        klv_mv lmv, amv, bmv;
        if (!(k & 3)) lmv = left->ymode == SPLITMV ? left->bmv[k + 3] : left->mv;
        else          lmv = mb->bmv[k - 1];
        if (!(k >> 2)) amv = above->ymode == SPLITMV ? above->bmv[k + 12] : above->mv;
        else           amv = mb->bmv[k - 4];
        int lez = mv_zero(lmv), aez = mv_zero(amv), lea = mv_eq(lmv, amv);
        const uint8_t *prob = sub_mv_ref_prob[(aez << 2) | (lez << 1) | lea];
        if (bd_bool(bc, prob[0])) {
            if (bd_bool(bc, prob[1])) {
                bmv.row = bmv.col = 0;
                if (bd_bool(bc, prob[2])) {
                    bmv.row = (int16_t)(read_mv_component(bc, d->p.mvc[0]) * 2 + best.row);
                    bmv.col = (int16_t)(read_mv_component(bc, d->p.mvc[1]) * 2 + best.col);
                }
            } else {
                bmv = amv;
            }
        } else {
            bmv = lmv;
        }
        const uint8_t *fill = &mbsplit_fill_offset[s][j * mbsplit_fill_count[s]];
        for (int n = 0; n < mbsplit_fill_count[s]; n++) mb->bmv[fill[n]] = bmv;
    }
    mb->partitioning = (uint8_t)s;
}

static void read_inter_modes(kl_vp8 *d, klv_bd *bc, klv_mb *mb, int r, int c,
                             const klv_edges *e) {
    enum { CNT_INTRA, CNT_NEAREST, CNT_NEAR, CNT_SPLITMV };
    mb->ref = (uint8_t)bd_bool(bc, d->prob_intra);
    if (!mb->ref) {
        mb->mv.row = mb->mv.col = 0;
        mb->ymode = (uint8_t)bd_tree(bc, t_ymode, d->p.ymode);
        if (mb->ymode == B_PRED) {
            mb->is4x4 = 1;
            for (int j = 0; j < 16; j++) mb->bmode[j] = (uint8_t)bd_tree(bc, t_bmode, p_bmode);
        }
        mb->uvmode = (uint8_t)bd_tree(bc, t_uvmode, d->p.uvmode);
        return;
    }
    if (bd_bool(bc, d->prob_last)) mb->ref = (uint8_t)(2 + bd_bool(bc, d->prob_gf));

    const klv_mb *above = mb_at(d, r - 1, c), *left = mb_at(d, r, c - 1),
                 *al = mb_at(d, r - 1, c - 1);
    int cnt[4] = { 0, 0, 0, 0 };
    klv_mv near[4];
    memset(near, 0, sizeof near);
    int ni = 0;                  // index of the last distinct MV in near[]
    int ci = 0;                  // cnt[] slot it counts into
    if (above->ref != INTRA_FRAME) {
        if (!mv_zero(above->mv)) {
            near[++ni] = above->mv;
            mv_bias(d, above->ref, mb->ref, &near[ni]);
            ++ci;
        }
        cnt[ci] += 2;
    }
    if (left->ref != INTRA_FRAME) {
        if (!mv_zero(left->mv)) {
            klv_mv t = left->mv;
            mv_bias(d, left->ref, mb->ref, &t);
            if (!mv_eq(t, near[ni])) { near[++ni] = t; ++ci; }
            cnt[ci] += 2;
        } else {
            cnt[CNT_INTRA] += 2;
        }
    }
    if (al->ref != INTRA_FRAME) {
        if (!mv_zero(al->mv)) {
            klv_mv t = al->mv;
            mv_bias(d, al->ref, mb->ref, &t);
            if (!mv_eq(t, near[ni])) { near[++ni] = t; ++ci; }
            cnt[ci] += 1;
        } else {
            cnt[CNT_INTRA] += 1;
        }
    }

    if (!bd_bool(bc, mode_contexts[cnt[CNT_INTRA]][0])) {
        mb->ymode = ZEROMV;
        mb->mv.row = mb->mv.col = 0;
        return;
    }
    cnt[CNT_NEAREST] += (cnt[CNT_SPLITMV] > 0) & mv_eq(near[ni], near[CNT_NEAREST]);
    if (cnt[CNT_NEAR] > cnt[CNT_NEAREST]) {
        int t = cnt[CNT_NEAREST]; cnt[CNT_NEAREST] = cnt[CNT_NEAR]; cnt[CNT_NEAR] = t;
        klv_mv m = near[CNT_NEAREST]; near[CNT_NEAREST] = near[CNT_NEAR]; near[CNT_NEAR] = m;
    }
    if (!bd_bool(bc, mode_contexts[cnt[CNT_NEAREST]][1])) {
        mb->ymode = NEARESTMV;
        mb->mv = near[CNT_NEAREST];
        clamp_mv2(&mb->mv, e);
        return;
    }
    if (!bd_bool(bc, mode_contexts[cnt[CNT_NEAR]][2])) {
        mb->ymode = NEARMV;
        mb->mv = near[CNT_NEAR];
        clamp_mv2(&mb->mv, e);
        return;
    }
    int bi = CNT_INTRA + (cnt[CNT_NEAREST] >= cnt[CNT_INTRA]);
    clamp_mv2(&near[bi], e);
    cnt[CNT_SPLITMV] = ((above->ymode == SPLITMV) + (left->ymode == SPLITMV)) * 2 +
                       (al->ymode == SPLITMV);
    if (bd_bool(bc, mode_contexts[cnt[CNT_SPLITMV]][3])) {
        read_split_mv(d, bc, mb, left, above, near[bi]);
        mb->mv = mb->bmv[15];
        mb->ymode = SPLITMV;
        mb->is4x4 = 1;
    } else {
        mb->mv.row = (int16_t)(read_mv_component(bc, d->p.mvc[0]) * 2 + near[bi].row);
        mb->mv.col = (int16_t)(read_mv_component(bc, d->p.mvc[1]) * 2 + near[bi].col);
        mb->ymode = NEWMV;
    }
}

static void read_modes(kl_vp8 *d, klv_bd *bc) {
    for (int r = 0; r < d->mbh; r++) {
        for (int c = 0; c < d->mbw; c++) {
            klv_mb *mb = mb_at(d, r, c);
            if (d->seg_update_map) {
                if (bd_bool(bc, d->seg_tree[0])) mb->segment = (uint8_t)(2 + bd_bool(bc, d->seg_tree[2]));
                else                             mb->segment = (uint8_t)bd_bool(bc, d->seg_tree[1]);
            } else if (d->key) {
                mb->segment = 0;
            }
            mb->skip = d->skip_enabled ? (uint8_t)bd_bool(bc, d->prob_skip) : 0;
            mb->is4x4 = 0;
            if (d->key) {
                mb->ref = INTRA_FRAME;
                mb->mv.row = mb->mv.col = 0;
                mb->ymode = (uint8_t)bd_tree(bc, t_kf_ymode, p_kf_ymode);
                if (mb->ymode == B_PRED) {
                    mb->is4x4 = 1;
                    for (int i = 0; i < 16; i++) {
                        int a = above_bmode(d, mb, i, r, c), l = left_bmode(d, mb, i, r, c);
                        mb->bmode[i] = (uint8_t)bd_tree(bc, t_bmode, klv_kf_bmode_probs[a][l]);
                    }
                }
                mb->uvmode = (uint8_t)bd_tree(bc, t_uvmode, p_kf_uvmode);
            } else {
                klv_edges e = { -((c * 16) << 3), ((d->mbw - 1 - c) * 16) << 3,
                                -((r * 16) << 3), ((d->mbh - 1 - r) * 16) << 3 };
                read_inter_modes(d, bc, mb, r, c, &e);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Residual tokens (libvpx detokenize.c).

static int get_coeffs(klv_bd *br, const uint8_t (*prob)[3][11], int ctx, int n, int16_t *out) {
    const uint8_t *p = prob[n][ctx];
    if (!bd_bool(br, p[0])) return 0;
    for (;;) {
        ++n;
        if (!bd_bool(br, p[1])) {
            p = prob[bands[n]][0];
        } else {
            int v;
            if (!bd_bool(br, p[2])) {
                p = prob[bands[n]][1];
                v = 1;
            } else {
                if (!bd_bool(br, p[3])) {
                    if (!bd_bool(br, p[4])) v = 2;
                    else v = 3 + bd_bool(br, p[5]);
                } else if (!bd_bool(br, p[6])) {
                    if (!bd_bool(br, p[7])) v = 5 + bd_bool(br, 159);
                    else { v = 7 + 2 * bd_bool(br, 165); v += bd_bool(br, 145); }
                } else {
                    int b1 = bd_bool(br, p[8]);
                    int b0 = bd_bool(br, p[9 + b1]);
                    int cat = 2 * b1 + b0;
                    v = 0;
                    for (const uint8_t *t = cat3456[cat]; *t; ++t) v += v + bd_bool(br, *t);
                    v += 3 + (8 << cat);
                }
                p = prob[bands[n]][2];
            }
            out[zigzag[n - 1]] = (int16_t)(bd_bool(br, 128) ? -v : v);
            if (n == 16 || !bd_bool(br, p[0])) return n;
        }
        if (n == 16) return 16;
    }
}

// Returns the eob total; eobs[25] and coeffs[25][16] are filled.
static int decode_tokens(kl_vp8 *d, klv_bd *bc, klv_mb *mb, uint8_t *actx, uint8_t *lctx,
                         int16_t coeffs[25][16], int eobs[25]) {
    int total = 0, skip_dc = 0;
    const uint8_t (*probs)[3][11];
    if (!mb->is4x4) {
        uint8_t *a = actx + 8, *l = lctx + 8;
        int nz = get_coeffs(bc, d->p.coef[1], *a + *l, 0, coeffs[24]);
        *a = *l = nz > 0;
        eobs[24] = nz;
        total += nz - 16;
        probs = d->p.coef[0];
        skip_dc = 1;
    } else {
        probs = d->p.coef[3];
    }
    for (int i = 0; i < 16; i++) {
        uint8_t *a = actx + (i & 3), *l = lctx + ((i & 12) >> 2);
        int nz = get_coeffs(bc, probs, *a + *l, skip_dc, coeffs[i]);
        *a = *l = nz > 0;
        nz += skip_dc;
        eobs[i] = nz;
        total += nz;
    }
    probs = d->p.coef[2];
    for (int i = 16; i < 24; i++) {
        uint8_t *a = actx + 4 + ((i > 19) << 1) + (i & 1);
        uint8_t *l = lctx + 4 + ((i > 19) << 1) + ((i & 3) > 1);
        int nz = get_coeffs(bc, probs, *a + *l, 0, coeffs[i]);
        *a = *l = nz > 0;
        eobs[i] = nz;
        total += nz;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Inverse transforms (libvpx idctllm.c), with libvpx's 16-bit intermediates.

static void idct_add(int16_t *in, uint8_t *dst, int stride) {
    static const int c1 = 20091, s1 = 35468;
    int16_t out[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
        int t1 = (in[4 + i] * s1) >> 16;
        int t2 = in[12 + i] + ((in[12 + i] * c1) >> 16);
        int cc = t1 - t2;
        t1 = in[4 + i] + ((in[4 + i] * c1) >> 16);
        t2 = (in[12 + i] * s1) >> 16;
        int dd = t1 + t2;
        out[i] = (int16_t)(a1 + dd);
        out[12 + i] = (int16_t)(a1 - dd);
        out[4 + i] = (int16_t)(b1 + cc);
        out[8 + i] = (int16_t)(b1 - cc);
    }
    for (int i = 0; i < 4; i++) {
        int16_t *ip = out + 4 * i;
        int a1 = ip[0] + ip[2], b1 = ip[0] - ip[2];
        int t1 = (ip[1] * s1) >> 16;
        int t2 = ip[3] + ((ip[3] * c1) >> 16);
        int cc = t1 - t2;
        t1 = ip[1] + ((ip[1] * c1) >> 16);
        t2 = (ip[3] * s1) >> 16;
        int dd = t1 + t2;
        int16_t o0 = (int16_t)((a1 + dd + 4) >> 3), o3 = (int16_t)((a1 - dd + 4) >> 3);
        int16_t o1 = (int16_t)((b1 + cc + 4) >> 3), o2 = (int16_t)((b1 - cc + 4) >> 3);
        uint8_t *row = dst + (size_t)i * stride;
        row[0] = clip8(row[0] + o0); row[1] = clip8(row[1] + o1);
        row[2] = clip8(row[2] + o2); row[3] = clip8(row[3] + o3);
    }
}

static void dc_add(int16_t dc, uint8_t *dst, int stride) {
    int a = (dc + 4) >> 3;
    for (int r = 0; r < 4; r++, dst += stride)
        for (int c = 0; c < 4; c++) dst[c] = clip8(dst[c] + a);
}

// Dequantize-and-add for one 4x4 block, the way vp8_dequant_idct_add_*_block does.
static void block_add(int16_t *q, const int16_t *dq, int eob, uint8_t *dst, int stride) {
    if (eob > 1) {
        for (int i = 0; i < 16; i++) q[i] = (int16_t)(q[i] * dq[i]);
        idct_add(q, dst, stride);
    } else {
        dc_add((int16_t)(q[0] * dq[0]), dst, stride);
    }
}

static void iwalsh(const int16_t *in, int16_t coeffs[25][16]) {
    int16_t out[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i];
        int c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
        out[i] = (int16_t)(a1 + b1);
        out[4 + i] = (int16_t)(c1 + d1);
        out[8 + i] = (int16_t)(a1 - b1);
        out[12 + i] = (int16_t)(d1 - c1);
    }
    for (int i = 0; i < 4; i++) {
        int16_t *ip = out + 4 * i;
        int a1 = ip[0] + ip[3], b1 = ip[1] + ip[2];
        int c1 = ip[1] - ip[2], d1 = ip[0] - ip[3];
        int a2 = a1 + b1, b2 = c1 + d1, c2 = a1 - b1, d2 = d1 - c1;
        ip[0] = (int16_t)((a2 + 3) >> 3); ip[1] = (int16_t)((b2 + 3) >> 3);
        ip[2] = (int16_t)((c2 + 3) >> 3); ip[3] = (int16_t)((d2 + 3) >> 3);
    }
    for (int i = 0; i < 16; i++) coeffs[i][0] = out[i];
}

// ---------------------------------------------------------------------------
// Intra prediction (vpx_dsp intrapred.c; the 4x4 set VP8 uses).

#define AVG2(a, b) (((a) + (b) + 1) >> 1)
#define AVG3(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)

static void pred_block(uint8_t *dst, int stride, int bs, int mode, const uint8_t *above,
                       const uint8_t *left, int have_left, int have_above) {
    int r, c;
    switch (mode) {
    case DC_PRED: {
        int sum = 0, n = 0, dc = 128;
        if (have_above) { for (c = 0; c < bs; c++) sum += above[c]; n += bs; }
        if (have_left)  { for (r = 0; r < bs; r++) sum += left[r];  n += bs; }
        if (n) dc = (sum + (n >> 1)) / n;
        for (r = 0; r < bs; r++) memset(dst + (size_t)r * stride, dc, bs);
        break;
    }
    case V_PRED:
        for (r = 0; r < bs; r++) memcpy(dst + (size_t)r * stride, above, bs);
        break;
    case H_PRED:
        for (r = 0; r < bs; r++) memset(dst + (size_t)r * stride, left[r], bs);
        break;
    default: /* TM_PRED */
        for (r = 0; r < bs; r++)
            for (c = 0; c < bs; c++)
                dst[(size_t)r * stride + c] = clip8(left[r] + above[c] - above[-1]);
        break;
    }
}

// above[-1..7] (top-left, above, above-right) and left[0..3].
static void pred4x4(uint8_t *dst, int stride, int mode, const uint8_t *above, const uint8_t *left) {
#define DST(x, y) dst[(x) + (size_t)(y) * stride]
    const int I = left[0], J = left[1], K = left[2], L = left[3], X = above[-1];
    const int A = above[0], B = above[1], C = above[2], D = above[3];
    const int E = above[4], F = above[5], G = above[6], H = above[7];
    int r, c;
    switch (mode) {
    case B_DC_PRED: {
        int v = (A + B + C + D + I + J + K + L + 4) >> 3;
        for (r = 0; r < 4; r++) memset(&DST(0, r), v, 4);
        break;
    }
    case B_TM_PRED:
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) DST(c, r) = clip8(left[r] + above[c] - X);
        break;
    case B_VE_PRED:
        DST(0, 0) = AVG3(X, A, B); DST(1, 0) = AVG3(A, B, C);
        DST(2, 0) = AVG3(B, C, D); DST(3, 0) = AVG3(C, D, E);
        for (r = 1; r < 4; r++) memcpy(&DST(0, r), &DST(0, 0), 4);
        break;
    case B_HE_PRED:
        memset(&DST(0, 0), AVG3(X, I, J), 4);
        memset(&DST(0, 1), AVG3(I, J, K), 4);
        memset(&DST(0, 2), AVG3(J, K, L), 4);
        memset(&DST(0, 3), AVG3(K, L, L), 4);
        break;
    case B_LD_PRED:      /* d45e */
        DST(0, 0) = AVG3(A, B, C);
        DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
        DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
        DST(3, 3) = AVG3(G, H, H);
        break;
    case B_RD_PRED:      /* d135 */
        DST(0, 3) = AVG3(J, K, L);
        DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(I, X, A);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(X, A, B);
        DST(3, 1) = DST(2, 0) = AVG3(A, B, C);
        DST(3, 0) = AVG3(B, C, D);
        break;
    case B_VR_PRED:      /* d117 */
        DST(0, 0) = DST(1, 2) = AVG2(X, A);
        DST(1, 0) = DST(2, 2) = AVG2(A, B);
        DST(2, 0) = DST(3, 2) = AVG2(B, C);
        DST(3, 0) = AVG2(C, D);
        DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
        DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
        DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
        DST(3, 1) = AVG3(B, C, D);
        DST(0, 2) = AVG3(X, I, J);
        DST(0, 3) = AVG3(I, J, K);
        break;
    case B_VL_PRED:      /* d63e */
        DST(0, 0) = AVG2(A, B);
        DST(1, 0) = DST(0, 2) = AVG2(B, C);
        DST(2, 0) = DST(1, 2) = AVG2(C, D);
        DST(3, 0) = DST(2, 2) = AVG2(D, E);
        DST(3, 2) = AVG3(E, F, G);
        DST(0, 1) = AVG3(A, B, C);
        DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
        DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
        DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
        DST(3, 3) = AVG3(F, G, H);
        break;
    case B_HD_PRED:      /* d153 */
        DST(0, 0) = DST(2, 1) = AVG2(I, X);
        DST(0, 1) = DST(2, 2) = AVG2(J, I);
        DST(0, 2) = DST(2, 3) = AVG2(K, J);
        DST(0, 3) = AVG2(L, K);
        DST(3, 0) = AVG3(A, B, C);
        DST(2, 0) = AVG3(X, A, B);
        DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
        DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
        DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
        DST(1, 3) = AVG3(L, K, J);
        break;
    default:             /* B_HU_PRED, d207 */
        DST(0, 0) = AVG2(I, J);
        DST(2, 0) = DST(0, 1) = AVG2(J, K);
        DST(2, 1) = DST(0, 2) = AVG2(K, L);
        DST(1, 0) = AVG3(I, J, K);
        DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
        DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = L;
        break;
    }
#undef DST
}

// ---------------------------------------------------------------------------
// Inter prediction (libvpx reconinter.c / filter.c).

typedef struct { const uint8_t *base; int stride, w, h; } klv_plane;

// The w x h block whose top-left full-pel position is (x, y), filtered by the
// subpel fraction (mx, my) in eighths. Reads outside the plane are clamped to
// its edge, which is what the replicated border holds.
static void predict(uint8_t *dst, int dstride, const klv_plane *pl, int x, int y,
                    int w, int h, int mx, int my, int bilinear) {
    uint8_t tmp[(16 + 5) * (16 + 5)];
    const uint8_t *src;
    int sstride;
    const int x0 = x - 2, y0 = y - 2, ww = w + 5, hh = h + 5;
    const int b = (pl->stride - pl->w) / 2;              // this plane's border
    if (x0 >= -b && y0 >= -b && x0 + ww <= pl->w + b && y0 + hh <= pl->h + b) {
        src = pl->base + (ptrdiff_t)y * pl->stride + x;
        sstride = pl->stride;
    } else {
        for (int r = 0; r < hh; r++) {
            int sy = clampi(y0 + r, 0, pl->h - 1);
            for (int c = 0; c < ww; c++)
                tmp[r * ww + c] = pl->base[(ptrdiff_t)sy * pl->stride + clampi(x0 + c, 0, pl->w - 1)];
        }
        src = tmp + 2 * ww + 2;
        sstride = ww;
    }
    if (!(mx | my)) {
        for (int r = 0; r < h; r++) memcpy(dst + (size_t)r * dstride, src + (ptrdiff_t)r * sstride, w);
        return;
    }
    if (!bilinear) {
        int fd[21 * 16];
        const short *hf = subpel6[mx], *vf = subpel6[my];
        for (int r = 0; r < h + 5; r++) {
            const uint8_t *s = src + (ptrdiff_t)(r - 2) * sstride;
            for (int c = 0; c < w; c++) {
                int t = s[c - 2] * hf[0] + s[c - 1] * hf[1] + s[c] * hf[2] +
                        s[c + 1] * hf[3] + s[c + 2] * hf[4] + s[c + 3] * hf[5] + 64;
                fd[r * w + c] = clip8(t >> 7);
            }
        }
        for (int r = 0; r < h; r++) {
            const int *s = fd + (r + 2) * w;
            for (int c = 0; c < w; c++) {
                int t = s[c - 2 * w] * vf[0] + s[c - w] * vf[1] + s[c] * vf[2] +
                        s[c + w] * vf[3] + s[c + 2 * w] * vf[4] + s[c + 3 * w] * vf[5] + 64;
                dst[(size_t)r * dstride + c] = clip8(t >> 7);
            }
        }
    } else {
        unsigned short fd[17 * 16];
        const short *hf = bilin[mx], *vf = bilin[my];
        for (int r = 0; r < h + 1; r++) {
            const uint8_t *s = src + (ptrdiff_t)r * sstride;
            for (int c = 0; c < w; c++)
                fd[r * w + c] = (unsigned short)((s[c] * hf[0] + s[c + 1] * hf[1] + 64) >> 7);
        }
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++)
                dst[(size_t)r * dstride + c] =
                    (uint8_t)((fd[r * w + c] * vf[0] + fd[(r + 1) * w + c] * vf[1] + 64) >> 7);
    }
}

static void predict_mv(uint8_t *dst, int dstride, const klv_plane *pl, int x, int y,
                       int w, int h, klv_mv mv, int bilinear) {
    predict(dst, dstride, pl, x + (mv.col >> 3), y + (mv.row >> 3), w, h,
            mv.col & 7, mv.row & 7, bilinear);
}

static void inter_predict(kl_vp8 *d, klv_mb *mb, const klv_fb *ref, int r, int c,
                          uint8_t *dy, uint8_t *du, uint8_t *dv) {
    int bil = d->version != 0, full = d->version == 3;
    klv_plane py = { ref->y, d->ys, d->aw, d->ah };
    klv_plane pu = { ref->u, d->uvs, d->aw / 2, d->ah / 2 };
    klv_plane pv = { ref->v, d->uvs, d->aw / 2, d->ah / 2 };
    int x = c * 16, y = r * 16;
    if (mb->ymode != SPLITMV) {
        klv_mv mv = mb->mv;
        predict_mv(dy, d->ys, &py, x, y, 16, 16, mv, bil);
        // Chroma: luma MV halved, rounded away from zero (libvpx).
        int mr = mv.row, mc = mv.col;
        mr += 1 | (mr >> 31); mc += 1 | (mc >> 31);
        mr /= 2; mc /= 2;
        if (full) { mr &= ~7; mc &= ~7; }
        klv_mv cmv = { (int16_t)mr, (int16_t)mc };
        predict_mv(du, d->uvs, &pu, x / 2, y / 2, 8, 8, cmv, bil);
        predict_mv(dv, d->uvs, &pv, x / 2, y / 2, 8, 8, cmv, bil);
        return;
    }
    for (int b = 0; b < 16; b++)
        predict_mv(dy + (size_t)(b >> 2) * 4 * d->ys + (b & 3) * 4, d->ys, &py,
                   x + (b & 3) * 4, y + (b >> 2) * 4, 4, 4, mb->bmv[b], bil);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int yo = i * 8 + j * 2;
            int tr = mb->bmv[yo].row + mb->bmv[yo + 1].row + mb->bmv[yo + 4].row + mb->bmv[yo + 5].row;
            int tc = mb->bmv[yo].col + mb->bmv[yo + 1].col + mb->bmv[yo + 4].col + mb->bmv[yo + 5].col;
            tr += 4 + ((tr >> 31) * 8);
            tc += 4 + ((tc >> 31) * 8);
            int mr = tr / 8, mc = tc / 8;
            if (full) { mr &= ~7; mc &= ~7; }
            klv_mv cmv = { (int16_t)mr, (int16_t)mc };
            size_t off = (size_t)i * 4 * d->uvs + j * 4;
            predict_mv(du + off, d->uvs, &pu, x / 2 + j * 4, y / 2 + i * 4, 4, 4, cmv, bil);
            predict_mv(dv + off, d->uvs, &pv, x / 2 + j * 4, y / 2 + i * 4, 4, 4, cmv, bil);
        }
    }
}

// ---------------------------------------------------------------------------
// Loop filter (libvpx loopfilter_filters.c, vp8_loopfilter.c).

static inline int8_t sclamp(int t) { return (int8_t)(t < -128 ? -128 : t > 127 ? 127 : t); }

static inline int lf_mask(int limit, int blimit, int p3, int p2, int p1, int p0,
                          int q0, int q1, int q2, int q3) {
    return !(abs(p3 - p2) > limit || abs(p2 - p1) > limit || abs(p1 - p0) > limit ||
             abs(q1 - q0) > limit || abs(q2 - q1) > limit || abs(q3 - q2) > limit ||
             abs(p0 - q0) * 2 + abs(p1 - q1) / 2 > blimit);
}

static void lf_edge(uint8_t *s, int step, int pitch, int n, int blimit, int limit,
                    int thresh, int mb) {
    for (int i = 0; i < n; i++, s += pitch) {
        int p3 = s[-4 * step], p2 = s[-3 * step], p1 = s[-2 * step], p0 = s[-step];
        int q0 = s[0], q1 = s[step], q2 = s[2 * step], q3 = s[3 * step];
        if (!lf_mask(limit, blimit, p3, p2, p1, p0, q0, q1, q2, q3)) continue;
        int hev = abs(p1 - p0) > thresh || abs(q1 - q0) > thresh;
        int8_t ps2 = (int8_t)(p2 ^ 0x80), ps1 = (int8_t)(p1 ^ 0x80), ps0 = (int8_t)(p0 ^ 0x80);
        int8_t qs0 = (int8_t)(q0 ^ 0x80), qs1 = (int8_t)(q1 ^ 0x80), qs2 = (int8_t)(q2 ^ 0x80);
        if (!mb) {
            int8_t f = hev ? sclamp(ps1 - qs1) : 0;
            f = sclamp(f + 3 * (qs0 - ps0));
            int8_t f1 = (int8_t)(sclamp(f + 4) >> 3), f2 = (int8_t)(sclamp(f + 3) >> 3);
            s[0] = (uint8_t)(sclamp(qs0 - f1) ^ 0x80);
            s[-step] = (uint8_t)(sclamp(ps0 + f2) ^ 0x80);
            if (!hev) {
                int8_t a = (int8_t)((f1 + 1) >> 1);
                s[step] = (uint8_t)(sclamp(qs1 - a) ^ 0x80);
                s[-2 * step] = (uint8_t)(sclamp(ps1 + a) ^ 0x80);
            }
        } else {
            int8_t f = sclamp(ps1 - qs1);
            f = sclamp(f + 3 * (qs0 - ps0));
            if (hev) {
                int8_t f1 = (int8_t)(sclamp(f + 4) >> 3), f2 = (int8_t)(sclamp(f + 3) >> 3);
                s[0] = (uint8_t)(sclamp(qs0 - f1) ^ 0x80);
                s[-step] = (uint8_t)(sclamp(ps0 + f2) ^ 0x80);
            } else {
                int8_t u = sclamp((63 + f * 27) >> 7);
                s[0] = (uint8_t)(sclamp(qs0 - u) ^ 0x80);
                s[-step] = (uint8_t)(sclamp(ps0 + u) ^ 0x80);
                u = sclamp((63 + f * 18) >> 7);
                s[step] = (uint8_t)(sclamp(qs1 - u) ^ 0x80);
                s[-2 * step] = (uint8_t)(sclamp(ps1 + u) ^ 0x80);
                u = sclamp((63 + f * 9) >> 7);
                s[2 * step] = (uint8_t)(sclamp(qs2 - u) ^ 0x80);
                s[-3 * step] = (uint8_t)(sclamp(ps2 + u) ^ 0x80);
            }
        }
    }
}

static void lf_simple(uint8_t *s, int step, int pitch, int blimit) {
    for (int i = 0; i < 16; i++, s += pitch) {
        int p1 = s[-2 * step], p0 = s[-step], q0 = s[0], q1 = s[step];
        if (abs(p0 - q0) * 2 + abs(p1 - q1) / 2 > blimit) continue;
        int8_t ps1 = (int8_t)(p1 ^ 0x80), ps0 = (int8_t)(p0 ^ 0x80);
        int8_t qs0 = (int8_t)(q0 ^ 0x80), qs1 = (int8_t)(q1 ^ 0x80);
        int8_t f = sclamp(ps1 - qs1);
        f = sclamp(f + 3 * (qs0 - ps0));
        int8_t f1 = (int8_t)(sclamp(f + 4) >> 3);
        s[0] = (uint8_t)(sclamp(qs0 - f1) ^ 0x80);
        int8_t f2 = (int8_t)(sclamp(f + 3) >> 3);
        s[-step] = (uint8_t)(sclamp(ps0 + f2) ^ 0x80);
    }
}

static void loop_filter(kl_vp8 *d, klv_fb *f) {
    static const int mode_lut[10] = { 1, 1, 1, 1, 0, 2, 2, 1, 2, 3 };
    int lvl[4][4][4];
    for (int seg = 0; seg < 4; seg++) {
        int ls = d->filter_level;
        if (d->seg_enabled) {
            ls = d->seg_abs ? d->seg_lf[seg] : ls + d->seg_lf[seg];
            ls = clampi(ls, 0, 63);
        }
        if (!d->lf_delta_on) {
            for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) lvl[seg][a][b] = ls;
            continue;
        }
        int lr = ls + d->ref_lf_delta[INTRA_FRAME];
        lvl[seg][INTRA_FRAME][0] = clampi(lr + d->mode_lf_delta[0], 0, 63);
        lvl[seg][INTRA_FRAME][1] = clampi(lr, 0, 63);
        for (int ref = 1; ref < 4; ref++) {
            lr = ls + d->ref_lf_delta[ref];
            for (int m = 1; m < 4; m++) lvl[seg][ref][m] = clampi(lr + d->mode_lf_delta[m], 0, 63);
        }
    }
    int sh = d->sharpness;
    for (int r = 0; r < d->mbh; r++) {
        for (int c = 0; c < d->mbw; c++) {
            klv_mb *mb = mb_at(d, r, c);
            int level = lvl[mb->segment & 3][mb->ref][mode_lut[mb->ymode]];
            if (!level) continue;
            int skip = mb->ymode != B_PRED && mb->ymode != SPLITMV && mb->skip;
            int interior = level >> (sh > 0);
            interior >>= (sh > 4);
            if (sh > 0 && interior > 9 - sh) interior = 9 - sh;
            if (interior < 1) interior = 1;
            int blim = 2 * level + interior, mblim = 2 * (level + 2) + interior;
            int hev = 0;
            if (level >= 40) hev = d->key ? 2 : 3;
            else if (level >= 20) hev = d->key ? 1 : 2;
            else if (level >= 15) hev = 1;
            uint8_t *y = f->y + (size_t)r * 16 * d->ys + c * 16;
            uint8_t *u = f->u + (size_t)r * 8 * d->uvs + c * 8;
            uint8_t *v = f->v + (size_t)r * 8 * d->uvs + c * 8;
            int ys = d->ys, uvs = d->uvs;
            if (d->filter_type == 0) {
                if (c > 0) {
                    lf_edge(y, 1, ys, 16, mblim, interior, hev, 1);
                    lf_edge(u, 1, uvs, 8, mblim, interior, hev, 1);
                    lf_edge(v, 1, uvs, 8, mblim, interior, hev, 1);
                }
                if (!skip) {
                    for (int k = 4; k < 16; k += 4) lf_edge(y + k, 1, ys, 16, blim, interior, hev, 0);
                    lf_edge(u + 4, 1, uvs, 8, blim, interior, hev, 0);
                    lf_edge(v + 4, 1, uvs, 8, blim, interior, hev, 0);
                }
                if (r > 0) {
                    lf_edge(y, ys, 1, 16, mblim, interior, hev, 1);
                    lf_edge(u, uvs, 1, 8, mblim, interior, hev, 1);
                    lf_edge(v, uvs, 1, 8, mblim, interior, hev, 1);
                }
                if (!skip) {
                    for (int k = 4; k < 16; k += 4)
                        lf_edge(y + (size_t)k * ys, ys, 1, 16, blim, interior, hev, 0);
                    lf_edge(u + (size_t)4 * uvs, uvs, 1, 8, blim, interior, hev, 0);
                    lf_edge(v + (size_t)4 * uvs, uvs, 1, 8, blim, interior, hev, 0);
                }
            } else {
                if (c > 0) lf_simple(y, 1, ys, mblim);
                if (!skip) for (int k = 4; k < 16; k += 4) lf_simple(y + k, 1, ys, blim);
                if (r > 0) lf_simple(y, ys, 1, mblim);
                if (!skip) for (int k = 4; k < 16; k += 4) lf_simple(y + (size_t)k * ys, ys, 1, blim);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Macroblock reconstruction (libvpx decodeframe.c decode_macroblock).

static void dequant_factors(const kl_vp8 *d, int seg, int16_t y1[16], int16_t y2[16],
                            int16_t uv[16]) {
    int q = d->qindex;
    if (d->seg_enabled) q = clampi(d->seg_abs ? d->seg_q[seg] : q + d->seg_q[seg], 0, 127);
    int y1dc = dc_q[clampi(q + d->dq_y1dc, 0, 127)];
    int y1ac = ac_q[clampi(q, 0, 127)];
    int y2dc = dc_q[clampi(q + d->dq_y2dc, 0, 127)] * 2;
    int y2ac = (ac_q[clampi(q + d->dq_y2ac, 0, 127)] * 101581) >> 16;
    if (y2ac < 8) y2ac = 8;
    int uvdc = dc_q[clampi(q + d->dq_uvdc, 0, 127)];
    if (uvdc > 132) uvdc = 132;
    int uvac = ac_q[clampi(q + d->dq_uvac, 0, 127)];
    y1[0] = (int16_t)y1dc; y2[0] = (int16_t)y2dc; uv[0] = (int16_t)uvdc;
    for (int i = 1; i < 16; i++) { y1[i] = (int16_t)y1ac; y2[i] = (int16_t)y2ac; uv[i] = (int16_t)uvac; }
}

static void decode_mb(kl_vp8 *d, klv_bd *bc, klv_mb *mb, int r, int c, klv_fb *cur,
                      uint8_t *actx, uint8_t *lctx) {
    int16_t coeffs[25][16];
    int eobs[25];
    memset(coeffs, 0, sizeof coeffs);
    memset(eobs, 0, sizeof eobs);
    if (mb->skip) {
        memset(actx, 0, 8); memset(lctx, 0, 8);
        if (!mb->is4x4) actx[8] = lctx[8] = 0;
    } else {
        int total = decode_tokens(d, bc, mb, actx, lctx, coeffs, eobs);
        mb->skip = total == 0;
    }
    int16_t dq_y1[16], dq_y2[16], dq_uv[16];
    dequant_factors(d, mb->segment & 3, dq_y1, dq_y2, dq_uv);

    const int ys = d->ys, uvs = d->uvs;
    uint8_t *dy = cur->y + (size_t)r * 16 * ys + c * 16;
    uint8_t *du = cur->u + (size_t)r * 8 * uvs + c * 8;
    uint8_t *dv = cur->v + (size_t)r * 8 * uvs + c * 8;

    if (mb->ref == INTRA_FRAME) {
        int hl = c > 0, ha = r > 0;
        uint8_t left[16];
        for (int i = 0; i < 8; i++) left[i] = du[(ptrdiff_t)i * uvs - 1];
        pred_block(du, uvs, 8, mb->uvmode, du - uvs, left, hl, ha);
        for (int i = 0; i < 8; i++) left[i] = dv[(ptrdiff_t)i * uvs - 1];
        pred_block(dv, uvs, 8, mb->uvmode, dv - uvs, left, hl, ha);
        if (mb->ymode != B_PRED) {
            for (int i = 0; i < 16; i++) left[i] = dy[(ptrdiff_t)i * ys - 1];
            pred_block(dy, ys, 16, mb->ymode, dy - ys, left, hl, ha);
        } else {
            if (mb->skip) memset(eobs, 0, sizeof eobs);
            // The macroblock's above-right, for the right column of sub-blocks
            // below the first row (libvpx intra_prediction_down_copy).
            const uint8_t *ar = dy - ys + 16;
            for (int i = 0; i < 16; i++) {
                int bx = i & 3, by = i >> 2;
                uint8_t *b = dy + (size_t)by * 4 * ys + bx * 4;
                uint8_t above[12];
                memcpy(above + 1, b - ys, 4);
                above[0] = b[-ys - 1];
                if (bx == 3 && by > 0) memcpy(above + 5, ar, 4);
                else memcpy(above + 5, b - ys + 4, 4);
                uint8_t l4[4] = { b[-1], b[ys - 1], b[2 * ys - 1], b[3 * ys - 1] };
                pred4x4(b, ys, mb->bmode[i], above + 1, l4);
                if (eobs[i]) block_add(coeffs[i], dq_y1, eobs[i], b, ys);
            }
        }
    } else {
        const klv_fb *ref = &d->fb[mb->ref == LAST_FRAME ? d->last
                                    : mb->ref == GOLDEN_FRAME ? d->golden : d->altref];
        inter_predict(d, mb, ref, r, c, dy, du, dv);
    }

    if (mb->skip) return;
    if (mb->ymode != B_PRED) {
        const int16_t *dq = dq_y1;
        int16_t dq_dc[16];
        if (mb->ymode != SPLITMV) {
            int16_t *y2 = coeffs[24];
            if (eobs[24] > 1) {
                for (int i = 0; i < 16; i++) y2[i] = (int16_t)(y2[i] * dq_y2[i]);
                iwalsh(y2, coeffs);
            } else {
                int16_t dc = (int16_t)(y2[0] * dq_y2[0]);
                int16_t v = (int16_t)((dc + 3) >> 3);
                for (int i = 0; i < 16; i++) coeffs[i][0] = v;
            }
            memcpy(dq_dc, dq_y1, sizeof dq_dc);
            dq_dc[0] = 1;
            dq = dq_dc;
        }
        for (int i = 0; i < 16; i++)
            block_add(coeffs[i], dq, eobs[i], dy + (size_t)(i >> 2) * 4 * ys + (i & 3) * 4, ys);
    }
    for (int i = 16; i < 24; i++) {
        uint8_t *base = i < 20 ? du : dv;
        int k = (i - 16) & 3;
        block_add(coeffs[i], dq_uv, eobs[i], base + (size_t)(k >> 1) * 4 * uvs + (k & 1) * 4, uvs);
    }
}

// ---------------------------------------------------------------------------
// Frame.

static int klv_free_fb(kl_vp8 *d) {
    for (int i = 0; i < KLV_NFB; i++) if (!d->fb[i].refs) return i;
    return -1;
}

static void klv_ref(kl_vp8 *d, int *slot, int to) {
    if (d->fb[*slot].refs > 0) d->fb[*slot].refs--;
    *slot = to;
    d->fb[to].refs++;
}

int kl_vp8_decode(kl_vp8 *d, const uint8_t *data, size_t len) {
    if (!d || !data || len < 3) return -1;
    int key = !(data[0] & 1);
    int version = (data[0] >> 1) & 7;
    int show = (data[0] >> 4) & 1;
    size_t first = (size_t)((data[0] | (data[1] << 8) | (data[2] << 16)) >> 5);
    data += 3; len -= 3;
    if (key) {
        if (len < 7 || data[0] != 0x9d || data[1] != 0x01 || data[2] != 0x2a) return -1;
        int w = (data[3] | (data[4] << 8)) & 0x3fff;
        int h = (data[5] | (data[6] << 8)) & 0x3fff;
        data += 7; len -= 7;
        if (!w || !h) return -1;
        if (w != d->w || h != d->h || !d->mbs) {
            if (!klv_alloc(d, w, h)) { klv_free_buffers(d); d->w = d->h = 0; return -1; }
        }
        d->have_key = 1;
    } else if (!d->have_key) {
        return -1;
    }
    if (first > len) return -1;
    d->key = key;
    d->version = version;

    int refresh_golden = 0, refresh_alt = 0, copy_gf = 0, copy_arf = 0;
    if (key) {
        klv_reset_probs(d);
        memset(d->seg_q, 0, sizeof d->seg_q); memset(d->seg_lf, 0, sizeof d->seg_lf);
        d->seg_abs = 0;
        memset(d->ref_lf_delta, 0, sizeof d->ref_lf_delta);
        memset(d->mode_lf_delta, 0, sizeof d->mode_lf_delta);
        refresh_golden = refresh_alt = 1;
        d->sign_bias[GOLDEN_FRAME] = d->sign_bias[ALTREF_FRAME] = 0;
    }

    klv_bd bc;
    bd_init(&bc, data, first);
    if (key) { (void)bd_bool(&bc, 128); (void)bd_bool(&bc, 128); }   // color space, clamping
    d->seg_enabled = bd_bool(&bc, 128);
    d->seg_update_map = 0;
    if (d->seg_enabled) {
        d->seg_update_map = bd_bool(&bc, 128);
        int update_data = bd_bool(&bc, 128);
        if (update_data) {
            d->seg_abs = bd_bool(&bc, 128);
            for (int i = 0; i < 4; i++) {
                int v = 0;
                if (bd_bool(&bc, 128)) { v = bd_lit(&bc, 7); if (bd_bool(&bc, 128)) v = -v; }
                d->seg_q[i] = (int8_t)v;
            }
            for (int i = 0; i < 4; i++) {
                int v = 0;
                if (bd_bool(&bc, 128)) { v = bd_lit(&bc, 6); if (bd_bool(&bc, 128)) v = -v; }
                d->seg_lf[i] = (int8_t)v;
            }
        }
        if (d->seg_update_map)
            for (int i = 0; i < 3; i++) d->seg_tree[i] = (uint8_t)(bd_bool(&bc, 128) ? bd_lit(&bc, 8) : 255);
    }
    d->filter_type = bd_bool(&bc, 128);
    d->filter_level = bd_lit(&bc, 6);
    d->sharpness = bd_lit(&bc, 3);
    d->lf_delta_on = bd_bool(&bc, 128);
    if (d->lf_delta_on && bd_bool(&bc, 128)) {
        for (int i = 0; i < 4; i++)
            if (bd_bool(&bc, 128)) {
                int v = bd_lit(&bc, 6);
                d->ref_lf_delta[i] = (int8_t)(bd_bool(&bc, 128) ? -v : v);
            }
        for (int i = 0; i < 4; i++)
            if (bd_bool(&bc, 128)) {
                int v = bd_lit(&bc, 6);
                d->mode_lf_delta[i] = (int8_t)(bd_bool(&bc, 128) ? -v : v);
            }
    }

    // Token partitions follow the first partition: 3-byte sizes, then data.
    d->nparts = 1 << bd_lit(&bc, 2);
    {
        const uint8_t *sizes = data + first, *end = data + len;
        const uint8_t *p = sizes + 3 * (d->nparts - 1);
        if (p > end) return -1;
        for (int i = 0; i < d->nparts; i++) {
            size_t n = (size_t)(end - p);
            if (i < d->nparts - 1) {
                size_t s = (size_t)(sizes[3 * i] | (sizes[3 * i + 1] << 8) | (sizes[3 * i + 2] << 16));
                if (s < n) n = s;
            }
            bd_init(&d->part[i], p, n);
            p += n;
        }
    }

    d->qindex = bd_lit(&bc, 7);
    d->dq_y1dc = delta_q(&bc);
    d->dq_y2dc = delta_q(&bc);
    d->dq_y2ac = delta_q(&bc);
    d->dq_uvdc = delta_q(&bc);
    d->dq_uvac = delta_q(&bc);

    if (!key) {
        refresh_golden = bd_bool(&bc, 128);
        refresh_alt = bd_bool(&bc, 128);
        if (!refresh_golden) copy_gf = bd_lit(&bc, 2);
        if (!refresh_alt) copy_arf = bd_lit(&bc, 2);
        d->sign_bias[GOLDEN_FRAME] = bd_bool(&bc, 128);
        d->sign_bias[ALTREF_FRAME] = bd_bool(&bc, 128);
    }
    int refresh_probs = bd_bool(&bc, 128);
    if (!refresh_probs) d->saved = d->p;
    int refresh_last = key || bd_bool(&bc, 128);

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 11; l++)
                    if (bd_bool(&bc, klv_coef_update_probs[i][j][k][l]))
                        d->p.coef[i][j][k][l] = (uint8_t)bd_lit(&bc, 8);

    d->skip_enabled = bd_bool(&bc, 128);
    d->prob_skip = d->skip_enabled ? bd_lit(&bc, 8) : 0;
    if (!key) {
        d->prob_intra = bd_lit(&bc, 8);
        d->prob_last = bd_lit(&bc, 8);
        d->prob_gf = bd_lit(&bc, 8);
        if (bd_bool(&bc, 128)) for (int i = 0; i < 4; i++) d->p.ymode[i] = (uint8_t)bd_lit(&bc, 8);
        if (bd_bool(&bc, 128)) for (int i = 0; i < 3; i++) d->p.uvmode[i] = (uint8_t)bd_lit(&bc, 8);
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < 19; k++)
                if (bd_bool(&bc, mv_update[i][k])) {
                    int x = bd_lit(&bc, 7);
                    d->p.mvc[i][k] = (uint8_t)(x ? x << 1 : 1);
                }
    }

    read_modes(d, &bc);

    // Reconstruct into a free buffer.
    int nf = klv_free_fb(d);
    if (nf < 0) return -1;
    klv_fb *cur = &d->fb[nf];
    const int ys = d->ys, uvs = d->uvs;
    memset(cur->y - 1 - ys, 127, d->aw + 5);
    memset(cur->u - 1 - uvs, 127, d->aw / 2 + 5);
    memset(cur->v - 1 - uvs, 127, d->aw / 2 + 5);
    memset(d->above_ctx, 0, (size_t)d->mbw * 9);
    for (int r = 0; r < d->mbh; r++) {
        klv_bd *tb = &d->part[r & (d->nparts - 1)];
        uint8_t lctx[9] = { 0 };
        for (int i = 0; i < 16; i++) cur->y[(size_t)(r * 16 + i) * ys - 1] = 129;
        for (int i = 0; i < 8; i++) {
            cur->u[(size_t)(r * 8 + i) * uvs - 1] = 129;
            cur->v[(size_t)(r * 8 + i) * uvs - 1] = 129;
        }
        for (int c = 0; c < d->mbw; c++)
            decode_mb(d, tb, mb_at(d, r, c), r, c, cur, d->above_ctx + 9 * c, lctx);
        // Above-right for the next row's last macroblock (vp8_extend_mb_row).
        uint8_t *ye = cur->y + (size_t)(r * 16 + 14) * ys + d->aw;
        uint8_t *ue = cur->u + (size_t)(r * 8 + 6) * uvs + d->aw / 2;
        uint8_t *ve = cur->v + (size_t)(r * 8 + 6) * uvs + d->aw / 2;
        for (int k = 0; k < 2; k++) {
            memset(ye + (size_t)k * ys, ye[(size_t)k * ys - 1], 4);
            memset(ue + (size_t)k * uvs, ue[(size_t)k * uvs - 1], 4);
            memset(ve + (size_t)k * uvs, ve[(size_t)k * uvs - 1], 4);
        }
    }
    if (d->filter_level) loop_filter(d, cur);
    klv_extend(cur->y, ys, d->aw, d->ah, KLV_BORDER);
    klv_extend(cur->u, uvs, d->aw / 2, d->ah / 2, KLV_BORDER / 2);
    klv_extend(cur->v, uvs, d->aw / 2, d->ah / 2, KLV_BORDER / 2);

    // Reference updates, in libvpx's order (swap_frame_buffers).
    if (copy_arf == 1) klv_ref(d, &d->altref, d->last);
    else if (copy_arf == 2) klv_ref(d, &d->altref, d->golden);
    if (copy_gf == 1) klv_ref(d, &d->golden, d->last);
    else if (copy_gf == 2) klv_ref(d, &d->golden, d->altref);
    if (refresh_golden) klv_ref(d, &d->golden, nf);
    if (refresh_alt) klv_ref(d, &d->altref, nf);
    if (refresh_last) klv_ref(d, &d->last, nf);

    if (!refresh_probs) d->p = d->saved;
    if (!show) return 0;
    d->shown = nf;
    return 1;
}

void kl_vp8_frame(const kl_vp8 *d, const uint8_t **y, const uint8_t **u, const uint8_t **v,
                  int *y_stride, int *uv_stride, int *width, int *height) {
    const klv_fb *f = &d->fb[d->shown];
    if (y) *y = f->y;
    if (u) *u = f->u;
    if (v) *v = f->v;
    if (y_stride) *y_stride = d->ys;
    if (uv_stride) *uv_stride = d->uvs;
    if (width) *width = d->w;
    if (height) *height = d->h;
}

kl_vp8 *kl_vp8_new(void) { return calloc(1, sizeof(kl_vp8)); }

void kl_vp8_free(kl_vp8 *d) {
    if (!d) return;
    klv_free_buffers(d);
    free(d);
}
