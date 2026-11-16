# Scotland2 port component

Current build and update instructions are in [CUSTOM-SONGS.md](../../CUSTOM-SONGS.md).

What AVP Play builds the mods from is `prebuilt/<version code>` (named by
`avp-profiles.json`), a git-ignored folder that a toolchain package must carry:
a package made from a checkout without it builds Beat Saber without mods. That
happened with toolchain 896 (AVP Play 1.10.17 and 1.10.18); 897 brings the
folder back (CUSTOM-SONGS.md, "Through AVP Play", says how to check a package).
The selected release composition is `selected-mods.json`. `build_release.py`
exports source into a private workspace; `bootstrap.py` verifies dependencies and
reconstructs original source; `package_release.py` builds that one composition.

The original Scotland2 loader is a signed native framework. `src/host-bridge.c`
is its optional host integration, behind the versioned `kl_extension` callbacks.
Mod policy and the Apple Unicode rasterizer live in this component. The generic
host retains image loading, prepared signed dispatch and compatibility fixes.

Original Android C++ objects stay in Android modules built with the pinned NDK.
Only flat C interfaces cross to native Apple code. Hook preparation validates
exact source/translation hashes and instruction words before signing. Runtime
installation swaps writable pointers and never writes executable pages.

Original scene timing uses `src/android-flamingo-scene.cpp`. It is required startup
code, not an observer. Release mode packages it as `libsc2_scene` and excludes
custom-types, BSML, SongCore, MetaCore and Flamingo observer DSOs. Unicode's
rendering tests remain diagnostic-only; its original TMP hooks/cache/atlas and
retained managed texture upload stay active in release mode.

Source identities and patches are declared in `source-inputs.json`; build/header
inputs in `build-inputs.json`. `audit_sources.py` verifies every imported file and
reconstructs adaptations. This automated identity check is distinct from the
manual code review recorded in `docs/cleanup/AUDIT-REPORT.md`.

Fake ID remains only as inactive historical source/inventory in Git. It is absent
from the selected composition and the source/builder release package. Historical
progressive package scripts and isolated host fixtures are diagnostic tools.
