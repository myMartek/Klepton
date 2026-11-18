// Guest-visible spelling of Android's public shared-storage directories.
//
// The guest names the Quest's shared film folder either as a literal
// ("/sdcard/Movies") or via
// Environment.getExternalStoragePublicDirectory(DIRECTORY_MOVIES). Both meet
// here: NULL/"" is the storage root, anything else appends as-is. The result
// is still a guest spelling — kl_guest_path maps the /sdcard prefix onto the
// container (Documents/android-files on visionOS).
#ifndef KL_JNI_PUBLICDIR_H
#define KL_JNI_PUBLICDIR_H

#include <stddef.h>

void kl_public_dir_for_type(const char *type, char *out, size_t cap);
// Delete-flag: devicectl cannot delete from the container, so the app honors
// a __delete__.txt list (one film file name per line) inside the Movies
// directory: each listed basename is unlinked, the flag removed afterwards.
// Refuses paths (slashes, ".."), the flag itself and blanks. Returns the
// number of films deleted; missing flag or dir is a silent 0. NULL-safe.
int kl_movies_process_delete_flag(const char *movies_host_dir);

#endif
