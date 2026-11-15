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
# A Mach-O guest (targets.py kind "macho": Minecraft, target vivecraft) needs a
# third kind of app. It is not made of an APK's libraries, its app compiles
# sources of its own (Sources/Minecraft/, KL_MACHO_GUEST) and is a Release build
# (its ownership gate, see run.sh), so the template above cannot become it. For
# each such target an archive of its own is built, after the others and without
# touching them, build/klepton-macho-<target>-<version>-<commit>.zip:
#   Payload/<Product>.app   that target's app, unsigned, everything compiled in
#                           and NOTHING of the game: none of its libraries in
#                           Frameworks/, no klepton-payload/
#   Klepton.entitlements    as above
#   template.json           as above, plus "macho": "<target>"
#   macho.json              what a finisher adds, from the game's archive (a zip
#                           in the shape of an IPA, tools/vivecraft_seed.py):
#                           each library and where it goes, the payload folder,
#                           the licence notices, the Info.plist values to write,
#                           what to sign, and where the game's own icon is
# The build needs no input folder. Where one is seeded (or KLEPTON_MACHO_IPA
# names the archive to seed it from, as for run.sh), the template takes the
# game's icon and the archive's licence notices from it; macho.json says whether
# an icon is compiled in. KLEPTON_TEMPLATE_MACHO=0 leaves the archive out.
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

# A Mach-O guest's own template (see the head of this file). After the archives
# above are written: nothing here is read by them, and the runtime is the one
# they were built with.
if [ "${KLEPTON_TEMPLATE_MACHO:-1}" = 1 ]; then
  python3 targets.py --macho-data > build/template-macho-data.json
  python3 - <<'PY' > build/template-macho-targets.txt
import json
for name, row in sorted(json.load(open('build/template-macho-data.json'))['targets'].items()):
    print(name, row['product'])
PY
  while read -r MACHO_TARGET MACHO_PRODUCT; do
    echo "[macho] $MACHO_TARGET: the app without the game (unsigned, Release)"
    if [ -n "${KLEPTON_MACHO_IPA:-}" ]; then
      python3 ../tools/vivecraft_seed.py --if-changed "$KLEPTON_MACHO_IPA" | sed 's/^/  /'
    fi
    KLEPTON_MACHO_TEMPLATE=1 KLEPTON_TARGET="$MACHO_TARGET" KLEPTON_TEAM="${KLEPTON_TEAM:-0000000000}" \
    KLEPTON_START_SCREEN=1 KLEPTON_APP_VERSION=1.0 KLEPTON_APP_BUILD=1 \
    KLEPTON_SURREAL_SUPPORT="${KLEPTON_SURREAL_SUPPORT:-0}" \
        python3 gen_xcodeproj.py | head -1
    # Release and never Debug: the only configuration this target is put on a
    # headset in (run.sh), and the one in which its ownership gate cannot be
    # compiled out. Its own derived data, like the mods.
    MACHO_LOG="build/template-macho-$MACHO_TARGET-xcodebuild.log"
    xcodebuild -project "$MACHO_PRODUCT.xcodeproj" -scheme "$MACHO_PRODUCT" -configuration Release \
        -destination 'generic/platform=visionOS' -derivedDataPath "build/dd-template-macho-$MACHO_TARGET" \
        CODE_SIGNING_ALLOWED=NO build < /dev/null > "$MACHO_LOG" 2>&1 \
        || { grep -E "error:|BUILD FAILED" "$MACHO_LOG" | head -20; exit 1; }
    MACHO_APP="build/dd-template-macho-$MACHO_TARGET/Build/Products/Release-xros/$MACHO_PRODUCT.app"
    [ -d "$MACHO_APP" ] || { echo "!! the app was not built"; exit 1; }
    # The point of it: nothing of the game. A library or a payload in here would
    # be somebody's files in an archive that is passed on.
    if [ -e "$MACHO_APP/klepton-payload" ] || ls "$MACHO_APP"/Frameworks/*.dylib > /dev/null 2>&1; then
      echo "!! $MACHO_APP holds game files - not packed"; exit 1
    fi
    [ -f "$MACHO_APP/Licenses/NOTICES.txt" ] || { echo "!! $MACHO_APP has no Licenses/NOTICES.txt"; exit 1; }

    MACHO_STAGE="$(mktemp -d)"
    mkdir -p "$MACHO_STAGE/Payload"
    ditto "$MACHO_APP" "$MACHO_STAGE/Payload/$MACHO_PRODUCT.app"
    cp Klepton.entitlements "$MACHO_STAGE/Klepton.entitlements"
    printf '{"format": 1, "version": %s, "commit": "%s", "app": "Payload/%s.app", "surreal": %s, "mods": false, "macho": "%s"}\n' \
        "$VERSION" "$COMMIT" "$MACHO_PRODUCT" \
        "$([ "${KLEPTON_SURREAL_SUPPORT:-0}" = 1 ] && echo true || echo false)" "$MACHO_TARGET" > "$MACHO_STAGE/template.json"
    python3 ../tools/vivecraft_seed.py --known > build/template-macho-known.json
    python3 - "$MACHO_TARGET" "$VERSION" "$COMMIT" "$MACHO_STAGE/Payload/$MACHO_PRODUCT.app" <<'PY' > "$MACHO_STAGE/macho.json"
import json, os, plistlib, sys
sys.path.insert(0, '.')
import macho_art
target, version, commit, app = sys.argv[1:5]
row = json.load(open('build/template-macho-data.json'))['targets'][target]
info = plistlib.load(open(os.path.join(app, 'Info.plist'), 'rb'))
icon = info.get('CFBundleIconName', '')
row['licenses']['inTemplate'] = sum(len(fs) for _, _, fs in os.walk(os.path.join(app, 'Licenses')))
print(json.dumps({
    'format': 1, 'kind': 'macho', 'target': target, 'version': int(version), 'commit': commit,
    'app': 'Payload/%s.app' % row['product'], 'executable': info['CFBundleExecutable'],
    'configuration': 'Release', 'display': row['display'],
    # The game's archive: one app folder in it, and the published ones by
    # sha256 for a name only (an archive is taken on what is in it).
    'archive': {'app': 'Payload/*.app', 'published': json.load(open('build/template-macho-known.json'))},
    'libraries': row['libraries'], 'payload': row['payload'], 'licenses': row['licenses'],
    # Info.plist: the row's values are in it already (info.row, to check
    # against); a finisher writes who the app is. {bundleId}, {team}, {version}
    # and {build} are the finisher's.
    'info': {'file': 'Info.plist',
             'write': {'CFBundleIdentifier': '{bundleId}', 'KLSharedKeychainGroup': '{team}.avpplay.shared',
                       'CFBundleShortVersionString': '{version}', 'CFBundleVersion': '{build}'},
             'row': row['info']},
    'entitlements': 'Klepton.entitlements',
    # Inside out: every library that was added, the frameworks, then the app.
    'sign': ['Frameworks/*.dylib', 'Frameworks/*.framework', '.'],
    # An icon compiled into the template (Assets.car) needs nothing. Without
    # one, the game's own is an asset object the payload's index names.
    'icon': {'compiled': os.path.isfile(os.path.join(app, 'Assets.car')) and icon not in ('', 'AppIcon'),
             'name': icon,
             'game': {'index': 'klepton-payload/root/inst-*/assets/indexes/*.json',
                      'names': [n for n, _ in macho_art.WANTED],
                      'object': 'klepton-payload/root/inst-*/assets/objects/{hash:2}/{hash}'}},
}, indent=1, sort_keys=True))
PY
    MACHO_ZIP="$PWD/build/klepton-macho-$MACHO_TARGET-$VERSION-$COMMIT.zip"
    rm -f "$MACHO_ZIP"
    (cd "$MACHO_STAGE" && zip -qry "$MACHO_ZIP" Payload Klepton.entitlements template.json macho.json)
    rm -rf "$MACHO_STAGE"
    echo "built $MACHO_ZIP ($(stat -f %z "$MACHO_ZIP") bytes)"
    if [ "${KLEPTON_TEMPLATE_KEEP_BUILD:-0}" != 1 ]; then rm -rf "build/dd-template-macho-$MACHO_TARGET"; fi
  done < build/template-macho-targets.txt
fi

# The archives are what is kept. The build folders are large and of no use to an installed toolchain, which builds
# its templates once; KLEPTON_TEMPLATE_KEEP_BUILD=1 keeps them for the next run while working on this script.
if [ "${KLEPTON_TEMPLATE_KEEP_BUILD:-0}" != 1 ]; then rm -rf build/dd-template build/dd-template-mods; fi
