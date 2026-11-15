#!/bin/bash
# mkmacho.sh — the Mach-O guest's mkguest.sh: wrap the target's Darwin dylibs
# (targets.py `libs`, kind "macho") into LIBRARY xcframeworks under
# Frameworks/<target>/, which gen_xcodeproj.py embeds (CodeSignOnCopy) like any
# other guest. Library and not .framework, because the game loads them by FILE
# name: LWJGL's -Dorg.lwjgl.librarypath=<Frameworks> looks for
# Frameworks/liblwjgl.dylib, and the dylibs' install names are
# @rpath/<name>.dylib already. Embedded, they land as Frameworks/<name>.dylib.
#
# Nothing is compiled here. The dylibs are INPUTS, from a release of
# vivecraft-visionos or from a build of it (tools/vivecraft_seed.py fills the
# folder):
#
#   visionos/prebuilt/<target>/Frameworks/<name>.dylib       device slices
#   visionos/prebuilt/<target>/Frameworks-sim/<name>.dylib   simulator slices
#
# KLEPTON_VIVECRAFT_PREBUILT names another folder with the same layout.
#
# Only the slices XROS_PLATFORMS names (run.sh exports it). A release carries
# device slices only, so a simulator build needs a folder seeded from a build
# tree (vivecraft_seed.py --tree) and stops here, saying so, without one.
set -euo pipefail
cd "$(dirname "$0")"
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
[ "$KLT_KIND" = macho ] || { echo "!! $KLT_NAME is not a Mach-O guest (kind $KLT_KIND)"; exit 1; }
PRE="${KLEPTON_VIVECRAFT_PREBUILT:-$PWD/prebuilt/$KLT_NAME}"
OUT="Frameworks/$KLT_NAME"

# Per slice: the input folder and what vtool calls its platform (older tools
# say XROS for VISIONOS).
SLICES=""
for P in ${XROS_PLATFORMS:-xros xrsim}; do
  case "$P" in
    xros)  SLICES="$SLICES xros" ;;
    xrsim) SLICES="$SLICES xrsim" ;;
    *)     echo "!! XROS_PLATFORMS: '$P' is not one of xros xrsim"; exit 1 ;;
  esac
done
for P in $SLICES; do
  case "$P" in xros) D="$PRE/Frameworks" ;; xrsim) D="$PRE/Frameworks-sim" ;; esac
  [ -d "$D" ] && continue
  if [ "$P" = xrsim ]; then
    echo "!! $D is missing — a release of the game has no simulator build."
    echo "   From a build tree of vivecraft-visionos: python3 tools/vivecraft_seed.py --tree <that tree>"
  else
    echo "!! $D is missing — the game's libraries are not built here."
    echo "   Fill it from a release: python3 tools/vivecraft_seed.py <vivecraft-visionos-x.y.z.ipa | unpacked .app>"
  fi
  exit 1
done

rm -rf "$OUT" "build/macho/$KLT_NAME"; mkdir -p "$OUT"
missing=0
for NAME in $KLT_LIBS; do
  ARGS=()
  for P in $SLICES; do
    case "$P" in
      xros)  SRC="$PRE/Frameworks/$NAME.dylib";     WANT='platform (VISIONOS|XROS)$' ;;
      xrsim) SRC="$PRE/Frameworks-sim/$NAME.dylib"; WANT='platform (VISIONOS|XROS)SIMULATOR$' ;;
    esac
    if [ ! -f "$SRC" ]; then echo "!! $SRC missing"; missing=1; continue; fi
    # What the platform can load, said here rather than by dyld at run time.
    if ! xcrun vtool -show-build "$SRC" 2>/dev/null | grep -qE "$WANT"; then
      echo "!! $SRC is not built for visionOS ($P)"; missing=1; continue
    fi
    # A copy per slice: the app build signs it, and the input folder stays as
    # it came. Cloned where the file system can (the image alone is 211 MB).
    D="build/macho/$KLT_NAME/$P"; mkdir -p "$D"
    cp -c "$SRC" "$D/$NAME.dylib" 2>/dev/null || cp "$SRC" "$D/$NAME.dylib"
    chmod u+w "$D/$NAME.dylib"
    id=$(otool -D "$D/$NAME.dylib" | tail -1)
    if [ "$id" != "@rpath/$NAME.dylib" ]; then
      install_name_tool -id "@rpath/$NAME.dylib" "$D/$NAME.dylib" 2>/dev/null
      echo "  $NAME ($P): install name $id -> @rpath/$NAME.dylib"
    fi
    ARGS+=(-library "$D/$NAME.dylib")
  done
  [ "${#ARGS[@]}" -gt 0 ] || continue
  xcodebuild -create-xcframework "${ARGS[@]}" -output "$OUT/$NAME.xcframework" > /dev/null
  printf '  %-26s %s bytes\n' "$NAME.xcframework" "$(stat -f%z "$D/$NAME.dylib")"
done
[ "$missing" = 0 ] || { echo "!! [mkmacho] libraries missing above (python3 tools/vivecraft_seed.py fills $PRE)"; exit 1; }
[ -f "$PRE/SOURCE.txt" ] && sed 's/^/  from: /' "$PRE/SOURCE.txt" | head -3
echo "[mkmacho] $KLT_NAME done -> $OUT/"
