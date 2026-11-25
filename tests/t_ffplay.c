// Bigscreen Beta's films through FFmpeg (runtime/media/kl_ffplay.c): what
// AVFoundation refuses (here an MKV with H.264 and 5.1 AC-3, and an AVI with
// MPEG-4 Part 2 and MP3) opens through kl_bsplayer_open, gives its size and
// length, a first picture, stereo sound and a seek.
//
// Needs the Mac build of the framework: `tools/ffmpeg_build.sh macos`, which
// `make ffplay` runs when it is missing. KL_FFMPEG_LIB points the runtime at it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/media/kl_bsplayer.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-56s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

static void one(const char *path, int want_rate, int want_ch) {
    printf("--- %s\n", path);
    char reason[512] = "";
    kl_bsplayer *p = kl_bsplayer_open(path, reason, sizeof reason);
    ok(p != NULL, "opens (AVFoundation refuses it, FFmpeg takes it)");
    if (!p) { printf("    %s\n", reason); return; }
    int w = 0, h = 0;
    const char *mime = NULL;
    long long dur = 0;
    ok(kl_bsplayer_info(p, &w, &h, &mime, &dur) == 0 && w == 160 && h == 90,
       "160x90");
    ok(dur > 900000 && dur < 1200000, "about one second long");
    int rate = 0, ch = 0;
    ok(kl_bsplayer_audio_info(p, &rate, &ch) == 1 && rate == want_rate && ch == want_ch,
       "sound as s16 at the file's rate, 5.1 mixed to stereo");
    unsigned char *bgra = NULL;
    int fw = 0, fh = 0;
    ok(kl_bsplayer_decode_first(p, &bgra, &fw, &fh) == 0 && bgra && fw == 160 && fh == 90,
       "a first picture, 160x90 BGRA");
    if (bgra) {
        size_t nz = 0;
        for (size_t i = 0; i < (size_t)fw * fh * 4; i++) nz += bgra[i] != 0;
        ok(nz > (size_t)fw * fh, "the picture is not empty");
        free(bgra);
    }
    // A seek, then the first picture after it.
    ok(kl_bsplayer_seek_to_us(p, 500000) == 500000, "seek to 0.5 s");
    bgra = NULL;
    ok(kl_bsplayer_decode_first(p, &bgra, &fw, &fh) == 0 && bgra, "a picture after the seek");
    free(bgra);
    ok(kl_bsplayer_audio_position_us(p) >= 400000, "the sound cursor followed the seek");
    kl_bsplayer_close(p);
}

int main(void) {
    printf("=== bigscreen films through FFmpeg ===\n");
    if (!getenv("KL_FFMPEG_LIB")) {
        printf("  KL_FFMPEG_LIB not set: run `make ffplay`\n");
        return 1;
    }
    one("tests/data/bs_tiny.mkv", 48000, 2);   // 5.1 AC-3
    one("tests/data/bs_tiny.avi", 44100, 1);   // mono MP3
    printf("%s bigscreen films through FFmpeg\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
