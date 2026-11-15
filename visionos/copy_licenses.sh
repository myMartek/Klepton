#!/bin/sh
# Copy visionos/Licenses/<target>/ into the built .app, as <app>/Licenses.
# A build phase of a Mach-O guest's project only (gen_xcodeproj.py, MACHO).
#
# The game is compiled INTO the app: Minecraft's mods and libraries are one
# library, and a player cannot take a mod out again. Their licences (Vivecraft
# and others LGPL, Sodium Polyform Shield, ...) ask for the notices and texts to
# travel with the binary, so they are part of the bundle the way the icon is.
#
# visionOS has no Contents/ folder: UNLOCALIZED_RESOURCES_FOLDER_PATH is the
# .app itself (relative to TARGET_BUILD_DIR).
#
# Where the folder comes from (NOTICES.txt is the index, then the texts):
#
#   visionos/prebuilt/<target>/Licenses/   the notices the game's archive carried
#       (tools/vivecraft_seed.py; KLEPTON_VIVECRAFT_PREBUILT names another
#       folder). They describe exactly what was compiled into that game, so
#       they are the ones that go in when they are there.
#   visionos/Licenses/<target>/            the folder authored in this tree, for
#       an archive that carried none.
#
# This script only copies. A missing index fails the app build.
set -eu

TARGET="${KLEPTON_TARGET:?copy_licenses: KLEPTON_TARGET is not set (run.sh passes it to xcodebuild)}"
SRC="${KLEPTON_VIVECRAFT_PREBUILT:-${PROJECT_DIR}/prebuilt/$TARGET}/Licenses"
[ -f "$SRC/NOTICES.txt" ] || SRC="${PROJECT_DIR}/Licenses/$TARGET"
DEST="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/Licenses"

if [ ! -f "$SRC/NOTICES.txt" ]; then
  echo "error: $SRC/NOTICES.txt is missing. The app ships the licence texts of what is compiled into the game." >&2
  exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$SRC/." "$DEST/"
# Drop anything the copy picked up from a local editor.
find "$DEST" -name '.DS_Store' -delete
n=$(find "$DEST" -type f | wc -l | tr -d ' ')
echo "copy_licenses: $n files from $SRC -> $DEST"
