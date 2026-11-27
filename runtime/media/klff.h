// KleptonFFmpeg.framework's own C API: the films AVFoundation cannot open
// (Bigscreen Beta: MKV, AVI, WebM, TS/M2TS, WMV, OGV, ...).
//
// The framework is FFmpeg (LGPL 2.1 or later, decoders and demuxers only, no
// GPL parts) plus the small shim in tools/ffmpeg/klff_shim.c, linked into one
// dylib by tools/ffmpeg_build.sh. The runtime never links it: kl_ffplay.c
// dlopens it by path and resolves the functions below, so no FFmpeg type or
// header reaches the runtime, a game without the framework is unchanged, and
// the library can be replaced on its own (what the LGPL asks of a program that
// uses it).
//
// One handle reads one stream of one file: KLFF_VIDEO decodes the best video
// stream to tightly packed BGRA, KLFF_AUDIO the best audio stream to signed
// 16-bit interleaved PCM (at most two channels; more are mixed down). A film
// opens one of each, so the picture and the sound keep their own cursors, as
// they do on the AVFoundation path (kl_avdec + kl_avdemux).
#ifndef KLFF_H
#define KLFF_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct klff klff;

enum { KLFF_VIDEO = 0, KLFF_AUDIO = 1 };

// NULL and a reason in `err` when the file cannot be read or has no stream of
// that kind.
klff *klff_open(const char *path, int kind, char *err, int cap);
void  klff_close(klff *f);

// The same, reading through the caller instead of a file: a film on a server
// (runtime/media/kl_remote.h). `read` gives up to n bytes at off (short only
// at the end, -1 on an error); `size` is the file's length; `name` is only a
// hint for the container ("Film.mkv"). `done(ctx)`, if set, is called once
// by klff_close (also when the open fails). Since the pCloud locations
// (2026-11-27); a framework without it plays local files only.
typedef long (*klff_read_fn)(void *ctx, long long off, void *buf, unsigned long n);
typedef void (*klff_done_fn)(void *ctx);
klff *klff_open_io(klff_read_fn read, klff_done_fn done, void *ctx, long long size,
                   const char *name, int kind, char *err, int cap);

// What the stream is. `codec` gets FFmpeg's codec name ("hevc", "ac3").
// For audio, rate and channels are those of the PCM klff_audio_next gives.
// Returns 1.
int klff_info(klff *f, int *w, int *h, long long *duration_us, double *fps,
              char *codec, int codec_cap, int *rate, int *channels);

// The next picture: 1 and a BGRA frame that stays valid until the next call
// on this handle, 0 at the end, -1 on an error that ends the stream.
// `hw` says whether VideoToolbox decoded it.
int klff_video_next(klff *f, const unsigned char **bgra, int *w, int *h,
                    long long *pts_us, int *hw);

// The next piece of sound: 1 and `frames` PCM frames valid until the next
// call, 0 at the end, -1 on an error that ends the stream.
int klff_audio_next(klff *f, const short **pcm, int *frames, long long *pts_us);

// To `us` from the start. The next frame given is the first at or after it.
int klff_seek(klff *f, long long us);

// Every audio stream of the file, one line each in file order:
// "language\ttitle\tcodec\tchannels\tdefault\tcurrent\tid\n" (id is the
// container's own track number: an MP4's track ID; language is the
// file's tag, often ISO 639-2 like "ger"; codec is a profile such as
// "DTS-HD MA" when there is one). Returns how many, -1 on bad arguments.
// Since the Audio Tracks menu (2026-11-27); optional, like klff_open_io.
int klff_audio_tracks(klff *f, char *buf, int cap);

// An audio handle reads the nth audio stream (0-based, the order above)
// from now on; seek it afterwards. 0, or -1 when there is no such stream or
// FFmpeg cannot decode it (the handle keeps its stream then).
int klff_audio_select(klff *f, int nth);

// "FFmpeg 9.0.2", for the log.
const char *klff_version(void);

#ifdef __cplusplus
}
#endif
#endif
