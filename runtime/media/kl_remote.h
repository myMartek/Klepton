// kl_remote — films that live on a web server, read by byte range.
//
// Bigscreen Beta's pCloud locations (visionos/Sources/KleptonBigscreenFolders.swift):
// the app mirrors a pCloud public folder as a tree of placeholder files (sparse,
// each as long as the film, no bytes on disk) inside a folder the game lists
// as Movies/<name>, and registers here which placeholder stands for which file
// on the server. kl_bsplayer then plays such a film from the server instead of
// the placeholder: AVFoundation through a resource loader (kl_avdec.m), FFmpeg
// through its own I/O callbacks (kl_ffplay.c). Nothing is copied: every read
// is an HTTP range request, kept in a small shared block cache in memory
// (KL_REMOTE_CACHE_MB, default 64) with a few blocks read ahead.
//
// pCloud's download links expire and are tied to the client, so the link is
// asked for when the film is first read, again after two hours, and again
// whenever the server refuses it.
//
// Nothing is registered unless the app registers it, which it does only for
// Bigscreen, so every other path, in every game, opens as before.
#ifndef KL_REMOTE_H
#define KL_REMOTE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One line per film: "<placeholder path>\t<kind>\t<a>\t<b>\t<c>\t<size>\n".
//   kind "pcloud": a = API host (eapi.pcloud.com / api.pcloud.com),
//                  b = the public link's code, c = the file id.
//   kind "url":    a = a direct http(s) URL (host tests); b, c unused.
// NULL or "" clears the list. Replaces the whole list; open handles keep
// reading from what they opened.
void kl_remote_files_set(const char *spec);

// 1 if `path` (a host path, symlinks resolved or not) is a registered placeholder.
int kl_remote_is(const char *path);

typedef struct kl_remote kl_remote;

// NULL if `path` is not registered.
kl_remote *kl_remote_open(const char *path);
void       kl_remote_close(kl_remote *r);
long long  kl_remote_size(kl_remote *r);
// The placeholder's file name ("Film.mkv"), for the container type.
const char *kl_remote_name(kl_remote *r);
// Up to `n` bytes at `off`. Short only at the end of the file; -1 when the
// server cannot be reached or keeps refusing (after retries).
long kl_remote_read(kl_remote *r, long long off, void *buf, size_t n);

// For the log and the host test: requests made, bytes fetched, link lookups.
void kl_remote_stats(unsigned long *requests, unsigned long long *bytes,
                     unsigned long *resolves);

#ifdef __cplusplus
}
#endif
#endif
