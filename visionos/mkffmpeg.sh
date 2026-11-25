#!/bin/bash
# Wrap KleptonFFmpeg (tools/ffmpeg_build.sh, `make ffmpeg`) as an XCFramework
# for the app to embed. gen_xcodeproj.py embeds it for Bigscreen Beta only, and
# runtime/media/kl_ffplay.c dlopens it by path, as ANGLE and MoltenVK are.
#
# Optional like MoltenVK: without it Bigscreen plays what AVFoundation opens
# (MP4, MOV, M4V) and says so in its log, so this is no failure.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
OUT="Frameworks"
NAME="KleptonFFmpeg"
for D in xros xrsim; do
  if [ ! -f "$ROOT/vendor/out/$D/$NAME.framework/$NAME" ]; then
    rm -rf "$OUT/$NAME.xcframework"
    echo "  $NAME: not built (vendor/out/$D) — run 'make ffmpeg' for MKV, AVI, WebM and more"
    exit 0
  fi
done
mkdir -p "$OUT"
rm -rf "$OUT/$NAME.xcframework"
xcodebuild -create-xcframework \
    -framework "$ROOT/vendor/out/xros/$NAME.framework" \
    -framework "$ROOT/vendor/out/xrsim/$NAME.framework" \
    -output "$OUT/$NAME.xcframework" > /dev/null
printf '  %-26s %s bytes (xros)\n' "$NAME.xcframework" \
    "$(stat -f%z "$ROOT/vendor/out/xros/$NAME.framework/$NAME")"
