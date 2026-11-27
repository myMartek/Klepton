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

// ---- TrackInfoList ----
//
// Laid out front to back, which FlatBuffers allows as long as every uoffset
// points forward: root, the list's vtable and table, the tracks vector, each
// track's vtable and table, then the strings. Every piece is 4-aligned.

static void fb_u16(unsigned char *b, size_t at, unsigned v) {
    b[at] = (unsigned char)(v & 0xff);
    b[at + 1] = (unsigned char)(v >> 8);
}
static void fb_u32(unsigned char *b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + (size_t)i] = (unsigned char)(v >> (8 * i));
}
static size_t fb_str_size(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return (4 + n + 1 + 3) & ~(size_t)3;
}

int kl_bstracks_encode(int n, const int *ids, const char *const *langs,
                       const char *const *labels, unsigned char **out, size_t *len) {
    if (!out || !len || n < 0 || (n > 0 && !ids)) return -1;
    enum { LIST_VT = 4, LIST_T = 16, VEC = 32, TRACK = 36 };   // TRACK: 16 vtable + 20 table
    size_t total = VEC + 4 + 4 * (size_t)n + TRACK * (size_t)n;
    for (int i = 0; i < n; i++)
        total += fb_str_size(labels ? labels[i] : NULL) + fb_str_size(langs ? langs[i] : NULL);
    unsigned char *b = calloc(1, total);
    if (!b) return -1;
    fb_u32(b, 0, LIST_T);                       // root -> list table
    fb_u16(b, LIST_VT + 0, 10);                 // vtable: 3 slots
    fb_u16(b, LIST_VT + 2, 16);                 // table size
    fb_u16(b, LIST_VT + 4, 4);                  // count
    fb_u16(b, LIST_VT + 6, 8);                  // activeTrackId
    fb_u16(b, LIST_VT + 8, 12);                 // tracks
    fb_u32(b, LIST_T + 0, LIST_T - LIST_VT);    // soffset: vtable = table - 12
    fb_u32(b, LIST_T + 4, (uint32_t)n);
    fb_u32(b, LIST_T + 8, 0);
    fb_u32(b, LIST_T + 12, VEC - (LIST_T + 12));
    fb_u32(b, VEC, (uint32_t)n);
    size_t tracks = VEC + 4 + 4 * (size_t)n;
    size_t str = tracks + TRACK * (size_t)n;
    for (int i = 0; i < n; i++) {
        size_t vt = tracks + TRACK * (size_t)i, t = vt + 16;
        size_t slot = VEC + 4 + 4 * (size_t)i;
        fb_u32(b, slot, (uint32_t)(t - slot));
        fb_u16(b, vt + 0, 16);                  // vtable: 6 slots
        fb_u16(b, vt + 2, 20);
        fb_u16(b, vt + 4, 4);                   // id
        fb_u16(b, vt + 6, 8);                   // format
        fb_u16(b, vt + 8, 0);
        fb_u16(b, vt + 10, 0);
        fb_u16(b, vt + 12, 12);                 // label
        fb_u16(b, vt + 14, 16);                 // language
        fb_u32(b, t + 0, 16);
        fb_u32(b, t + 4, (uint32_t)ids[i]);
        fb_u32(b, t + 8, 0);
        const char *pair[2] = { labels ? labels[i] : NULL, langs ? langs[i] : NULL };
        for (int k = 0; k < 2; k++) {
            size_t field = t + 12 + 4 * (size_t)k;
            const char *v = pair[k] ? pair[k] : "";
            size_t vn = strlen(v);
            fb_u32(b, field, (uint32_t)(str - field));
            fb_u32(b, str, (uint32_t)vn);
            memcpy(b + str + 4, v, vn);          // NUL and padding are calloc's
            str += fb_str_size(v);
        }
    }
    *out = b;
    *len = total;
    return 0;
}

// ---- SubtitleCue ----
//
// Front to back like the track list: root, vtable (11 slots, 26 bytes + 2
// pad), table { soffset, startTime, duration, lines }, the one-string vector,
// the string.
int kl_bscue_encode(int start_ms, int duration_ms, const char *text,
                    unsigned char **out, size_t *len) {
    if (!out || !len || !text) return -1;
    enum { VT = 4, T = 32, V = 48, S = 56 };
    size_t tn = strlen(text);
    size_t total = S + ((4 + tn + 1 + 3) & ~(size_t)3);
    unsigned char *b = calloc(1, total);
    if (!b) return -1;
    fb_u32(b, 0, T);
    fb_u16(b, VT + 0, 26);
    fb_u16(b, VT + 2, 16);
    fb_u16(b, VT + 4 + 2 * 2, 4);               // startTime
    fb_u16(b, VT + 4 + 2 * 3, 8);               // duration
    fb_u16(b, VT + 4 + 2 * 10, 12);             // lines
    fb_u32(b, T + 0, T - VT);
    fb_u32(b, T + 4, (uint32_t)start_ms);
    fb_u32(b, T + 8, (uint32_t)duration_ms);
    fb_u32(b, T + 12, V - (T + 12));
    fb_u32(b, V, 1);
    fb_u32(b, V + 4, S - (V + 4));
    fb_u32(b, S, (uint32_t)tn);
    memcpy(b + S + 4, text, tn);
    *out = b;
    *len = total;
    return 0;
}
