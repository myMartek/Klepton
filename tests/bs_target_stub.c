// Stub for the Bigscreen host tests that link kl_avdec.m but not the audio
// backend.
//
// toolchain-850's kl_avdec.m feeds a movie's soundtrack through kl_audio's
// source API (open/write/play/pause/flush/close). The decoder, present and
// transport tests only look at video; the audio test injects its own sink.
// Linking runtime/media/kl_audio.c for these would open a real device and race
// the test's own assertions, so the calls are accepted and dropped here.
//
// It also carries the target name (see below), which kl_avdec.m asks for.
#include <stddef.h>

// The 850 decoder asks the driver which target it is running for
// (KL_AVDEC_AUDIO is enabled for Bulletstorm VR only). These tests decode the
// fixture files, not a guest, so there is no target name; NULL makes every
// per-target check take its default. Linking runtime/guest/kl_driver.c here
// would drag the whole guest runtime into a decoder test for one string.
const char *kl_driver_target_name(void) { return 0; }

int kl_audio_open_src(const void *tag, const char *name,
                      unsigned rate, unsigned channels, unsigned bits) {
    (void)tag; (void)name; (void)rate; (void)channels; (void)bits;
    return 0;
}
void kl_audio_close_src(const void *tag) { (void)tag; }
size_t kl_audio_write_src(const void *tag, const void *pcm, size_t bytes) {
    (void)tag; (void)pcm;
    return bytes;                      // accepted, and dropped
}
void kl_audio_play_src(const void *tag) { (void)tag; }
void kl_audio_pause_src(const void *tag) { (void)tag; }
void kl_audio_flush_src(const void *tag) { (void)tag; }
