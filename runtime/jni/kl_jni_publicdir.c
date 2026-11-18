// See kl_jni_publicdir.h.
#include "kl_jni_publicdir.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void kl_public_dir_for_type(const char *type, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!type || !*type) snprintf(out, cap, "/sdcard");
    else snprintf(out, cap, "/sdcard/%s", type);
}

#define KL_MOVIES_DELETE_FLAG "__delete__.txt"

int kl_movies_process_delete_flag(const char *movies_host_dir) {
    if (!movies_host_dir || !*movies_host_dir) return 0;
    char flag[1152];
    snprintf(flag, sizeof flag, "%s/%s", movies_host_dir, KL_MOVIES_DELETE_FLAG);
    FILE *f = fopen(flag, "r");
    if (!f) return 0;
    int deleted = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        // Trim trailing whitespace/newlines.
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = '\0';
        // Only plain basenames: no paths, no flag, no blanks.
        if (!n || !strcmp(line, KL_MOVIES_DELETE_FLAG) ||
            strchr(line, '/') || strstr(line, "..")) {
            fprintf(stderr, "  [movies] delete-flag refused: \"%s\"\n", line);
            continue;
        }
        char victim[1152];
        snprintf(victim, sizeof victim, "%s/%s", movies_host_dir, line);
        if (unlink(victim) == 0) {
            fprintf(stderr, "  [movies] deleted \"%s\"\n", line);
            deleted++;
        } else {
            fprintf(stderr, "  [movies] delete-flag missing: \"%s\"\n", line);
        }
    }
    fclose(f);
    unlink(flag);
    return deleted;
}
