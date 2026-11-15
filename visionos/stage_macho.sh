#!/bin/bash
# stage_macho.sh <simulator-udid> — a Mach-O guest's files into the SIMULATOR's
# container, for a build that does not carry them in its bundle.
#
# On a headset the game's files ride in the app (bundle_payload.sh) and the app
# lays its container out from them. A simulator build does not bundle them — the
# tree from a build of vivecraft-visionos still has its class folders loose,
# which is fine in a container and not in a bundle, and cloning 0.6 GB into
# every .app of the edit loop buys nothing — so they are put where the app
# looks, as that tree's own stage_native.sh does:
#
#   <container>/Documents/<tree>/launch.txt, baked.cp, root/
#
# from the input folder's payload/ (visionos/prebuilt/<target>/, or
# KLEPTON_VIVECRAFT_PREBUILT). The per-account game dirs the app itself made
# (root/inst-*/profiles: worlds, options) survive the replacement of root/.
#
# No account is staged: a Microsoft sign-in is made in the app, on the device it
# is for, and never copied from this Mac.
set -euo pipefail
cd "$(dirname "$0")"
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
[ "$KLT_KIND" = macho ] || { echo "!! $KLT_NAME is not a Mach-O guest (kind $KLT_KIND)"; exit 1; }
UDID="${1:?usage: stage_macho.sh <simulator-udid>}"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
PRE="${KLEPTON_VIVECRAFT_PREBUILT:-$PWD/prebuilt/$KLT_NAME}/payload"
for need in launch.txt baked.cp root; do
  [ -e "$PRE/$need" ] || { echo "!! $PRE has no $need (python3 tools/vivecraft_seed.py fills it)"; exit 1; }
done
CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null) || {
  echo "!! $BUNDLE_ID is not installed on $UDID — install the app first"; exit 1; }
DEST="$CONTAINER/Documents/$KLT_TREE"
mkdir -p "$DEST"
cp "$PRE/launch.txt" "$PRE/baked.cp" "$DEST/"
KEEP="$DEST/.profiles-keep"; rm -rf "$KEEP"; mkdir -p "$KEEP"
for p in "$DEST"/root/inst-*/profiles; do
  [ -d "$p" ] || continue
  i="$(basename "$(dirname "$p")")"; mv "$p" "$KEEP/$i"; echo "[stage] keeping root/$i/profiles"
done
rm -rf "$DEST/root"
cp -Rc "$PRE/root" "$DEST/root" 2>/dev/null || { rm -rf "$DEST/root"; cp -R "$PRE/root" "$DEST/root"; }
for k in "$KEEP"/*; do
  [ -d "$k" ] || continue
  i="$(basename "$k")"; mkdir -p "$DEST/root/$i"; mv "$k" "$DEST/root/$i/profiles"
done
rmdir "$KEEP"
echo "[stage] simulator $UDID -> $DEST ($(du -sh "$DEST" | cut -f1))"
