#!/bin/bash
# Put the game's files into the .app, at <app>/klepton-payload/. A build phase
# of a Mach-O guest's project only (gen_xcodeproj.py, MACHO).
#
# Minecraft's jars, its Fabric game dir and its assets (~0.6 GB) are not staged
# into the headset from a Mac the way an APK's assets are: the app has to start
# with an empty data container (an install through AVP Play or SideStore has no
# file copy after it). So the tree rides in the bundle, and on first launch
# KleptonPayload.swift lays Documents out from it: the read-only half (jars,
# assets) as symlinks back into the bundle, the writable defaults (options,
# config) as copies.
#
# The tree is an INPUT, taken out of a release of vivecraft-visionos by
# tools/vivecraft_seed.py and kept in
#
#   visionos/prebuilt/<target>/payload/     launch.txt baked.cp root/ tree
#                                           payload-version
#
# (KLEPTON_VIVECRAFT_PREBUILT names another folder). It goes into the app AS IT
# IS, with its own payload-version. That file is the identity KleptonPayload
# compares: an update of the app that carries the same payload leaves the
# player's container alone, and one that carries another refreshes the symlinks
# without touching the options and worlds the player has made. Recomputing it
# here would make every build look like a new game to an installed copy.
#
# What is checked before the copy, because a payload that fails these fails
# later and far away:
#
#  - no personal data: launch.txt still holds the OFFLINE placeholders (@USER@,
#    the nil uuid, token 0). The account is put in at run time, in memory.
#  - no loose class file and no Mach-O: a Java .class starts with 0xCAFEBABE,
#    which is also the fat Mach-O magic, and the installer parses every file in
#    a bundle by its magic ("FAT slice bounds exceed file size"); a Mach-O here
#    would be unsigned code. The release packs its class folders into jars.
set -euo pipefail

DEST="${TARGET_BUILD_DIR:?}/${UNLOCALIZED_RESOURCES_FOLDER_PATH:?}/klepton-payload"
cd "${PROJECT_DIR:?}"

# Not for the simulator: there the files are staged into the container
# (stage_macho.sh, from run.sh), because a tree out of a build of
# vivecraft-visionos has its class folders loose, and 0.6 GB cloned into every
# .app of the edit loop buys nothing. The app finds no payload in its bundle and
# takes the staged container as it is (KleptonPayload.swift).
if [ "${PLATFORM_NAME:-}" = xrsimulator ]; then
  rm -rf "$DEST"
  echo "bundle_payload: simulator build — the game's files are staged into the container instead"
  exit 0
fi
TARGET="${KLEPTON_TARGET:?bundle_payload: KLEPTON_TARGET is not set (run.sh passes it to xcodebuild)}"
PRE="${KLEPTON_VIVECRAFT_PREBUILT:-$PWD/prebuilt/$TARGET}/payload"

[ -d "$PRE" ] || {
  echo "error: bundle_payload: $PRE is missing. The game's files are not built here: python3 tools/vivecraft_seed.py <release .ipa | unpacked .app>" >&2
  exit 1; }
for need in launch.txt baked.cp root payload-version tree; do
  [ -e "$PRE/$need" ] || { echo "error: bundle_payload: $PRE has no $need" >&2; exit 1; }
done
WANT_TREE="$(python3 targets.py "$TARGET" tree)"
[ "$(head -1 "$PRE/tree")" = "$WANT_TREE" ] || {
  echo "error: bundle_payload: $PRE is laid out for '$(head -1 "$PRE/tree")', this target's tree is '$WANT_TREE'" >&2
  exit 1; }

# Skipped when the app already holds this payload: the scan and the copy are
# 5,000 files, and the edit loop changes the app's code, not the game's.
VER="$(head -1 "$PRE/payload-version")"
if [ -f "$DEST/payload-version" ] && [ "$(head -1 "$DEST/payload-version")" = "$VER" ] \
   && [ "$(find "$PRE" -type f | wc -l)" = "$(find "$DEST" -type f | wc -l)" ]; then
  echo "bundle_payload: $TARGET payload ${VER:0:12} is already in the app"
  exit 0
fi

python3 - "$PRE" <<'PY'
import os, sys
pre = sys.argv[1]
bad = []
lines = open(os.path.join(pre, "launch.txt"), encoding="utf-8", errors="replace").read().splitlines()
nxt = {}
for i, line in enumerate(lines):
    if i + 1 < len(lines) and line.strip() in ("--username", "--uuid", "--accessToken"):
        nxt[line.strip()] = lines[i + 1].strip()
if nxt.get("--username") not in ("@USER@", "Player"):
    bad.append("launch.txt --username is not the offline placeholder")
if nxt.get("--uuid") != "00000000-0000-0000-0000-000000000000":
    bad.append("launch.txt --uuid is not the offline placeholder")
if nxt.get("--accessToken") != "0":
    bad.append("launch.txt --accessToken is not the offline placeholder")
MAGIC = {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
         b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce"}
for r, _, fs in os.walk(pre):
    for f in fs:
        p = os.path.join(r, f)
        if os.path.islink(p):
            continue
        with open(p, "rb") as fh:
            if fh.read(4) in MAGIC:
                bad.append("class or Mach-O file: " + os.path.relpath(p, pre))
if bad:
    sys.stderr.write("error: bundle_payload: this payload would not install or carries an account:\n")
    for b in bad[:40]:
        sys.stderr.write("  " + b + "\n")
    sys.exit(1)
PY

rm -rf "$DEST"
# Cloned where the file system can; a plain copy otherwise.
if ! cp -Rc "$PRE" "$DEST" 2>/dev/null; then
  rm -rf "$DEST"
  cp -R "$PRE" "$DEST"
fi
find "$DEST" -name '.DS_Store' -delete
echo "bundle_payload: $TARGET $(du -sh "$DEST" | awk '{print $1}') version ${VER:0:12} -> $DEST"
