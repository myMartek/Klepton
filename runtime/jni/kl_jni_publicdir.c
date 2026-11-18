// See kl_jni_publicdir.h.
#include "kl_jni_publicdir.h"

#include <stdio.h>
#include <string.h>

void kl_public_dir_for_type(const char *type, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!type || !*type) snprintf(out, cap, "/sdcard");
    else snprintf(out, cap, "/sdcard/%s", type);
}
