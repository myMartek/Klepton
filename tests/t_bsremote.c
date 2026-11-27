// Bigscreen Beta's films on a server (runtime/media/kl_remote.h, the pCloud
// locations): a placeholder with no bytes in it plays from a web server that
// answers byte ranges, which is what pCloud's download links are.
//
//   MP4 (index at the end) through AVFoundation's resource loader,
//   MKV through FFmpeg's I/O callbacks (with the Mac FFmpeg build),
//   a link the server refuses once (an expired one) is asked for again,
//   an MP4 with DTS 5.1 sound (no Apple decoder): picture through
//   AVFoundation, sound through FFmpeg, from the server and from a file,
//   and nothing is ever read from the placeholder itself.
//
// `make bsremote` starts tests/range_server.py and passes its port.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "../runtime/media/kl_bsplayer.h"
#include "../runtime/media/kl_remote.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-60s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

static long long fsize(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

// A sparse file as long as the film: what the app writes for every film of a
// pCloud folder.
static void placeholder(const char *path, long long size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (ftruncate(fd, size) != 0) perror("ftruncate");
        close(fd);
    }
}

static void play(const char *path, const char *what) {
    printf("--- %s\n", what);
    char reason[512] = "";
    kl_bsplayer *p = kl_bsplayer_open(path, reason, sizeof reason);
    ok(p != NULL, "opens from the server");
    if (!p) { printf("    %s\n", reason); return; }
    int w = 0, h = 0;
    const char *mime = NULL;
    long long dur = 0;
    ok(kl_bsplayer_info(p, &w, &h, &mime, &dur) == 0 && w == 160 && h == 90, "160x90");
    ok(dur > 900000 && dur < 1200000, "about one second long");
    int rate = 0, ch = 0;
    ok(kl_bsplayer_audio_info(p, &rate, &ch) == 1 && ch == 2, "stereo sound");
    unsigned char *bgra = NULL;
    int fw = 0, fh = 0;
    ok(kl_bsplayer_decode_first(p, &bgra, &fw, &fh) == 0 && bgra && fw == 160 && fh == 90,
       "a first picture");
    // AVFoundation's first picture is now and then black, from a local file
    // too (1 in 40 here, 2026-11-27), so the colour is asked of either picture.
    int coloured = 0;
    for (int k = 0; k < 2; k++) {
        if (bgra) {
            size_t nz = 0;
            for (size_t i = 0; i < (size_t)fw * fh * 4; i++) nz += bgra[i] != 0;
            coloured |= nz > (size_t)fw * fh;
            free(bgra);
            bgra = NULL;
        }
        if (k == 0) {
            ok(kl_bsplayer_seek_to_us(p, 500000) == 500000, "seek to 0.5 s");
            ok(kl_bsplayer_decode_first(p, &bgra, &fw, &fh) == 0 && bgra, "a picture after the seek");
        }
    }
    ok(coloured, "the pictures are the film's (not the placeholder's zeros)");
    kl_bsplayer_close(p);
}

// The sound the pump hands a sink while the film plays half a second.
struct ear { unsigned rate, ch; size_t bytes, loud; };
static int ear_open(void *c, unsigned rate, unsigned ch) {
    struct ear *e = c; e->rate = rate; e->ch = ch; return 0;
}
static size_t ear_write(void *c, const void *pcm, size_t n) {
    struct ear *e = c;
    const short *s = pcm;
    for (size_t i = 0; i < n / 2; i++) e->loud += s[i] > 500 || s[i] < -500;
    e->bytes += n;
    usleep(5000);       // a device takes about real time, not all at once
    return n;
}
static void ear_nop(void *c) { (void)c; }

static void hear(const char *path, const char *what) {
    printf("--- %s\n", what);
    char reason[512] = "";
    kl_bsplayer *p = kl_bsplayer_open(path, reason, sizeof reason);
    ok(p != NULL, "opens");
    if (!p) { printf("    %s\n", reason); return; }
    int rate = 0, ch = 0;
    ok(kl_bsplayer_audio_info(p, &rate, &ch) == 1 && rate == 48000 && ch == 2,
       "48 kHz stereo sound");
    struct ear e = {0};
    kl_bsplayer_audio_sink sink = { ear_open, ear_write, ear_nop, ear_nop, ear_nop, ear_nop };
    kl_bsplayer_set_audio_sink(p, &sink, &e);
    kl_bsplayer_play(p);
    for (int i = 0; i < 100 && e.bytes < 48000 * 4 / 2; i++) usleep(20000);
    kl_bsplayer_stop(p);
    printf("    %zu bytes, %zu loud samples\n", e.bytes, e.loud);
    ok(e.ch == 2 && e.bytes >= 48000 * 4 / 4, "the sink got a quarter second of PCM");
    ok(e.loud > 1000, "and it is the tone, not silence");
    kl_bsplayer_close(p);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== bigscreen films from a server ===\n");
    if (argc < 2) {
        printf("  no port: run `make bsremote`\n");
        return 1;
    }
    const char *port = argv[1];
    char dir[] = "/tmp/kl-bsremote-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char mp4[256], mkv[256], flaky[256], stray[256], dts[256];
    snprintf(mp4, sizeof mp4, "%s/Film.mp4", dir);
    snprintf(mkv, sizeof mkv, "%s/Film.mkv", dir);
    snprintf(flaky, sizeof flaky, "%s/Expired.mp4", dir);
    snprintf(stray, sizeof stray, "%s/Local.mp4", dir);
    snprintf(dts, sizeof dts, "%s/Remux-DTS.mp4", dir);
    long long n4 = fsize("tests/data/bs_tiny_remote.mp4");
    long long nk = fsize("tests/data/bs_tiny.mkv");
    long long nd = fsize("tests/data/bs_tiny_dts.mp4");
    placeholder(dts, nd);
    placeholder(mp4, n4);
    placeholder(mkv, nk);
    placeholder(flaky, n4);
    placeholder(stray, n4);

    char spec[3072];
    snprintf(spec, sizeof spec,
             "%s\turl\thttp://127.0.0.1:%s/bs_tiny_remote.mp4\t-\t-\t%lld\n"
             "%s\turl\thttp://127.0.0.1:%s/bs_tiny.mkv\t-\t-\t%lld\n"
             "%s\turl\thttp://127.0.0.1:%s/flaky/bs_tiny_remote.mp4\t-\t-\t%lld\n"
             "%s\turl\thttp://127.0.0.1:%s/bs_tiny_dts.mp4\t-\t-\t%lld\n",
             mp4, port, n4, mkv, port, nk, flaky, port, n4, dts, port, nd);
    kl_remote_files_set(spec);
    ok(kl_remote_is(mp4) && kl_remote_is(mkv), "the placeholders are registered");
    ok(!kl_remote_is(stray), "a file that is not registered stays a file");

    // Straight reads: the bytes are the server's.
    kl_remote *r = kl_remote_open(mp4);
    unsigned char a[64], b[64];
    FILE *f = fopen("tests/data/bs_tiny_remote.mp4", "rb");
    size_t got = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    ok(r && kl_remote_read(r, 0, a, sizeof a) == (long)sizeof a && got == sizeof b &&
       memcmp(a, b, sizeof a) == 0, "the first bytes are the film's");
    ok(r && kl_remote_read(r, n4 - 10, a, 64) == 10, "a read past the end is short");
    kl_remote_close(r);

    play(mp4, "MP4 through AVFoundation");
    play(flaky, "MP4 whose link is refused once (expired)");
    hear(mp4, "MP4 sound (AAC) through AVFoundation");
    if (getenv("KL_FFMPEG_LIB")) {
        play(mkv, "MKV through FFmpeg");
        play(dts, "MP4 with DTS 5.1 sound: picture through AVFoundation");
        hear(dts, "its sound through FFmpeg, from the server");
        hear("tests/data/bs_tiny_dts.mp4", "and from a file");
    } else {
        printf("  (no KL_FFMPEG_LIB: MKV and DTS through FFmpeg are skipped)\n");
    }

    unsigned long req = 0, res = 0;
    unsigned long long bytes = 0;
    kl_remote_stats(&req, &bytes, &res);
    printf("  %lu requests, %llu bytes\n", req, bytes);
    ok(bytes > 0 && bytes <= (unsigned long long)(3 * n4 + nk + 3 * nd) * 2,
       "each film read about once, not copied over and over");

    unlink(dts); unlink(mp4); unlink(mkv); unlink(flaky); unlink(stray); rmdir(dir);
    printf("%s bigscreen films from a server\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
