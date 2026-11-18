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

#endif
