// The asset files a title's installation says it has: id, file and size, as
// the installer read them off the store. kl_ovrplat.c answers the platform's
// asset-file questions (ovr_AssetFile_GetList, StatusById/ByName and the
// AssetDetails readers) from this list where one is configured.
//
// The file: <files>/klepton-asset-files.txt, or the path in
// KL_ASSET_FILES_PATH. Written by the installer (AVP Play's recipe). One entry
// per line, fields separated by blanks:
//
//   asset <store asset id> <file> <size in bytes> [<language tag>]
//
//   # Marvel's Deadpool VR: its voice-over packs, loaded by libUE4.so
//   asset 1234567890123456 de.lang 412345678 de
//   asset 2345678901234567 fr.lang 398765432
//
// <file> is relative to the game's OBB directory (kl_jni_obb_dir), or an
// absolute path inside it. The tag defaults to the file name up to its first
// '.' ("de.lang" -> "de"). '#' starts a comment line; blank lines are skipped.
//
// What is rejected, the whole line, with the reason logged: a line that is not
// exactly those fields; an id or size that is not a plain decimal number above
// zero; a path with a "." or ".." component, an empty component, a blank or
// control character or a backslash; an absolute path outside the OBB directory,
// or one that leads out of it through a symbolic link; a tag outside
// [A-Za-z0-9_-]{1,31}; a second line with an id or file already listed.
//
// An accepted entry is INSTALLED only when its file is a regular file of
// exactly the size given. Anything else is listed as not installed, and
// nothing in Klepton fetches it.
//
// No dependencies beyond libc, so the parser is tested on its own
// (tests/t_assetcfg.c, `make assetcfg`).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define KL_ASSETCFG_MAX       32     // entries kept from one file
#define KL_ASSETCFG_FILE      "klepton-asset-files.txt"
#define KL_ASSETCFG_ENV       "KL_ASSET_FILES_PATH"

typedef struct {
    uint64_t           id;           // the store's asset id
    unsigned long long size;         // the size the file must have
    char file[256];                  // as written in the list
    char tag[32];                    // the language tag
    char path[1024];                 // absolute: the real path when installed
    int  installed;                  // a regular file of exactly `size` bytes
} kl_assetcfg_entry;

// One line. 1: an entry in *e (path and installed not yet set); 0: blank or a
// comment; -1: malformed, *why says how.
int kl_assetcfg_parse_line(const char *line, kl_assetcfg_entry *e, const char **why);

// Where the entry's file is, under `data_dir` (absolute), and whether it is
// installed. 0, or -1 with *why when the file is not inside data_dir.
int kl_assetcfg_resolve(kl_assetcfg_entry *e, const char *data_dir, const char **why);

// A whole list: the accepted entries into out[0..max), their count returned;
// -1 when the file cannot be read or data_dir is not absolute. Every rejected
// line is said on `log` (NULL: quiet).
int kl_assetcfg_load(const char *cfg_path, const char *data_dir,
                     kl_assetcfg_entry *out, int max, FILE *log);
