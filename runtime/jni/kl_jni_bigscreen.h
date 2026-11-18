// Bigscreen local-file film: the decoded-frame handoff to the mirror hook in
// kl_ovrp.c. The session (kl_jni_bigscreen.c) keeps the newest BGRA frame;
// the hook uploads it into its own GL texture and mirrors that under the
// film quad's layer id.
#ifndef KL_JNI_BIGSCREEN_H
#define KL_JNI_BIGSCREEN_H

// Newest held frame, or NULL when none was ever latched. Buffer stays owned
// by the session and valid until the next latched frame; `serial` counts
// latched frames so the hook uploads each exactly once.
const unsigned char *kl_bigscreen_film_frame(int *w, int *h,
                                             unsigned long long *serial);

// Frame-boundary step for MediaInfo thumbnails (GL is current there).
void kl_bigscreen_info_boundary(void);

#endif
