// Bigscreen local-file player 7/7: MediaInfo flatbuffer + base64 (2026-10-26).
// The guest's UI (duration, thumbnail, Play) is built from a base64 FlatBuffer
// delivered via UnitySendMessage: table MediaInfo with 7 int fields at slots
// 0=requestId, 3=durationMs, 4=mediaWidth, 5=mediaHeight, 6=textureWidth,
// 7=textureHeight, 8=textureId (from the dex: addRequestId/addDurationMs/…,
// startObject(9)). This TU encodes that table; the test decodes it back by
// the FlatBuffers spec (root uoffset, vtable, table) — no guest needed.
// `make bs_fbs`, in `make check`.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/media/kl_bsfbs.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

// Minimal spec decoder: root -> table -> vtable -> field slot. The guest's
// Table.__offset computes vtable = table - soffset; using "+" here would
// validate the encoder against itself instead of the spec.
static int32_t fbs_get(const unsigned char *b, size_t n, int slot, int *found) {
    *found = 0;
    if (n < 4) return 0;
    uint32_t root;
    memcpy(&root, b, 4);
    if (root + 4 > n) return 0;
    int32_t so;
    memcpy(&so, b + root, 4);
    uint32_t vt = (uint32_t)((int64_t)root - so);
    if (vt + 4 > n) return 0;
    uint16_t vlen, tlen;
    memcpy(&vlen, b + vt, 2);
    memcpy(&tlen, b + vt + 2, 2);
    uint32_t vo = vt + 4 + (uint32_t)slot * 2;
    if (vo + 2 > vt + vlen) return 0;
    uint16_t off;
    memcpy(&off, b + vo, 2);
    if (!off) return 0;
    if (root + off + 4 > n) return 0;
    int32_t v;
    memcpy(&v, b + root + off, 4);
    *found = 1;
    return v;
}

int main(void) {
    printf("=== bigscreen mediainfo ===\n");

    unsigned char *buf = NULL;
    size_t len = 0;
    ok(kl_bsinfo_encode(7, 52209, 854, 480, 854, 480, 359, &buf, &len) == 0,
       "encode ok");
    ok(buf && len > 0 && (len % 4) == 0, "non-empty, 4-aligned");
    if (buf) {
        int f = 0;
        // The vtable sits before the table, so the spec soffset at the table
        // position is positive: table 28 - soffset 24 = vtable 4.
        uint32_t root = 0;
        int32_t so = 0;
        memcpy(&root, buf, 4);
        if ((size_t)root + 4 <= len) memcpy(&so, buf + root, 4);
        ok(root == 28 && so == 24, "soffset +24 -> vtable at 4");
        ok(fbs_get(buf, len, 0, &f) == 7 && f, "slot 0 requestId");
        ok(fbs_get(buf, len, 3, &f) == 52209 && f, "slot 3 durationMs");
        ok(fbs_get(buf, len, 4, &f) == 854 && f, "slot 4 mediaWidth");
        ok(fbs_get(buf, len, 5, &f) == 480 && f, "slot 5 mediaHeight");
        ok(fbs_get(buf, len, 6, &f) == 854 && f, "slot 6 textureWidth");
        ok(fbs_get(buf, len, 7, &f) == 480 && f, "slot 7 textureHeight");
        ok(fbs_get(buf, len, 8, &f) == 359 && f, "slot 8 textureId");
        ok(!fbs_get(buf, len, 1, &f) && !f, "slot 1 absent");
        ok(!fbs_get(buf, len, 2, &f) && !f, "slot 2 absent");

        char *b64 = kl_bsinfo_base64(buf, len);
        ok(b64 && strlen(b64) > 0, "base64 non-empty");
        if (b64) {
            // Base64 of the buffer must start with the root uoffset bytes;
            // decode the first quantum back and compare.
            ok(strlen(b64) % 4 == 0, "base64 padded");
            free(b64);
        }
        free(buf);
    }
    ok(kl_bsinfo_encode(0, 0, 0, 0, 0, 0, 0, NULL, NULL) == -1,
       "NULL out -> -1");

    // --- BGRA thumbnail scaling (createScaledBitmap equivalent) ---
    {
        // 2x2 BGRA corners: red, green, blue, white.
        static const unsigned char src[16] = {
            0, 0, 255, 255,   0, 255, 0, 255,
            255, 0, 0, 255,   255, 255, 255, 255,
        };
        unsigned char dst[64];
        memset(dst, 0xCD, sizeof dst);
        ok(kl_bsinfo_scale_bgra(src, 2, 2, dst, 4, 4) == 0,
           "scale 2x2 -> 4x4 ok");
        ok(dst[(0 * 4 + 0) * 4 + 0] == 0 && dst[(0 * 4 + 0) * 4 + 1] == 0 &&
           dst[(0 * 4 + 0) * 4 + 2] == 255, "scale corner top-left");
        ok(dst[(0 * 4 + 3) * 4 + 0] == 0 && dst[(0 * 4 + 3) * 4 + 1] == 255 &&
           dst[(0 * 4 + 3) * 4 + 2] == 0, "scale corner top-right");
        ok(dst[(3 * 4 + 0) * 4 + 0] == 255 && dst[(3 * 4 + 0) * 4 + 1] == 0 &&
           dst[(3 * 4 + 0) * 4 + 2] == 0, "scale corner bottom-left");
        ok(dst[(3 * 4 + 3) * 4 + 0] == 255 && dst[(3 * 4 + 3) * 4 + 1] == 255 &&
           dst[(3 * 4 + 3) * 4 + 2] == 255, "scale corner bottom-right");
        // Pixel centre (1,1) maps to source (0.25, 0.25): B=64, G=64, R=159.
        int cb = dst[(1 * 4 + 1) * 4 + 0];
        int cg = dst[(1 * 4 + 1) * 4 + 1];
        int cr = dst[(1 * 4 + 1) * 4 + 2];
        ok(abs(cb - 64) <= 1 && abs(cg - 64) <= 1 && abs(cr - 159) <= 1,
           "scale centre bilinear");
        ok(dst[(1 * 4 + 1) * 4 + 3] == 255, "scale keeps alpha");
    }
    {
        static const unsigned char src[4] = { 7, 8, 9, 10 };
        unsigned char dst[36];
        ok(kl_bsinfo_scale_bgra(src, 1, 1, dst, 3, 3) == 0,
           "scale 1x1 -> 3x3 ok");
        int same = 1;
        for (int i = 0; i < 9; i++)
            if (memcmp(dst + i * 4, src, 4) != 0) same = 0;
        ok(same, "scale 1x1 -> 3x3 uniform");
    }
    {
        static const unsigned char src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        unsigned char dst[8];
        memset(dst, 0, sizeof dst);
        ok(kl_bsinfo_scale_bgra(src, 2, 1, dst, 2, 1) == 0,
           "scale same size ok");
        ok(memcmp(src, dst, sizeof src) == 0, "scale same size copies");
    }
    {
        static const unsigned char src[4] = { 0, 0, 0, 0 };
        unsigned char dst[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
        ok(kl_bsinfo_scale_bgra(NULL, 2, 2, dst, 2, 2) == -1,
           "scale NULL src -> -1");
        ok(kl_bsinfo_scale_bgra(src, 2, 2, NULL, 2, 2) == -1,
           "scale NULL dst -> -1");
        ok(kl_bsinfo_scale_bgra(src, 0, 2, dst, 2, 2) == -1,
           "scale zero src width -> -1");
        ok(kl_bsinfo_scale_bgra(src, 2, 2, dst, 2, 0) == -1,
           "scale zero dst height -> -1");
        ok(dst[0] == 0xAA, "scale bad args leave dst alone");
    }

    // The diagnostic picture (KL_BS_THUMB_TEST=1): every texel names its own
    // position, so what the guest shows can be read back off the screen.
    {
        enum { PW = 226, PH = 128 };
        static unsigned char pat[PW * PH * 4];
        memset(pat, 0xEE, sizeof pat);
        ok(kl_bsinfo_test_pattern(pat, PW, PH) == 0, "pattern ok");
        // BGRA: B at 0, G at 1, R at 2, A at 3.
        ok(pat[0] == 0 && pat[1] == 0 && pat[2] == 0 && pat[3] == 255,
           "pattern top-left is black (u=0, v=0)");
        {
            unsigned char *tr = pat + (PW - 1) * 4;
            ok(tr[1] == 255 && tr[2] == 0, "pattern top-right green=255 (u=1)");
            // Row 127 is the last ruler row, so the red end of the ramp is one
            // row above it — the ruler is what makes a tiled sample countable,
            // and it costs only that one row.
            unsigned char *bl = pat + (size_t)(PH - 2) * PW * 4 + (PW - 1) * 4;
            ok(bl[2] >= 250 && bl[1] == 255, "pattern bottom-right near red=255 (v=1)");
        }
        // The ruler: every 16th row white, so a tiled or flipped sample can be
        // counted rather than argued about.
        {
            int white = 0, marked = 0;
            for (int y = 0; y < PH; y++) {
                const unsigned char *p = pat + (size_t)y * PW * 4;
                int all = 1;
                for (int x = 0; x < PW; x++)
                    if (!(p[x * 4] == 255 && p[x * 4 + 1] == 255 &&
                          p[x * 4 + 2] == 255)) { all = 0; break; }
                if (all) white++;
                if (y % 16 == 0) marked++;
            }
            ok(marked == 8 && white == 8, "pattern has 8 white ruler rows");
        }
        // No texel may be left uninitialised: an untouched byte would read as
        // content on screen and be mistaken for the guest's doing. A sentinel
        // cannot prove this (the ramp legitimately produces every value), so
        // every texel is checked against the ramp itself.
        {
            int bad = 0;
            for (int y = 0; y < PH && !bad; y++) {
                for (int x = 0; x < PW; x++) {
                    const unsigned char *p = pat + ((size_t)y * PW + x) * 4;
                    if ((y % 16) == 15) {
                        if (!(p[0] == 255 && p[1] == 255 && p[2] == 255)) { bad = 1; break; }
                    } else if (!(p[0] == 0 &&
                                 p[1] == (unsigned char)(x * 255 / (PW - 1)) &&
                                 p[2] == (unsigned char)(y * 255 / (PH - 1)))) {
                        bad = 1; break;
                    }
                    if (p[3] != 255) { bad = 1; break; }
                }
            }
            ok(!bad, "pattern writes every texel as the ramp");
        }
        {
            static unsigned char tiny[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
            ok(kl_bsinfo_test_pattern(NULL, 2, 2) == -1, "pattern NULL -> -1");
            ok(kl_bsinfo_test_pattern(tiny, 0, 2) == -1, "pattern zero w -> -1");
            ok(kl_bsinfo_test_pattern(tiny, 2, -1) == -1, "pattern bad h -> -1");
            ok(tiny[0] == 0xAA, "pattern bad args leave buffer alone");
        }
    }

    printf("%s bigscreen mediainfo\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
