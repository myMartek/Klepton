#!/bin/bash
# Build the app TEMPLATE: the visionOS app with no game in it.
#
# Every other build here is for one title: the title's name is compiled in
# (KL_TARGET_DEFAULT), its translated libraries are embedded, its icon is in the
# asset catalog, and Xcode signs the result. That makes a Mac part of every
# install. The template is the part of such an app that does not depend on the
# title — the Swift app, the runtime, ANGLE, MoltenVK — built once, unsigned.
# Whoever finishes it for a game needs no compiler:
#
#   1. translate the game's libraries (tools/klepton_ld.c) into Frameworks/,
#      one <name>.framework each, as mkguest.sh lays them out;
#   2. write the game into Info.plist: KLTarget (and, for a title the runtime's
#      table does not have, KLTargetEntry / KLTargetObb / KLTargetKind — see
#      runtime/kl_target.h, kl_target_define), the values of targets.json,
#      bundle id, name, version;
#   3. sign it and install it.
#
# The output is build/klepton-template-<version>-<commit>.zip:
#   Payload/KleptonTemplate.app   the unsigned app
#   targets.json                  targets.py --template-data: the rows, and how
#                                 a store title outside the table gets its own
#   Klepton.entitlements          what an app built here is signed with
#   template.json                 which toolchain this is
#
# Not in it, because they cannot be made without Xcode: an icon and a start
# image (an asset catalog is compiled by actool). A finished app has neither
# until someone supplies a compiled catalog.
#
# Beat Saber's mods need a different app: the runtime with the mod bridge, the
# mod loader and the translated mods (run.sh with KLEPTON_SCOTLAND2=1). Where the
# toolchain carries the prebuilt mods of a game version (ports/scotland2/
# avp-profiles.json), a second archive is built for it,
# build/klepton-mods-<version code>-<version>-<commit>.zip: the same layout, the
# app with the mods in it, and mods.json — what the game's files must be, and the
# entries of its libil2cpp and libunity that get a gate before signing
# (ports/scotland2/tools/prepare_hooks.py). KLEPTON_TEMPLATE_MODS=0 leaves it out.
#
# KLEPTON_SURREAL_SUPPORT=1 builds the template with the Surreal Touch SDK, as
# for any app (it has to be fetched first: tools/surreal_sdk_fetch.sh).
set -euo pipefail
cd "$(dirname "$0")"

export XROS_PLATFORMS=xros
VERSION="$(git -C .. rev-list --count HEAD 2>/dev/null || python3 -c 'import json; print(json.load(open("../toolchain.json"))["version"])')"
COMMIT="$(git -C .. rev-parse --short=7 HEAD 2>/dev/null || python3 -c 'import json; print(json.load(open("../toolchain.json"))["commit"])')"
mkdir -p build

# build_app <derived data> <log>: generate the project from the environment and build it unsigned.
build_app() {
  # The team only has to be a value: nothing is signed here.
  KLEPTON_TEMPLATE=1 KLEPTON_TARGET=template KLEPTON_TEAM="${KLEPTON_TEAM:-0000000000}" \
  KLEPTON_START_SCREEN=1 KLEPTON_APP_VERSION=1.0 KLEPTON_APP_BUILD=1 \
  KLEPTON_SURREAL_SUPPORT="${KLEPTON_SURREAL_SUPPORT:-0}" \
      python3 gen_xcodeproj.py | head -1
  # Debug, like run.sh: the configuration every installed app has been.
  xcodebuild -project KleptonTemplate.xcodeproj -scheme KleptonTemplate -configuration Debug \
      -destination 'generic/platform=visionOS' -derivedDataPath "$1" \
      CODE_SIGNING_ALLOWED=NO build > "$2" 2>&1 \
      || { grep -E "error:|BUILD FAILED" "$2" | head -20; exit 1; }
  [ -d "$1/Build/Products/Debug-xros/KleptonTemplate.app" ] || { echo "!! the app was not built"; exit 1; }
}

# package <app> <archive> [mods.json]
package() {
  local stage; stage="$(mktemp -d)"
  mkdir -p "$stage/Payload"
  ditto "$1" "$stage/Payload/KleptonTemplate.app"
  python3 targets.py --template-data > "$stage/targets.json"
  cp Klepton.entitlements "$stage/Klepton.entitlements"
  printf '{"format": 1, "version": %s, "commit": "%s", "app": "Payload/KleptonTemplate.app", "surreal": %s, "mods": %s}\n' \
      "$VERSION" "$COMMIT" "$([ "${KLEPTON_SURREAL_SUPPORT:-0}" = 1 ] && echo true || echo false)" \
      "$([ -n "${3:-}" ] && echo true || echo false)" > "$stage/template.json"
  local names="Payload targets.json Klepton.entitlements template.json"
  if [ -n "${3:-}" ]; then cp "$3" "$stage/mods.json"; names="$names mods.json"; fi
  rm -f "$2"
  (cd "$stage" && zip -qry "$2" $names)
  rm -rf "$stage"
  echo "built $2 ($(stat -f %z "$2") bytes)"
}

./mkangle.sh > /dev/null
./mkmvk.sh > /dev/null

# The mods first, so that the runtime is left as every other build expects it.
if [ "${KLEPTON_TEMPLATE_MODS:-1}" = 1 ]; then
  python3 - <<'PY' > build/template-mod-profiles.txt
import json, os
port = '../ports/scotland2'
try:
    rows = json.load(open(port + '/avp-profiles.json'))['profiles']
except (OSError, ValueError, KeyError):
    rows = []
for row in rows:
    libs = os.path.join(port, row.get('prebuilt', ''), 'libs')
    if '..' not in row.get('prebuilt', '..') and os.path.isdir(libs) and any(n.endswith('.so') for n in os.listdir(libs)):
        print(row['target'], row['versionCode'])
PY
  while read -r MOD_TARGET MOD_CODE; do
    echo "[mods] $MOD_TARGET $MOD_CODE: runtime with the mod bridge, the mods, the app"
    (
      export KLEPTON_SCOTLAND2=1 SC2_BUILD_MODE=release SC2_TEST_SET=coremods
      (cd .. && make -s build/klepton-ld xros XROS_PLATFORMS=xros)
      SC2_OUT=$(python3 ../ports/scotland2/tools/stage_avp_mods.py --template --target "$MOD_TARGET" --version-code "$MOD_CODE")
      printf '%s\n' "$SC2_OUT" | grep -v '^SC2_GAME_PROFILE='
      export SC2_GAME_PROFILE="$(printf '%s\n' "$SC2_OUT" | sed -n 's/^SC2_GAME_PROFILE=//p')"
      [ -n "$SC2_GAME_PROFILE" ] || { echo "!! mods: no profile"; exit 1; }
      # Its own derived data: Xcode does not reliably notice a changed Swift condition.
      build_app build/dd-template-mods build/template-mods-xcodebuild.log
    )
    package build/dd-template-mods/Build/Products/Debug-xros/KleptonTemplate.app "$PWD/build/klepton-mods-$MOD_CODE-$VERSION-$COMMIT.zip" build/mods.json
  done < build/template-mod-profiles.txt
fi

echo "[1/3] runtime"
(cd .. && make -s build/klepton-ld xros XROS_PLATFORMS=xros)
echo "[2/3] project and build (unsigned)"
build_app build/dd-template build/template-xcodebuild.log
echo "[3/3] package"
package build/dd-template/Build/Products/Debug-xros/KleptonTemplate.app "$PWD/build/klepton-template-$VERSION-$COMMIT.zip"

# The archives are what is kept. The build folders are large and of no use to an installed toolchain, which builds
# its templates once; KLEPTON_TEMPLATE_KEEP_BUILD=1 keeps them for the next run while working on this script.
if [ "${KLEPTON_TEMPLATE_KEEP_BUILD:-0}" != 1 ]; then rm -rf build/dd-template build/dd-template-mods; fi
