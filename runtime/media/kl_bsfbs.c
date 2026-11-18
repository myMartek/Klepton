// See kl_bsfbs.h.
#include "kl_bsfbs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int kl_bsinfo_encode(int request_id, long long duration_ms, int media_w,
                     int media_h, int tex_w, int tex_h, int tex_id,
                     unsigned char **out, size_t *len) {
    if (!out || !len) return -1;
    *out = NULL;
    *len = 0;
    // Table: soffset + 9 int32 slots = 40 bytes. Vtable: vlen + tlen + 9
    // voffsets = 22 bytes. Buffer: root(4) + vtable(22) + pad(2) + table(40).
    static const uint16_t voff[9] = { 4, 0, 0, 16, 20, 24, 28, 32, 36 };
    int32_t vals[9];
    vals[0] = (int32_t)request_id;
    vals[1] = vals[2] = 0;
    vals[3] = (int32_t)duration_ms;
    vals[4] = (int32_t)media_w;
    vals[5] = (int32_t)media_h;
    vals[6] = (int32_t)tex_w;
    vals[7] = (int32_t)tex_h;
    vals[8] = (int32_t)tex_id;
    unsigned char *b = calloc(1, 68);
    if (!b) return -1;
    uint32_t root = 28;
    memcpy(b, &root, 4);
    uint16_t vlen = 22, tlen = 40;
    memcpy(b + 4, &vlen, 2);
    memcpy(b + 6, &tlen, 2);
    memcpy(b + 8, voff, sizeof voff);
    // FlatBuffers stores table_pos - vtable_pos: the vtable lies before the
    // table, so this is positive (28 - 4). The guest's Table.__offset does
    // vtable = table - soffset; a negative value here lands past the buffer.
    int32_t so = 28 - 4;   // vtable at 4, table at 28
    memcpy(b + 28, &so, 4);
    for (int i = 0; i < 9; i++) memcpy(b + 32 + i * 4, &vals[i], 4);
    *out = b;
    *len = 68;
    return 0;
}

int kl_bsinfo_scale_bgra(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return -1;
    if (sw == dw && sh == dh) {
        memcpy(dst, src, (size_t)sw * (size_t)sh * 4);
        return 0;
    }
    for (int y = 0; y < dh; y++) {
        // Pixel-centre mapping, clamped at the edges: dst(y) samples
        // (y + 0.5) * sh / dh - 0.5, so corners keep the source corners.
        double sy = ((double)y + 0.5) * sh / dh - 0.5;
        if (sy < 0) sy = 0;
        if (sy > sh - 1) sy = sh - 1;
        int y0 = (int)sy;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        double wy = sy - y0;
        for (int x = 0; x < dw; x++) {
            double sx = ((double)x + 0.5) * sw / dw - 0.5;
            if (sx < 0) sx = 0;
            if (sx > sw - 1) sx = sw - 1;
            int x0 = (int)sx;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            double wx = sx - x0;
            const unsigned char *p00 = src + ((size_t)y0 * sw + x0) * 4;
            const unsigned char *p10 = src + ((size_t)y0 * sw + x1) * 4;
            const unsigned char *p01 = src + ((size_t)y1 * sw + x0) * 4;
            const unsigned char *p11 = src + ((size_t)y1 * sw + x1) * 4;
            unsigned char *d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++) {
                double v = p00[c] * (1 - wx) * (1 - wy) +
                           p10[c] * wx * (1 - wy) +
                           p01[c] * (1 - wx) * wy +
                           p11[c] * wx * wy;
                int iv = (int)(v + 0.5);
                d[c] = (unsigned char)(iv > 255 ? 255 : iv);
            }
        }
    }
    return 0;
}

char *kl_bsinfo_base64(const unsigned char *b, size_t n) {
    if (!b && n) return NULL;
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t m = (n + 2) / 3 * 4;
    char *s = malloc(m + 1);
    if (!s) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16;
        if (i + 1 < n) v |= (uint32_t)b[i + 1] << 8;
        if (i + 2 < n) v |= b[i + 2];
        s[j++] = tab[(v >> 18) & 63];
        s[j++] = tab[(v >> 12) & 63];
        s[j++] = i + 1 < n ? tab[(v >> 6) & 63] : '=';
        s[j++] = i + 2 < n ? tab[v & 63] : '=';
    }
    s[j] = '\0';
    return s;
}

int kl_bsinfo_test_pattern(unsigned char *bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return -1;
    int wd = w - 1 ? w - 1 : 1;
    int hd = h - 1 ? h - 1 : 1;
    for (int y = 0; y < h; y++) {
        // The ruler sits at 15, 31, … so row 0 stays the ramp's origin and the
        // top-left texel is the unambiguous (u=0, v=0) reference.
        int ruler = (y % 16) == 15;
        // The ramp reaches 255 exactly at the last row/column, so a full-range
        // sample is distinguishable from a clipped one.
        unsigned char r = (unsigned char)((long long)y * 255 / hd);
        for (int x = 0; x < w; x++) {
            unsigned char *p = bgra + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (ruler) {
                p[0] = 255; p[1] = 255; p[2] = 255;
            } else {
                // BGRA, the same order the decoder hands us: B, G, R, A.
                p[0] = 0;
                p[1] = (unsigned char)((long long)x * 255 / wd);
                p[2] = r;
            }
            p[3] = 255;
        }
    }
    return 0;
}
