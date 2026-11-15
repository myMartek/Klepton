# Building Klepton

macOS on Apple Silicon only. Everything here assumes an arm64 host — the project
runs guest ARM64 code natively, so there is no x86_64 path and never will be.

The build is a plain `Makefile` plus a few shell scripts. There is no CMake, no
package manager for the project itself, and no git submodules.

---

## TL;DR

```bash
# host deps
brew install pkg-config sdl3 apktool

# put the APKs you supply in the repo root, then unpack them
apktool d -f -o <target> <target>.apk

make check                               # regression sweep, builds everything in the process
make angle-all                           # pull, patch and build ANGLE (slow)
make mvk                                 # MoltenVK, for the Vulkan targets

./build_run_viewer.sh <target>           # build and run the macOS viewer for a target
./build_run_vpro.sh <target>             # build and run the visionOS app bundle for a target
```

`make check` needs no ANGLE, no NDK and no headset. Everything past it does.

---

## Host requirements

Known-good versions are what this tree is developed against, not minimums —
where a version is load-bearing it says so.

| dependency | known-good | needed for | how to get it |
|---|---|---|---|
| macOS on Apple Silicon | 26.5.1 (Darwin 25.5) | everything | — |
| Xcode + Command Line Tools | 26.0.1 (17A400), Apple clang 17.0.0 | everything | App Store; `xcode-select --install` |
| macOS SDK | 26.0 | host builds | ships with Xcode |
| xrOS + xrSimulator SDKs | 26.0 | `make xros`, visionOS app | Xcode → Settings → Components → visionOS |
| Python 3 | 3.14.6 (3.8+ is plenty) | `tools/*.py`, `visionos/gen_xcodeproj.py` | ships with Xcode CLT, or `brew install python` |
| `pkg-config` | any | locating SDL3 | `brew install pkg-config` |
| SDL3 | 3.4.12 | macOS interactive viewer (**optional**) | `brew install sdl3` |
| apktool | 2.11.1 | unpacking the guest APKs | `brew install apktool` |
| ANGLE (vendored, patched) | ANGLE `25e7211`, depot_tools `6afa997` | any real-GL path | `make angle-all` |
| MoltenVK (vendored, prebuilt) | `v1.4.2` | any Vulkan path (BONELAB, Open Brush, RE4) | `make mvk` |
| Android NDK | r25c (`25.2.9519653`) | `make guest` only (**optional**) | Android Studio SDK Manager |
| Vision Pro + Apple Developer account | — | device runs only | — |

visionOS signing is auto-detected from your keychain's `Apple Development`
certificate; override with `KLEPTON_TEAM=<teamid>`. The Simulator needs neither.

The bundle id is set to `$USER.dev.klepton.target.<target>` in order to avoid collisions.
Set `KLEPTON_BUNDLE_SCOPE` to use something else instead of `$USER`, or `KLEPTON_BUNDLE_ID` 
to override the the whole id.

The name under the icon comes from the target's row in `visionos/targets.py`.
`KLEPTON_DISPLAY_OVERRIDE` replaces it (for every target, generic ones included),
and `KLEPTON_DISPLAY_SUFFIX` is appended to whatever name that leaves. Together
with `KLEPTON_BUNDLE_ID`, that installs a test copy next to the one you already have:

```sh
KLEPTON_BUNDLE_ID=$USER.dev.klepton.target.beatsaber140.test \
KLEPTON_DISPLAY_SUFFIX=" (Test)" \
  ./build_run_vpro.sh beatsaber140      # -> "Beat Saber (Test)"
```

If neither is set, or both are empty, the name stays exactly as it is now.

---

## Guest applications

**Do not ask me where to get these.** You supply them.

| target | APK | unpacked to | graphics | also needs |
|---|---|---|---|---|
| Beat Saber 1.40.8 | `beatsaber.apk` | `beatsaber/` | GLES (ANGLE) | one OBB, 1.3 GB |
| SUPERHOT VR 1.161 | `superhot.apk` | `superhot/` | GLES (ANGLE) | — |
| BONELAB 1.2974 | `bonelab.apk` | `bonelab/` | Vulkan (MoltenVK) | two OBBs, 6.3 GB |
| VRChat 2026.2.3 | `vrchat.apk` | `vrchat/` | GLES (ANGLE) | — |
| Open Brush 2.30.0 | `openbrush.apk` | `openbrush/` | Vulkan (MoltenVK) | — |
| RE4 VR 2.3 | `re4.apk` | `re4/` | Vulkan (MoltenVK) | two OBBs, 7.9 GB |
| JKXR 1.1.28 | `jkxr.apk` | `jkxr/` | GLES (ANGLE, via gl4es) | retail game pk3s |
| Steam Link 2.0.20 (VR) | `steamlink-vr.apk` | `steamlink-vr/` | GLES (ANGLE) | — |
| Steam Link (flat) | `steamlink-android.apk` | `steamlink-android/` | — | `make x18-slink` only |

Unpack with **apktool**:

```bash
apktool d -f -o beatsaber    beatsaber.apk
apktool d -f -o superhot     superhot.apk
apktool d -f -o steamlink-vr steamlink-vr.apk
# ... etc
```

**Keep the `.apk` files themselves.** `getPackageCodePath()` hands Unity the APK path
and Unity opens it as a zip.

Additional targets can be defined in `visionos/targets.py`; run `make targets`
afterwards to regenerate the runtime's copy of the table
(`runtime/kl_target_table.h`, which is committed).

To build a target:

```bash
make check TARGET=<target>          # the host gates
./build/m_boot <target>             # host run
./build_run_vpro.sh <target>        # on the headset
```

Steam Link is the only target with separate mains of its own, still working on unifying that.

### OBBs (split application binaries)

Some targets ship their data beside the APK rather than inside it. The runtime
reads the OBB out of the guest's userdata directory:

```
~/Library/Application Support/Klepton/userdata/<target>/<obb path>/
```

| target | OBB path under userdata | files |
|---|---|---|
| `beatsaber` | `obb/` | `main.1716.com.beatgames.beatsaber.obb` |
| `bonelab` | `obb/` | `main.2974` + `patch.2974`, `.com.StressLevelZero.BONELAB.obb` |
| `re4` | `Android/obb/com.Armature.VR4/` | `main.203` + `patch.203`, `.com.Armature.VR4.obb` |

---

## Minecraft (the `vivecraft` target)

Minecraft: Java Edition with Vivecraft is the one target that is not made from
an APK. The game is rebelancap's `vivecraft-visionos`: Minecraft, its mods and
Vivecraft compiled ahead of time into `libminecraft.dylib`, with seventeen more
Darwin libraries beside it (LWJGL, OpenAL, the GL translator, the OpenXR loader
shim, the voice codecs). In `visionos/targets.py` its kind is `macho`; the kind
`native` is an Android NativeActivity and has nothing to do with it.

None of that is built here. The compiled game and its files (jars, Minecraft's
assets, the Fabric game dir; about 0.9 GB) are inputs, taken out of an archive
of the game: a zip in the shape of an IPA. A release IPA of `vivecraft-visionos`
is one; so is what a build of that project on one's own Mac leaves, the game's
parts with nothing around them:

```
Payload/<Name>.app/Frameworks/<name>.dylib   the libraries the target's row names, and no others
Payload/<Name>.app/klepton-payload/          launch.txt baked.cp payload-version tree root/inst-<version>/
Payload/<Name>.app/Licenses/  or  Licenses/  optional: the notices of what is compiled in (NOTICES.txt + texts)
```

```bash
python3 tools/vivecraft_seed.py /path/to/vivecraft-visionos-1.0.3.ipa   # or a parts archive, or an unpacked .app
```

A locally built archive is another file on every Mac, so an archive is taken on
what is in it and not on its hash: exactly the row's libraries, each an arm64
dynamic library built for visionOS devices; the payload's five parts, laid out
for this target, with no account in `launch.txt` and no loose class or Mach-O
file; licence files that are texts. Each check that fails is one line, and then
nothing is taken. (The script's table of sha256 values only gives a published
archive its name.) It fills `visionos/prebuilt/vivecraft/` (git-ignored;
`KLEPTON_VIVECRAFT_PREBUILT=<folder>` names another one, for the script and for
the build):

```
visionos/prebuilt/vivecraft/Frameworks/<name>.dylib   the 18 libraries, device slices
visionos/prebuilt/vivecraft/payload/                  launch.txt baked.cp root/ tree payload-version
visionos/prebuilt/vivecraft/Licenses/                 the archive's notices, when it carried them
visionos/prebuilt/vivecraft/SOURCE.txt                which archive they came from (name or sha256)
```

Then the app, for a headset only (an archive has no simulator build of the game):

```bash
KLEPTON_TARGET=vivecraft KLEPTON_TEAM=<team id> KL_NO_INSTALL=1 visionos/run.sh device
```

An installer does both in one step by handing the build its archive:

```bash
KLEPTON_MACHO_IPA=/absolute/path/to/the/archive KLEPTON_TARGET=vivecraft ... visionos/run.sh device
```

`run.sh` then calls the script with `--if-changed` before it builds: a folder
that already holds what came out of that file (same sha256) is left alone,
another file fills it anew, and an archive that fails a check stops the build
with the script's lines. The path must be absolute. Without the variable the
folder is used as it is. The variable is the build's alone: `run.sh` forwards
`KL_*`, `MTL_*` and `METAL_*` into the app, and this name is none of them.

`KL_NO_INSTALL=1` builds the signed `.app` and stops without looking for a
headset; without it the app is installed and launched as for any target.
`visionos/mkmacho.sh` wraps the libraries for embedding, the "Bundle game
payload" build phase copies the payload into the app as it is (with the
archive's own `payload-version`, so an update that carries the same game leaves
a player's container alone), and "Copy licence notices" puts the notices into
the app as `Licenses/`: the archive's when it carried them, else
`visionos/Licenses/vivecraft/`. The build is the
Release configuration unless `KL_XCODE_CONFIG=Debug`: the game does not start
until an account that owns Minecraft: Java Edition has signed in on the device,
the only way to compile that gate out is the Swift condition
`KL_OWNERSHIP_GATE_OFF`, and a build without `DEBUG` that defines it does not
compile. No environment variable switches the gate off.

Because the payload goes in unchanged, what a new player starts with is set by
the app, whatever the archive ships, at the moment it makes that player's
`options.txt` (`MCOptionDefaults` in
`visionos/Sources/Minecraft/KleptonMCProfiles.swift`): render distance 5 and
simulation distance 8 (the 1.0.3 release ships 12 and 12), and
`graphicsPreset:"custom"` where the file names another preset, because the game
puts a preset's own distances back at its first start. An `options.txt` that
exists is never opened for it, and no other line is touched.

The icon is the installer's when it left one in `visionos/Assets.xcassets`
(`AppIcon-vivecraft.solidimagestack`, as for every target). Without one the
build takes the game's own: Minecraft's icon is an asset object the payload's
index names (`icons/minecraft.icns`, 1024 px), and `visionos/macho_art.py`
makes the visionOS icon from it at project-generation time, into
`visionos/build/gameart/vivecraft/` (the game's picture over a plain ground;
nothing of it is part of this tree). No payload seeded and no stack: iconless.

`make swiftcheck-macho` typechecks this target's Swift (the shared sources with
`KL_MACHO_GUEST`, plus `visionos/Sources/Minecraft/`) without Xcode.

### Installing it without a compiler

`visionos/mktemplate.sh` (its head describes the archives) builds a third one
for this kind, `visionos/build/klepton-macho-vivecraft-<version>-<commit>.zip`:
the vivecraft app, unsigned, Release, with everything compiled in and nothing
of the game — no library of its eighteen in `Frameworks/`, no `klepton-payload/`.
The plain template cannot become this app (other sources, another
configuration), and it is built exactly as before. `KLEPTON_TEMPLATE_MACHO=0`
leaves the new archive out.

```
Payload/KleptonVivecraft.app   the app without the game
Klepton.entitlements           what an app built here is signed with
template.json                  which toolchain this is; "macho": "vivecraft"
macho.json                     what to add, and where
```

Finishing it takes the game's archive (the same file `KLEPTON_MACHO_IPA`
names) and no compiler. `macho.json` (`visionos/targets.py --macho-data` plus
what the build knows) says, with `from` inside the archive's one
`Payload/<Name>.app` and `to` inside the template's app:

- `libraries`: `names` (the eighteen), each from `Frameworks/{name}.dylib` to
  `Frameworks/{name}.dylib`; arm64, built for visionOS devices, install name
  `@rpath/{name}.dylib` (an archive's libraries have it).
- `payload`: the folder `klepton-payload`, to `klepton-payload`, as it is; it
  must hold the names in `requires` and be laid out for `tree`.
- `licenses`: the template carries a set (`inTemplate` files). An archive with
  its own (`from`, a folder that has the `index`) replaces the folder `to`.
- `info.write`: the four Info.plist values that say whose app it is —
  `CFBundleIdentifier`, `KLSharedKeychainGroup` (`{team}.avpplay.shared`),
  `CFBundleShortVersionString`, `CFBundleVersion`. Every other value is in the
  template already; `info.row` lists the row's, to check against.
- `entitlements`: the file to sign the app with, `$(AppIdentifierPrefix)` and
  `$(PRODUCT_BUNDLE_IDENTIFIER)` filled in, plus the application and team
  identifiers every signed app has.
- `sign`: what to sign, inside out — the added libraries, the frameworks, the
  app.
- `icon`: `compiled` says whether the template has one (it has when an
  installer's stack or a seeded payload was there at build time; set
  `KLEPTON_MACHO_IPA` for `mktemplate.sh` as for `run.sh`). Without one, `game`
  says where the game's own is in the payload: an asset index, the names to look
  for in it, and the object file by its hash (`{hash:2}` is the hash's first two
  characters).
- `archive.published`: sha256 → name of the published archives, for display
  only. An archive is taken on what is in it (the checks `tools/vivecraft_seed.py`
  makes), never on its hash.

A template finished that way from the 1.0.3 IPA on a Mac and the device build
with the same options (`KLEPTON_START_SCREEN=1 KLEPTON_SURREAL_SUPPORT=0`) had
the same list of files, the same Info.plist, the same entitlements and the same
signed libraries (equal code hashes), and both verified. The two program files
are two builds of the same sources: equal in size once their debug symbols are
stripped, different in the debug map's paths, the UUID and the order of the
Objective-C message stubs.

## ANGLE

```bash
make angle-all       # pull + patch + all three slices, from nothing
```

ANGLE requires a lot of disk space to build, budget ~20 GiB total for both the
source checkout and build intermediates:

| | |
|---|---|
| sources (`third_party`, `buildtools`, `depot_tools`, `.git`) | ~12 GiB |
| `out/Debug` — host slice | 4.4 GiB |
| `out/ios` + `out/ios-sim` | ~1.4 GiB |
| `out/xros` + `out/xrsim` — the retargeted frameworks | 24 MiB |
| **`vendor/` total** | **~18 GiB** |


`vtool` is used to retarget the iOS builds to visionOS.

---

## MoltenVK

```bash
make mvk             # pull the pinned prebuilt + stage all three slices
```

---

## Build Targets

Everything below builds what it needs first. `<target>` is any name from
`python3 visionos/targets.py --list` and defaults to Beat Saber.

| want | command |
|---|---|
| a guest on macOS, in a window | `./build_run_viewer.sh <target>` |
| a guest on macOS, headless | `./build/m_boot <target>` |
| a guest on the visionOS Simulator | `KLEPTON_TARGET=<target> visionos/run.sh` |
| a guest on Vision Pro | `./build_run_vpro.sh <target>` |
| a guest on macOS, with a summary | `./build_run_host.sh <target>` |
| Steam Link config UI, macOS | `./build_run_host.sh steamlink-vr --shell --view` |
| any target's shim work list, in seconds | `./build_run_host.sh <target> --gap` |
| visionOS build gate, no device needed | `make xros` |

### Icon and start picture

The app's icon and the picture behind its start screen are whatever the
installer left in `visionos/Assets.xcassets` for the target
(`AppIcon-<target>.solidimagestack`, `StartHero-<target>.imageset`; both
gitignored). A target whose game is not an APK and names a Steam app instead
(`steam_app` in `targets.py`, today `alyx`) gets both from Steam's public store
artwork at project-generation time (`visionos/steam_art.py`, into
`visionos/build/steamart/<target>/`, kept for rebuilds without a network).
`KL_NO_STEAM_ART=1` skips that, and a fetch that fails just leaves the build
iconless, as it was before.

---

## Working on ANGLE itself

Skip unless you are editing `vendor/`.

```bash
make angle-save      # export vendor/'s delta -> angle-patches/klepton.patch
make angle-status    # pin, HEAD, local delta, whether the patch is current
make angle-sync      # force the DEPS re-resolve that angle-fetch skips
```

##Note for visionOS 27+ / Beat Saber 1.40:

1. If make angle-all fails, navigate to vendor/depot_tools and run ./update_depot_tools before retrying.
2. To prevent being stuck on the interactive startup screens (Language selection / Explicit content) on a device, you must stage the local save profile explicitly by using:
KLEPTON_TARGET=beatsaber KLT_STAGE_FILES="files" visionos/stage_assets.sh <your-device-udid>
