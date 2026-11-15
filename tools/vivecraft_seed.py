#!/usr/bin/env python3
"""Fill the vivecraft target's input folder from an archive of the game.

The `vivecraft` target (visionos/targets.py, kind "macho") is Minecraft: Java
Edition with Vivecraft, compiled ahead of time. This tree builds the app around
it — the runtime, the compositor, the launcher — in a few minutes. It does not
build the game: libminecraft.dylib is a GraalVM native image of Minecraft plus
the mods, and making one is a bake of its own (vivecraft-visionos, BUILDING.md
there). So the compiled parts come in an archive and are kept beside the
sources as inputs:

  <app>/Frameworks/<name>.dylib  ->  visionos/prebuilt/vivecraft/Frameworks/
  <app>/klepton-payload/         ->  visionos/prebuilt/vivecraft/payload/
  <app>/Licenses/ or Licenses/   ->  visionos/prebuilt/vivecraft/Licenses/   (when carried)
                                     visionos/prebuilt/vivecraft/SOURCE.txt

The archive is a zip in the shape of an IPA. A release IPA of vivecraft-visionos
is one, with a whole app in it; an archive of the game's parts alone — what a
build of that project on somebody's own Mac leaves — has the same shape and
nothing else in it (Payload/<Name>.app/Frameworks, Payload/<Name>.app/
klepton-payload, and the licence notices either inside the .app or as Licenses/
at the top). Those are all that is asked of it and all that is taken out of it:
no program file and no Info.plist are looked for.

A locally built archive is a different file on every Mac, so an archive is not
accepted by its hash. It is accepted by what is in it, and each of these is
checked before anything is taken:

  - one Payload/<Name>.app
  - Frameworks/ holds exactly the libraries the target's row names
    (visionos/targets.py `libs`), none missing and none beside them
  - each of them is a dynamic library for arm64, built for visionOS devices
    (not the simulator, not another platform, not a universal file)
  - klepton-payload/ has launch.txt, baked.cp, payload-version, tree and
    root/inst-<version> (one), and is laid out for this target's tree
  - launch.txt names no account (the offline placeholders)
  - the payload holds no loose class file and no Mach-O (they fail an install;
    visionos/bundle_payload.sh checks the same before it bundles)
  - a Licenses folder, when carried, holds texts only

Every check that fails is one line on stderr, and the folder is left as it was.
The table KNOWN below only gives an archive its name where it is a published
one (`--known` prints it as JSON); an archive it does not list is named by its
sha256. An unpacked .app is checked the same way and recorded as such.

Where the archive carries the notices (a folder with NOTICES.txt) they go into
the app in place of the tree's visionos/Licenses/vivecraft/
(visionos/copy_licenses.sh): they describe exactly what was compiled into that
archive's game.

visionos/mkmacho.sh wraps the dylibs for the app, visionos/bundle_payload.sh
puts the payload into it as it is. The folder is git-ignored: the game's files
are Mojang's and the mods' authors', and never part of this tree.
KLEPTON_VIVECRAFT_PREBUILT names another folder (here and in the two scripts).

Device slices only: an archive has no simulator build of the game.

The image, the payload and the launch arguments belong together and to one
build of the game. A change to Vivecraft, the mods or Minecraft's version needs
a new image and is not something this script can make.

  python3 tools/vivecraft_seed.py /path/to/vivecraft-visionos-1.0.3.ipa
  python3 tools/vivecraft_seed.py /path/to/<a locally built parts archive>.zip
  python3 tools/vivecraft_seed.py /path/to/Payload/KleptonVivecraft.app

--if-changed leaves a folder alone that was already filled from the same
archive (SOURCE.txt names its sha256, and the libraries and the payload are
there); an archive with another sha256 fills it anew. It is how visionos/run.sh
calls this for a build that was handed one:

  KLEPTON_MACHO_IPA=/abs/path/<archive>  ->  --if-changed <that path>

For the SIMULATOR there is a second source, because a release has no simulator
build of the game: a tree of vivecraft-visionos that was built from source.

  python3 tools/vivecraft_seed.py --tree /path/to/vivecraft-visionos

takes the libraries that tree built for the headset and for the simulator
(where its own visionos/targets.py says they are) and its staged game tree
(visionos/build/native-stage/vivecraft-<version>), into a folder of its OWN:

  visionos/prebuilt/vivecraft-tree/Frameworks/       device slices
  visionos/prebuilt/vivecraft-tree/Frameworks-sim/   simulator slices
  visionos/prebuilt/vivecraft-tree/payload/          the staged tree

A self-built image belongs with the staged tree of its own bake, a release's
image with the release's payload; mixing the two does not start. So the two
sources never share a folder, and a build says which one it is for:

  KLEPTON_VIVECRAFT_PREBUILT=visionos/prebuilt/vivecraft-tree visionos/run.sh

That staged tree has its class folders loose, which a simulator's container
takes and a bundle on a headset does not (visionos/bundle_payload.sh refuses
it): it is for simulator builds.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Published archives: sha256 of the file -> what it is, as it is shown and
# recorded. One line each; a new one is one more line. Only a name: an archive
# that is not here (every locally built one) is taken on what is in it.
KNOWN = {
    "ece6db2e0e3cf0c04e542730a3af2279e2ad4bae463967c06c9f081cf40c5f9b": "vivecraft-visionos 1.0.3",
}

# The first four bytes of a file the installer would take for code: Java class
# files share the fat Mach-O magic. See visionos/bundle_payload.sh.
MAGIC = {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
         b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def target_field(field):
    out = subprocess.run([sys.executable, os.path.join(ROOT, "visionos", "targets.py"), "vivecraft", field],
                         check=True, capture_output=True, text=True).stdout
    return out.strip()


def clone(src, dst):
    """Copy a file or a tree, as an APFS clone where the volume can (the image
    alone is 211 MB and the payload 0.6 GB), else byte for byte. Symlinks are
    kept as links: nothing is followed out of the release."""
    if subprocess.run(["cp", "-Rc", src, dst], capture_output=True).returncode == 0:
        return
    if os.path.lexists(dst):
        shutil.rmtree(dst) if os.path.isdir(dst) and not os.path.islink(dst) else os.remove(dst)
    if os.path.isdir(src):
        shutil.copytree(src, dst, symlinks=True)
    else:
        shutil.copyfile(src, dst)


def seeded_from(dest, digest, libs):
    """True when `dest` was filled from the IPA with this sha256 and still holds
    what that left: SOURCE.txt's first line, every library, the payload with the
    version SOURCE.txt recorded."""
    try:
        lines = open(os.path.join(dest, "SOURCE.txt"), encoding="utf-8").read().splitlines()
        version = open(os.path.join(dest, "payload", "payload-version"), encoding="utf-8").read().strip()
    except OSError:
        return False
    if not lines or not lines[0].rstrip(")").endswith("sha256 " + digest):
        return False
    if "payload " + version not in lines:
        return False
    if any(l.startswith("licences from the archive") for l in lines) \
            and not os.path.isfile(os.path.join(dest, "Licenses", "NOTICES.txt")):
        return False
    return (all(os.path.isfile(os.path.join(dest, "Frameworks", n + ".dylib")) for n in libs)
            and all(os.path.exists(os.path.join(dest, "payload", n)) for n in ("launch.txt", "baked.cp", "root", "tree")))


def code_files(folder):
    """The files under `folder` the installer would take for code (MAGIC)."""
    found = []
    for r, _, fs in os.walk(folder):
        for f in fs:
            p = os.path.join(r, f)
            if os.path.islink(p):
                continue
            with open(p, "rb") as fh:
                if fh.read(4) in MAGIC:
                    found.append(os.path.relpath(p, folder))
    return found


def unpack(archive, folder):
    """Take out of the archive what this target is made of, and nothing else:
    the one app's Frameworks/<name>.dylib, its klepton-payload/ and the licence
    notices (inside the app, or Licenses/ at the top). Returns the app's folder
    and the licence folder (None when the archive carries no NOTICES.txt)."""
    try:
        z = zipfile.ZipFile(archive)
    except (zipfile.BadZipFile, OSError) as e:
        raise SystemExit("vivecraft_seed: %s is not a zip archive (%s) - nothing was taken from it"
                         % (os.path.basename(archive), e))
    with z:
        names = z.namelist()
        apps = sorted({n.split("/")[1] for n in names
                       if n.startswith("Payload/") and n.count("/") >= 2 and n.split("/")[1].endswith(".app")})
        if len(apps) != 1:
            raise SystemExit("vivecraft_seed: %s: one Payload/<Name>.app is expected, the archive has %d - "
                             "nothing was taken from it" % (os.path.basename(archive), len(apps)))
        root = "Payload/%s/" % apps[0]
        def wanted(n):
            if n.startswith(root + "Frameworks/"):
                rest = n[len(root) + len("Frameworks/"):]
                return "/" not in rest and rest.endswith(".dylib")
            return n.startswith((root + "klepton-payload/", root + "Licenses/", "Licenses/"))
        # extractall keeps every member under `folder` (it drops leading
        # slashes and ".." parts) and writes a link in the archive as a file.
        z.extractall(folder, [n for n in names if wanted(n)])
    app = os.path.join(folder, "Payload", apps[0])
    for licences in (os.path.join(app, "Licenses"), os.path.join(folder, "Licenses")):
        if os.path.isfile(os.path.join(licences, "NOTICES.txt")):
            return app, licences
    return app, None


# Mach-O: what a library of this target has to be.
MH_MAGIC_64, CPU_ARM64, MH_DYLIB, LC_BUILD_VERSION = 0xfeedfacf, 0x0100000c, 6, 0x32
PLATFORM_VISIONOS = 11
PLATFORMS = {1: "macOS", 2: "iOS", 3: "tvOS", 4: "watchOS", 6: "Mac Catalyst", 7: "the iOS simulator",
             8: "the tvOS simulator", 9: "the watchOS simulator", 11: "visionOS devices",
             12: "the visionOS simulator"}


def library_problem(path):
    """None when the file is a dynamic library for arm64 built for visionOS
    devices; otherwise what it is instead. Read from the file's own header."""
    with open(path, "rb") as f:
        head = f.read(32)
        if len(head) < 32:
            return "is not a Mach-O file"
        if head[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            return "is a universal file (one arm64 library is expected)"
        magic, cpu, _, filetype, ncmds, sizeofcmds = struct.unpack("<IIIIII", head[:24])
        if magic != MH_MAGIC_64:
            return "is not a 64-bit Mach-O file"
        if cpu != CPU_ARM64:
            return "is not built for arm64"
        if filetype != MH_DYLIB:
            return "is not a dynamic library"
        if sizeofcmds > (4 << 20):
            return "has a header that is not a library's"
        cmds = f.read(sizeofcmds)
    at, platform = 0, None
    for _ in range(ncmds):
        if at + 8 > len(cmds):
            return "has a damaged header"
        cmd, size = struct.unpack_from("<II", cmds, at)
        if size < 8 or at + size > len(cmds):
            return "has a damaged header"
        if cmd == LC_BUILD_VERSION and size >= 12:
            platform = struct.unpack_from("<I", cmds, at + 8)[0]
            break
        at += size
    if platform is None:
        return "does not say which platform it is built for"
    if platform != PLATFORM_VISIONOS:
        return "is built for %s, not for visionOS devices" % PLATFORMS.get(platform, "platform %d" % platform)
    return None


def problems_of(app, licences, libs, want_tree):
    """Everything about the archive's app that is not as this target needs it,
    one line each (empty: it is fine), and the Minecraft version it holds."""
    bad, mcv = [], ""
    frameworks = os.path.join(app, "Frameworks")
    have = sorted(f[:-6] for f in (os.listdir(frameworks) if os.path.isdir(frameworks) else [])
                  if f.endswith(".dylib") and os.path.isfile(os.path.join(frameworks, f)))
    for n in libs:
        if n not in have:
            bad.append("Frameworks/%s.dylib is missing (the target's row names it)" % n)
    for n in have:
        if n not in libs:
            bad.append("Frameworks/%s.dylib is not a library the target's row names "
                       "(visionos/targets.py, vivecraft, libs)" % n)
        else:
            why = library_problem(os.path.join(frameworks, n + ".dylib"))
            if why:
                bad.append("Frameworks/%s.dylib %s" % (n, why))

    payload = os.path.join(app, "klepton-payload")
    for need in ("launch.txt", "baked.cp", "payload-version", "tree", "root"):
        if not os.path.exists(os.path.join(payload, need)):
            bad.append("klepton-payload/%s is missing" % need)
    def text(name):
        try:
            return open(os.path.join(payload, name), encoding="utf-8", errors="replace").read()
        except OSError:
            return None
    tree = text("tree")
    if tree is not None and tree.strip() != want_tree:
        bad.append("klepton-payload/tree says %r, this target's tree is %r" % (tree.strip(), want_tree))
    version = text("payload-version")
    if version is not None and not version.strip():
        bad.append("klepton-payload/payload-version is empty")
    if os.path.isdir(os.path.join(payload, "root")):
        insts = sorted(d for d in os.listdir(os.path.join(payload, "root"))
                       if d.startswith("inst-") and os.path.isdir(os.path.join(payload, "root", d)))
        if len(insts) != 1:
            bad.append("klepton-payload/root holds %d inst-<version> folders (one is expected)%s"
                       % (len(insts), ": " + " ".join(insts) if insts else ""))
        else:
            mcv = insts[0][len("inst-"):]
    launch = text("launch.txt")
    if launch is not None:
        # No account in the game's command line: it is put in at run time, in
        # memory (the same three values visionos/bundle_payload.sh insists on).
        lines = [l.strip() for l in launch.splitlines()]
        after = {l: lines[i + 1] for i, l in enumerate(lines[:-1]) if l in ("--username", "--uuid", "--accessToken")}
        if after.get("--username") not in ("@USER@", "Player"):
            bad.append("klepton-payload/launch.txt: --username is not the offline placeholder")
        if after.get("--uuid") != "00000000-0000-0000-0000-000000000000":
            bad.append("klepton-payload/launch.txt: --uuid is not the offline placeholder")
        if after.get("--accessToken") != "0":
            bad.append("klepton-payload/launch.txt: --accessToken is not the offline placeholder")
    if os.path.isdir(payload):
        loose = code_files(payload)
        if loose:
            bad.append("klepton-payload holds %d loose class or Mach-O file(s), which fail an install "
                       "(first: %s)" % (len(loose), loose[0]))
    if licences:
        # Texts and nothing else: a link or a program in there would ride into
        # the app unsigned.
        odd = code_files(licences) + [os.path.relpath(os.path.join(r, n), licences)
                                      for r, ds, fs in os.walk(licences) for n in ds + fs
                                      if os.path.islink(os.path.join(r, n))]
        if odd:
            bad.append("Licenses holds %d file(s) that are not texts (first: %s)" % (len(odd), odd[0]))
    return bad, mcv


def seed_from_tree(tree):
    """The libraries and the staged game tree of a from-source build."""
    tree = os.path.abspath(tree)
    dest = os.environ.get("KLEPTON_VIVECRAFT_PREBUILT") or os.path.join(ROOT, "visionos", "prebuilt", "vivecraft-tree")
    stages = sorted(d for d in os.listdir(os.path.join(tree, "visionos", "build", "native-stage"))
                    if d.startswith("vivecraft-"))
    if len(stages) != 1:
        raise SystemExit("expected one visionos/build/native-stage/vivecraft-<version> in the tree, found %s" % stages)
    stage = os.path.join(tree, "visionos", "build", "native-stage", stages[0])
    mcv = stages[0][len("vivecraft-"):]
    for need in ("launch.txt", "baked.cp", "root"):
        if not os.path.exists(os.path.join(stage, need)):
            raise SystemExit("the staged tree has no %s" % need)
    if not os.path.isdir(os.path.join(stage, "root", "inst-" + mcv)):
        raise SystemExit("%s holds no root/inst-%s" % (stage, mcv))

    # Where that tree built each library, by its own table ({sdk}, {sim} are its
    # placeholders; the image of an instance is in out/t8-<version>/, as its
    # mknative.sh has it).
    out = subprocess.run([sys.executable, "-I", os.path.join(tree, "visionos", "targets.py"), "vivecraft", "natives"],
                         check=True, capture_output=True, text=True, env=dict(os.environ, MCV=mcv)).stdout
    theirs = dict(p.split("=", 1) for p in out.split())
    libs = target_field("libs").split()
    if set(theirs) != set(libs):
        raise SystemExit("the tree builds %s, this target embeds %s" % (sorted(theirs), sorted(libs)))

    if os.path.lexists(dest):
        shutil.rmtree(dest)
    counts = {}
    for folder, sdk, sim in (("Frameworks", "xros", "xros"), ("Frameworks-sim", "xrsimulator", "xrsim")):
        os.makedirs(os.path.join(dest, folder))
        for name in libs:
            rel = theirs[name].replace("{sdk}", sdk).replace("{sim}", sim)
            if name == "libminecraft" and "/out/t8/" in rel:
                rel = rel.replace("/out/t8/", "/out/t8-%s/" % mcv)
            src = os.path.join(tree, rel)
            if not os.path.isfile(src):
                raise SystemExit("the tree has not built %s (%s)" % (rel, sdk))
            clone(src, os.path.join(dest, folder, name + ".dylib"))
        counts[folder] = len(libs)
    os.makedirs(os.path.join(dest, "payload"))
    for part in ("launch.txt", "baked.cp", "root"):
        clone(os.path.join(stage, part), os.path.join(dest, "payload", part))
    with open(os.path.join(dest, "payload", "tree"), "w", encoding="utf-8") as f:
        f.write(target_field("tree") + "\n")
    with open(os.path.join(dest, "SOURCE.txt"), "w", encoding="utf-8") as f:
        f.write("a build tree, %s\nminecraft %s\npayload its staged tree, class folders loose: simulator only\n" % (tree, mcv))
    print("seeded %s" % dest)
    print("  from the build tree %s" % tree)
    print("  Minecraft %s, %d libraries for the headset and %d for the simulator, its staged tree"
          % (mcv, counts["Frameworks"], counts["Frameworks-sim"]))
    print("build with: KLEPTON_VIVECRAFT_PREBUILT=%s KLEPTON_TARGET=vivecraft visionos/run.sh" % dest)


def main(argv):
    if "--known" in argv:
        import json
        print(json.dumps(KNOWN, indent=1, sort_keys=True))
        return
    if "--tree" in argv:
        rest = [a for a in argv[1:] if a != "--tree"]
        if len(rest) != 1:
            raise SystemExit(__doc__)
        return seed_from_tree(rest[0])
    # (--any is accepted and means nothing any more: no archive is refused for
    # its hash.)
    args = [a for a in argv[1:] if not a.startswith("--")]
    if_changed = "--if-changed" in argv
    if len(args) != 1:
        raise SystemExit(__doc__)
    src = os.path.abspath(args[0])
    dest = os.environ.get("KLEPTON_VIVECRAFT_PREBUILT") or os.path.join(ROOT, "visionos", "prebuilt", "vivecraft")
    libs = target_field("libs").split()
    name = os.path.basename(src.rstrip("/"))

    tmp = None
    licences = None
    if os.path.isdir(src) and src.rstrip("/").endswith(".app"):
        app, release = src.rstrip("/"), "an unpacked app, no archive to name"
        if os.path.isfile(os.path.join(app, "Licenses", "NOTICES.txt")):
            licences = os.path.join(app, "Licenses")
    elif os.path.isfile(src):
        digest = sha256(src)
        release = ("%s (sha256 %s)" % (KNOWN[digest], digest)) if digest in KNOWN \
            else "an archive not among the published ones, sha256 " + digest
        if if_changed and seeded_from(dest, digest, libs):
            print("already seeded: %s holds %s" % (dest, release))
            return
        tmp = tempfile.mkdtemp(prefix="vivecraft-seed-")
        try:
            app, licences = unpack(src, tmp)
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
    else:
        raise SystemExit("vivecraft_seed: %s is neither an archive (.ipa, .zip) nor an unpacked .app" % src)

    try:
        bad, mcv = problems_of(app, licences, libs, target_field("tree"))
        if bad:
            for line in bad:
                print("vivecraft_seed: %s: %s" % (name, line), file=sys.stderr)
            raise SystemExit("vivecraft_seed: %s is not usable as this target's game (%d check%s failed) - "
                             "nothing was taken from it" % (name, len(bad), "" if len(bad) == 1 else "s"))
        payload = os.path.join(app, "klepton-payload")

        if os.path.lexists(dest):
            shutil.rmtree(dest)
        os.makedirs(os.path.join(dest, "Frameworks"))
        for lib in libs:
            clone(os.path.join(app, "Frameworks", lib + ".dylib"),
                  os.path.join(dest, "Frameworks", lib + ".dylib"))
        clone(payload, os.path.join(dest, "payload"))
        count = 0
        if licences:
            clone(licences, os.path.join(dest, "Licenses"))
            count = sum(len(fs) for _, _, fs in os.walk(os.path.join(dest, "Licenses")))
        version = open(os.path.join(payload, "payload-version"), encoding="utf-8").read().strip()
        with open(os.path.join(dest, "SOURCE.txt"), "w", encoding="utf-8") as f:
            f.write("%s\nminecraft %s\npayload %s\n%s\n" % (
                release, mcv, version,
                ("licences from the archive, %d files" % count) if licences
                else "licences not in the archive: the tree's visionos/Licenses/%s" % target_field("name")))

        size = sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(dest) for n in fs
                   if not os.path.islink(os.path.join(r, n)))
        print("seeded %s" % dest)
        print("  from %s" % release)
        print("  Minecraft %s, %d libraries, payload %s, %s, %.0f MB in all"
              % (mcv, len(libs), version[:12],
                 ("its %d licence files" % count) if licences else "no licence folder (the tree's is used)",
                 size / 1e6))
        print("build with: KLEPTON_TARGET=vivecraft KL_NO_INSTALL=1 visionos/run.sh device")
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main(sys.argv)
