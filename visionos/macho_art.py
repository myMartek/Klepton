#!/usr/bin/env python3
"""The icon of a Mach-O guest (targets.py kind "macho": Minecraft), made from
the game's own files when nobody supplied one.

AVP Play's installer leaves a target's icon in visionos/Assets.xcassets
(AppIcon-<target>.solidimagestack) and that one always wins. This is for the
build that has none there: a build by hand, or an installer that had no picture
for the game. The game brings its icon with it. Minecraft's is not in the client
jar: it is one of the asset objects the asset index names,

  <payload>/root/inst-<version>/assets/indexes/<n>.json
      "icons/minecraft.icns"    1024 px, the one taken
      "icons/icon_256x256.png"  the largest PNG, taken when there is no .icns
  <payload>/root/inst-<version>/assets/objects/<hh>/<sha1>

in the input folder tools/vivecraft_seed.py fills (visionos/prebuilt/<target>/
payload). The file is checked against the hash the index gives before it is
used. It becomes the FRONT layer of a visionOS icon, fitted into the middle of
a transparent square; the BACK layer has to be opaque and is a plain vertical
gradient written here, so the depth visionOS gives an icon lifts the game's
picture off its ground.

Written to <out>/AppIcon-<target>.solidimagestack in the shape the installer
writes, and gen_xcodeproj.py takes it from <out> when visionos/Assets.xcassets
has none for the target. <out> is under visionos/build/ and the pictures are
the game's: nothing of this is part of the tree. A stack made from the same
icon file is kept. The image work is `sips`, which every Mac has. Any failure
leaves <out> without the stack and the build goes on iconless. All of it
reports on stderr: run.sh keeps only the first stdout line of the project
generator, which must stay "wrote ...".

  python3 macho_art.py <target> <payload folder> <out>
"""
import glob, hashlib, json, os, shutil, struct, subprocess, sys, zlib

ICON_PX = 1024          # a visionOS icon layer
FRONT_PX = 700          # the game's picture on it: inside the circle the icon is shown as
# The ground behind it, top to bottom: a clear sky.
BACK_TOP, BACK_BOTTOM = (0x5d, 0x9b, 0xe6), (0xbf, 0xdf, 0xfb)
# The index's names, best first, and the first bytes each must have.
WANTED = (("icons/minecraft.icns", b"icns"),
          ("icons/icon_256x256.png", b"\x89PNG\r\n\x1a\n"),
          ("icons/icon_128x128.png", b"\x89PNG\r\n\x1a\n"))
MAX_BYTES = 20 * 1024 * 1024   # an "icon" larger than this is not one


def say(text):
    print("  game art: " + text, file=sys.stderr)


def find_icon(payload):
    """(path, sha1, name) of the game's icon in the payload's asset store, or None."""
    for index in sorted(glob.glob(os.path.join(payload, "root", "inst-*", "assets", "indexes", "*.json"))):
        try:
            with open(index, encoding="utf-8") as f:
                objects = json.load(f)["objects"]
        except (OSError, ValueError, KeyError, TypeError):
            continue
        store = os.path.join(os.path.dirname(os.path.dirname(index)), "objects")
        for name, magic in WANTED:
            digest = (objects.get(name) or {}).get("hash", "")
            # Before the hash reaches a path.
            if len(digest) != 40 or any(c not in "0123456789abcdef" for c in digest):
                continue
            path = os.path.join(store, digest[:2], digest)
            if not os.path.isfile(path) or os.path.islink(path) or os.path.getsize(path) > MAX_BYTES:
                continue
            with open(path, "rb") as f:
                data = f.read()
            if hashlib.sha1(data).hexdigest() != digest or not data.startswith(magic):
                continue
            return path, digest, name
    return None


def sips(*args):
    subprocess.run(["sips", *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def gradient_png(path, px, top, bottom):
    """An opaque px-by-px RGB PNG, `top` to `bottom`. Written by hand: sips can
    resize and pad a picture but not make one."""
    rows = bytearray()
    for y in range(px):
        t = y / (px - 1)
        rows += b"\x00" + bytes(round(a + (b - a) * t) for a, b in zip(top, bottom)) * px
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", px, px, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
                + chunk(b"IEND", b""))


def layer(stack, name, png):
    d = os.path.join(stack, f"{name}.solidimagestacklayer", "Content.imageset")
    os.makedirs(d)
    write(os.path.join(os.path.dirname(d), "Contents.json"),
          '{ "info" : { "author" : "xcode", "version" : 1 } }\n')
    base = os.path.basename(png)
    shutil.copy(png, os.path.join(d, base))
    write(os.path.join(d, "Contents.json"),
          '{ "images" : [ { "filename" : "%s", "idiom" : "vision", "scale" : "2x" } ], '
          '"info" : { "author" : "xcode", "version" : 1 } }\n' % base)


def main(target, payload, out):
    stack = os.path.join(out, f"AppIcon-{target}.solidimagestack")
    stamp = os.path.join(out, "source.txt")
    found = find_icon(payload)
    if found is None:
        # No seeded payload, or one without an icon the index vouches for. A
        # stack from an earlier payload is not kept either: it is not this game's.
        shutil.rmtree(stack, ignore_errors=True)
        say(f"no icon of the game under {payload} - the build stays iconless")
        return 1
    src, digest, name = found
    want = f"{name} {digest} front {FRONT_PX} back {BACK_TOP} {BACK_BOTTOM}\n"
    try:
        if os.path.isdir(stack) and open(stamp).read() == want:
            say(f"icon from the game's own {name} (kept from an earlier build)")
            return 0
    except OSError:
        pass
    tmp = os.path.join(out, "tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    try:
        # Front: the game's picture. The copy carries the extension sips reads
        # the format from (the store names a file by its hash alone).
        raw = os.path.join(tmp, "icon" + os.path.splitext(name)[1])
        shutil.copy(src, raw)
        front = os.path.join(tmp, "front.png")
        sips("-s", "format", "png", raw, "--out", front)
        # Resample and pad are two calls: sips does not apply its options in
        # the order given.
        sips("-Z", str(FRONT_PX), front)
        sips("-p", str(ICON_PX), str(ICON_PX), front)
        back = os.path.join(tmp, "back.png")
        gradient_png(back, ICON_PX, BACK_TOP, BACK_BOTTOM)

        shutil.rmtree(stack, ignore_errors=True)
        os.makedirs(stack)
        layer(stack, "Front", front)
        layer(stack, "Back", back)
        write(os.path.join(stack, "Contents.json"),
              '{ "info" : { "author" : "xcode", "version" : 1 }, "layers" : [ %s ] }\n'
              % ", ".join('{ "filename" : "%s.solidimagestacklayer" }' % l for l in ("Front", "Back")))
        write(stamp, want)
    except Exception as e:
        say(f"could not make the icon from {name}: {e} - the build stays iconless")
        shutil.rmtree(stack, ignore_errors=True)
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    say(f"icon from the game's own {name}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
