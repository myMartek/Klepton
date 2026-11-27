#!/bin/bash
# Build KleptonFFmpeg.framework: FFmpeg for the films AVFoundation cannot open
# (Bigscreen Beta lists MKV, AVI, WebM, TS/M2TS, WMV, OGV; until now only
# MP4/MOV/M4V played).
#
#   tools/ffmpeg_build.sh            both slices -> vendor/out/{xros,xrsim}/
#   tools/ffmpeg_build.sh xros       the device slice only
#   tools/ffmpeg_build.sh macos      a Mac build for the host tests
#                                    (build/ffmpeg/out-macos, never shipped)
#   `make ffmpeg` runs the first form.
#
# What is built: FFmpeg's demuxers and decoders for film files, libswscale
# (to BGRA) and libswresample (to s16), with VideoToolbox for H.264, HEVC, AV1,
# VP9 and MPEG-1/2/4. No encoders, muxers, filters, devices, network or
# programs. Licence: LGPL 2.1 or later. --enable-gpl and --enable-nonfree are
# never passed, so no GPL or non-free part is in it; configure prints the
# licence it settled on, and this script stops unless that is LGPL.
#
# One dylib: the static FFmpeg libraries and tools/ffmpeg/klff_shim.c linked
# together, exporting only klff_* (runtime/media/klff.h). The runtime dlopens
# it (kl_ffplay.c) and never links it, so it is a library that can be replaced
# by its own build, as the LGPL asks. The FFmpeg source is the release tarball
# below with one change, tools/ffmpeg/visionos.patch (VideoToolbox asked for
# OpenGL ES buffers, which visionOS does not have; it asks for Metal ones).
#
# Like vendor/out's ANGLE frameworks it is not committed: the tracked parts are
# this script, the pin and the shim. The toolchain package carries
# vendor/out/xros and vendor/out/xrsim whole, so a toolchain packed from a
# checkout that ran this has the framework.
set -euo pipefail
cd "$(dirname "$0")/.."

FF_VERSION="9.0.2"
FF_SHA256="8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
FF_URL="https://ffmpeg.org/releases/ffmpeg-$FF_VERSION.tar.xz"
# The release key the tarball's .asc is signed with (FFmpeg release signing
# key, FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8). Checked once by hand
# on 2026-11-25; the sha256 above pins what was checked.

DL="port-deps/sources/ffmpeg-dl"
TAR="$DL/ffmpeg-$FF_VERSION.tar.xz"
WORK="build/ffmpeg"
SRC="$WORK/ffmpeg-$FF_VERSION"
NAME="KleptonFFmpeg"

mkdir -p "$DL" "$WORK"
if [ ! -f "$TAR" ]; then
  echo "  [ffmpeg] downloading $FF_URL"
  curl -sSfL -o "$TAR.part" "$FF_URL"
  mv "$TAR.part" "$TAR"
fi
got=$(shasum -a 256 "$TAR" | cut -d' ' -f1)
[ "$got" = "$FF_SHA256" ] || { echo "!! $TAR: sha256 $got, expected $FF_SHA256"; exit 1; }
if [ ! -f "$SRC/.klepton-patched" ]; then
  rm -rf "$SRC"
  tar xf "$TAR" -C "$WORK"
  patch -s -d "$SRC" -p1 < tools/ffmpeg/visionos.patch
  touch "$SRC/.klepton-patched"
fi

DEMUXERS="matroska,mov,avi,mpegts,mpegps,asf,ogg,flv,m4v,mpegvideo,h264,hevc,wav,flac,mp3,aac,ac3,eac3,dts,truehd"
VIDEO="h264,hevc,av1,vp8,vp9,mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,h263,wmv1,wmv2,wmv3,vc1,theora,mjpeg,prores"
AUDIO="aac,aac_latm,ac3,eac3,dca,truehd,mlp,opus,vorbis,flac,mp3,mp2,mp1,alac,wmav1,wmav2,wmapro,wmalossless"
PCM="pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8,pcm_bluray,pcm_dvd"
PARSERS="h264,hevc,av1,vp8,vp9,mpegvideo,mpeg4video,vc1,aac,aac_latm,ac3,dca,mlp,opus,vorbis,flac,mpegaudio,h263,mjpeg"
HWACCELS="h264_videotoolbox,hevc_videotoolbox,av1_videotoolbox,vp9_videotoolbox,mpeg1_videotoolbox,mpeg2_videotoolbox,mpeg4_videotoolbox,h263_videotoolbox"

build_slice() {
  local D="$1" SDK TRIPLE PLAT
  case "$D" in
    xros)  SDK=xros;          TRIPLE=arm64-apple-xros1.0;           PLAT=XROS ;;
    xrsim) SDK=xrsimulator;   TRIPLE=arm64-apple-xros1.0-simulator; PLAT=XRSimulator ;;
    # The Mac, for the host tests only (KL_FFMPEG_LIB); never shipped.
    macos) SDK=macosx;        TRIPLE=arm64-apple-macos13.0;         PLAT=MacOSX ;;
    *) echo "!! unknown slice $D"; exit 1 ;;
  esac
  local SYSROOT; SYSROOT=$(xcrun -sdk "$SDK" --show-sdk-path)
  local CC; CC=$(xcrun -sdk "$SDK" -f clang)
  local B="$PWD/$WORK/build-$D" P="$PWD/$WORK/install-$D"
  rm -rf "$B" "$P"; mkdir -p "$B"
  echo "  [ffmpeg] configure $D"
  ( cd "$B" && "$OLDPWD/$SRC/configure" \
      --prefix="$P" \
      --enable-cross-compile --target-os=darwin --arch=aarch64 \
      --cc="$CC" --sysroot="$SYSROOT" \
      --extra-cflags="-target $TRIPLE -fvisibility=hidden" \
      --extra-ldflags="-target $TRIPLE" \
      --disable-programs --disable-doc --disable-debug --disable-network \
      --disable-autodetect --disable-everything \
      --disable-avdevice --disable-avfilter \
      --enable-static --disable-shared --enable-pic \
      --enable-videotoolbox --enable-zlib \
      --enable-swscale --enable-swresample \
      --enable-protocol=file \
      --enable-demuxer="$DEMUXERS" \
      --enable-decoder="$VIDEO,$AUDIO,$PCM" \
      --enable-parser="$PARSERS" \
      --enable-hwaccel="$HWACCELS" > "$B/configure.out" ) || {
        tail -20 "$B/configure.out"; tail -40 "$B/ffbuild/config.log" 2>/dev/null; exit 1; }
  grep -q "^License: LGPL" "$B/configure.out" || {
    echo "!! configure did not settle on an LGPL build:"; grep "^License" "$B/configure.out"; exit 1; }
  echo "  [ffmpeg] build $D ($(grep '^License' "$B/configure.out"))"
  make -C "$B" -j"$(sysctl -n hw.ncpu)" > "$B/make.out" 2>&1 || { tail -30 "$B/make.out"; exit 1; }
  make -C "$B" install > /dev/null 2>&1

  # The shim and the libraries as one dylib, exporting klff_* only.
  local FW="vendor/out/$D/$NAME.framework"
  [ "$D" = macos ] && FW="$WORK/out-macos/$NAME.framework"
  rm -rf "$FW"; mkdir -p "$FW"
  printf '_klff_open\n_klff_open_io\n_klff_close\n_klff_info\n_klff_video_next\n_klff_audio_next\n_klff_seek\n_klff_version\n_klff_audio_tracks\n_klff_audio_select\n' \
      > "$B/exports.txt"
  "$CC" -target "$TRIPLE" -isysroot "$SYSROOT" -O2 -fPIC -I"$P/include" \
      -c tools/ffmpeg/klff_shim.c -o "$B/klff_shim.o"
  "$CC" -target "$TRIPLE" -isysroot "$SYSROOT" -dynamiclib \
      -install_name "@rpath/$NAME.framework/$NAME" \
      -exported_symbols_list "$B/exports.txt" -dead_strip \
      -o "$FW/$NAME" "$B/klff_shim.o" \
      "$P/lib/libavformat.a" "$P/lib/libavcodec.a" "$P/lib/libswscale.a" \
      "$P/lib/libswresample.a" "$P/lib/libavutil.a" \
      -framework CoreFoundation -framework CoreVideo -framework CoreMedia \
      -framework VideoToolbox -framework CoreServices -lz
  strip -x "$FW/$NAME"
  cat > "$FW/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>org.ffmpeg.klepton</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$FF_VERSION</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$PLAT</string></array>
  <key>MinimumOSVersion</key><string>1.0</string>
</dict>
</plist>
PLIST
  # The licence beside the binary, and what was configured, for anyone who
  # wants to rebuild it.
  cp "$SRC/COPYING.LGPLv2.1" "$FW/COPYING.LGPLv2.1"
  { echo "FFmpeg $FF_VERSION ($FF_URL, sha256 $FF_SHA256)"; echo "built by tools/ffmpeg_build.sh";
    grep -m1 "^License" "$B/configure.out"; } > "$FW/BUILD.txt"
  printf '  [ffmpeg] %-34s %s bytes\n' "$FW" "$(stat -f%z "$FW/$NAME")"
}

SLICES="${1:-xros xrsim}"
for D in $SLICES; do build_slice "$D"; done
echo "[ffmpeg] done"
