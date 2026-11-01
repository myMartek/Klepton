#!/usr/bin/env python3
"""Lay out one game of a SourceVR package (github.com/tinsarfal/SourceVR) the way
this tree expects a guest: its libraries in <tree>/lib/arm64-v8a, and the files
its Android launcher would stage at first start in a folder that is copied to
the headset.

    tools/sourcevr_prepare.py <SourceVR-x.y.z.apk> <profile> <tree> <files>

SourceVR is ONE Android app for seven games. The APK itself holds only a small
bootstrap; everything else is a content-addressed store under
assets/sourcevr/v1 with three lists per game:

    native/<profile>.manifest   the engine and game libraries (31 of them; the
                                last is liblauncher.so, the entry)
    overlay/<profile>.manifest  files laid over the game's own (cfg, shaders …)
    vr/<layer>.manifest         the VR additions; which layers a profile uses is
                                the launcher's vrLayers() table, repeated below

The Java launcher picks a profile, stages overlay and vr into its data folder
(sourcevr_runtime_data/v1) and loads the libraries straight out of the APK. No
Java runs here, so this script does the staging once, on the Mac:

    <tree>/lib/arm64-v8a/*.so                 for the translator
    <files>/runtime/v1/overlay/<profile>/…    EXTRAS_VPK_PATH
    <files>/runtime/v1/vr/<layer>/…           VR_CONTENT_PATH

<files> is the folder that becomes /sdcard/SourceVRPort on the headset; the
game's own data (the user's Steam files) goes beside it under common/, and is
not this script's business. Every blob is checked against the hash it is named
by. Nothing of the package is executed.
"""
import hashlib
import pathlib
import shutil
import sys
import zipfile

ROOT = "assets/sourcevr/v1/"
# ProfileAssetStage.vrLayers() in the launcher.
BODY = ["hl2vr", "hl2bodyvr", "hl2bodyhla"]
VR_LAYERS = {
    "portal": ["portalvr"],
    "portal2": ["portal2vr"],
    "ep1": ["ep1vr"] + BODY,
    "ep2": ["ep2vr"] + BODY,
    "lostcoast": BODY,
    "hl2": BODY,
    "entropyzero": BODY,
}


def die(msg):
    sys.exit(f"sourcevr_prepare: {msg}")


def safe(rel):
    p = pathlib.PurePosixPath(rel)
    if p.is_absolute() or ".." in p.parts or not p.parts:
        die(f"unsafe path in a manifest: {rel!r}")
    return p


def blob(z, name, want):
    data = z.read(name)
    if hashlib.sha256(data).hexdigest() != want:
        die(f"{name} is not the content it is named by")
    return data


def stage_tree(z, manifest, dest):
    """`<sha256>  <path>` lines -> files under dest."""
    if dest.exists():
        shutil.rmtree(dest)
    n = size = 0
    for line in z.read(ROOT + manifest).decode().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        digest, rel = line.split(None, 1)
        out = dest / safe(rel.strip())
        out.parent.mkdir(parents=True, exist_ok=True)
        data = blob(z, f"{ROOT}blobs/sha256/{digest}", digest)
        out.write_bytes(data)
        n += 1
        size += len(data)
    return n, size


class DirStore:
    """The same content store, already unpacked: AVP Play unpacks the whole APK
    into the target's tree, so <tree>/assets/sourcevr/v1 holds it. Reads like
    the zip, with the tree as the archive root."""
    def __init__(self, root):
        self.root = pathlib.Path(root)
    def read(self, name):
        p = self.root / name
        if not p.is_file():
            raise KeyError(name)
        return p.read_bytes()


def main():
    # tools/sourcevr_prepare.py <apk> <profile> <tree> <files>
    #     from the APK, on the Mac: libraries into the tree, the staged folders
    #     into <files>/runtime/v1 (copied to the headset as the data folder).
    # tools/sourcevr_prepare.py --tree <profile> <tree>
    #     the tree is the APK as unpacked (AVP Play's way): libraries into the
    #     tree, the staged folders into <tree>/assets/sourcevr-runtime/v1 — the
    #     assets go to the headset with the app, and the runtime (kl_native.c)
    #     reads the folders there. The content store itself (1.5 GB for all
    #     seven games) is then removed from assets, so 355 MB travel, not 1.5 GB.
    args = sys.argv[1:]
    from_tree = bool(args) and args[0] == "--tree"
    if from_tree:
        args = args[1:]
        if len(args) != 2:
            die("usage: sourcevr_prepare.py --tree <profile> <tree>")
        profile, tree = args[0], pathlib.Path(args[1])
        src, files = tree, None
    else:
        if len(args) != 4:
            die("usage: sourcevr_prepare.py <apk> <profile> <tree> <files>")
        src, profile, tree, files = args[0], args[1], pathlib.Path(args[2]), pathlib.Path(args[3])
    if profile not in VR_LAYERS:
        die(f"unknown profile {profile!r}; one of {', '.join(sorted(VR_LAYERS))}")
    z = DirStore(src) if from_tree else zipfile.ZipFile(src)

    # --- libraries
    rows = z.read(f"{ROOT}native/{profile}.manifest").decode().splitlines()
    if not rows or not rows[0].startswith("SOURCEVR_NATIVE_V1\t"):
        die("native manifest has a form this script does not know")
    libdir = tree / "lib" / "arm64-v8a"
    if libdir.exists():
        shutil.rmtree(libdir)
    libdir.mkdir(parents=True)
    names = []
    for row in rows[1:]:
        cols = row.split("\t")
        if len(cols) < 5:
            continue
        digest, name, path = cols[1], cols[2], cols[4]
        if "/" in name or not name.endswith(".so"):
            die(f"unexpected library name {name!r}")
        (libdir / name).write_bytes(blob(z, "assets/" + path, digest))
        names.append(name)
    if not names or names[-1] != "liblauncher.so":
        die("the library list does not end with liblauncher.so")
    if from_tree:
        # The tree is the app's unpacked APK: AndroidManifest.xml and apktool.yml
        # are the real ones and stay. The staged folders go beside the store,
        # then the store goes.
        runtime = tree / "assets" / "sourcevr-runtime" / "v1"
        n, size = stage_tree(z, f"overlay/{profile}.manifest", runtime / "overlay" / profile)
        print(f"  overlay/{profile}: {n} files, {size / 1e6:.0f} MB")
        for layer in VR_LAYERS[profile]:
            n, size = stage_tree(z, f"vr/{layer}.manifest", runtime / "vr" / layer)
            print(f"  vr/{layer}: {n} files, {size / 1e6:.0f} MB")
        shutil.rmtree(tree / "assets" / "sourcevr")
        print(f"  {len(names)} libraries -> {libdir}; the content store is removed from assets")
        return

    # --- what the rest of the tree reads beside the libraries
    version = dict(l.split("=", 1) for l in z.read("assets/version.properties").decode().splitlines()
                   if "=" in l and not l.startswith("#"))
    (tree / "assets").mkdir(parents=True, exist_ok=True)
    (tree / "assets" / "version.properties").write_bytes(z.read("assets/version.properties"))
    (tree / "AndroidManifest.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.sourcevrport.hl2vr">\n'
        '    <application>\n'
        f'        <meta-data android:name="com.sourcevrport.GAME_PROFILE" android:value="{profile}"/>\n'
        '    </application>\n'
        '</manifest>\n')
    (tree / "apktool.yml").write_text(
        "versionInfo:\n"
        f"  versionCode: '{version.get('vrVersionCode', '1')}'\n"
        f"  versionName: {version.get('vrVersionName', '0')}\n")

    # The tree's staging step wants the guest's APK beside the tree and copies it
    # to the headset. Nothing in this guest reads it there (its libraries are
    # translated, its files are staged as folders), so it is a stand-in holding
    # the two small files above instead of 770 MB of content store.
    with zipfile.ZipFile(str(tree) + ".apk", "w", zipfile.ZIP_STORED) as stub:
        stub.write(tree / "AndroidManifest.xml", "AndroidManifest.xml")
        stub.write(tree / "assets" / "version.properties", "assets/version.properties")

    # --- the launcher's staging
    runtime = files / "runtime" / "v1"
    n, size = stage_tree(z, f"overlay/{profile}.manifest", runtime / "overlay" / profile)
    print(f"  overlay/{profile}: {n} files, {size / 1e6:.0f} MB")
    for layer in VR_LAYERS[profile]:
        n, size = stage_tree(z, f"vr/{layer}.manifest", runtime / "vr" / layer)
        print(f"  vr/{layer}: {n} files, {size / 1e6:.0f} MB")
    print(f"  {len(names)} libraries -> {libdir}")


if __name__ == "__main__":
    main()
