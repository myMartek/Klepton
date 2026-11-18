// Bigscreen local-file player 1/6: session open/info/close (2026-10-24).
// Thin shell around kl_avdec (all-in-one file player) + kl_avdemux (track
// facts): Bigscreen needs a local-file SESSION, not NDK extractor+codec.
// Assets (all < 100 KB, committed):
//   ffmpeg -f lavfi -i "testsrc2=size=160x90:rate=10:duration=1" -f lavfi -i \
//     "sine=frequency=440:duration=1" -c:v libx264 -pix_fmt yuv420p -c:a aac \
//     -shortest tests/data/bs_tiny.mp4
//   ffmpeg -f lavfi -i "testsrc2=size=160x90:rate=10:duration=1" -c:v libx264 \
//     -pix_fmt yuv420p -t 1 -f matroska tests/data/bs_tiny.mkv
//   ffmpeg -f lavfi -i "testsrc2=size=160x90:rate=10:duration=1" -f lavfi -i \
//     "sine=frequency=440:duration=1" -c:v libx265 -pix_fmt yuv420p -tag:v hev1 \
//     -c:a aac -shortest tests/data/bs_tiny_hevc.mp4
// (The mkv carries a real H.264 track: its refusal proves the CONTAINER is
// unsupported, not that the track is missing.)
// `make bs_demux`, in `make check`.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../runtime/media/kl_bsplayer.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("=== bigscreen demux ===\n");
    char reason[256];

    // The good path: tiny MP4 opens, facts read off the asset.
    kl_bsplayer *p = kl_bsplayer_open("tests/data/bs_tiny.mp4", reason, sizeof reason);
    ok(p != NULL, "tiny mp4 opens");
    if (p) {
        int w = 0, h = 0;
        const char *mime = NULL;
        long long dur = 0;
        ok(kl_bsplayer_info(p, &w, &h, &mime, &dur) == 0, "info answers");
        ok(w == 160 && h == 90, "160x90 off the asset");
        ok(mime && strcmp(mime, "video/avc") == 0, "mime video/avc");
        ok(dur > 0, "duration positive");
        kl_bsplayer_close(p);
    }

    // Missing file: NULL with a reason, never a crash.
    reason[0] = '\0';
    ok(kl_bsplayer_open("tests/data/does-not-exist.mp4", reason, sizeof reason) == NULL,
       "missing file -> NULL");
    ok(reason[0] != '\0', "missing file names a reason");

    // MKV container: refused with a reason (AVFoundation has no splitter).
    reason[0] = '\0';
    ok(kl_bsplayer_open("tests/data/bs_tiny.mkv", reason, sizeof reason) == NULL,
       "mkv -> NULL");
    ok(reason[0] != '\0', "mkv names a reason");

    // A 3 GB non-movie must fail WITHOUT reading gigabytes: open returns.
    char big[64];
    snprintf(big, sizeof big, "build/bs_big.bin");
    int fd = open(big, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        int rc = ftruncate(fd, (off_t)3 << 30);
        close(fd);
        ok(rc == 0, "3 GB sparse fixture made");
        if (rc == 0) {
            reason[0] = '\0';
            ok(kl_bsplayer_open(big, reason, sizeof reason) == NULL,
               "3 GB non-movie -> NULL");
            ok(reason[0] != '\0', "3 GB non-movie names a reason");
        }
        unlink(big);
    } else {
        ok(0, "3 GB sparse fixture made");
    }

    // NULL-close is safe.
    kl_bsplayer_close(NULL);
    ok(1, "close(NULL) safe");

    // hev1 sample entry (ffmpeg's spelling; kCMVideoCodecType_HEVC is hvc1):
    // same HEVC codec, must open like any H.264 file.
    {
        char reason[256];
        kl_bsplayer *hevc = kl_bsplayer_open("tests/data/bs_tiny_hevc.mp4",
                                             reason, sizeof reason);
        ok(hevc != NULL, "hev1 mp4 opens");
        if (hevc) {
            int w = 0, h = 0;
            const char *mime = NULL;
            long long dur = 0;
            ok(kl_bsplayer_info(hevc, &w, &h, &mime, &dur) == 0 &&
               w == 160 && h == 90, "hev1 facts 160x90");
            ok(mime && !strcmp(mime, "video/hevc"), "hev1 mime video/hevc");
            kl_bsplayer_close(hevc);
        } else {
            printf("  (reason: %s)\n", reason);
        }
    }

    printf("%s bigscreen demux\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
