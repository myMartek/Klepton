# Third-party components

Everything compiled into the vivecraft-visionos IPA, with the license, version, and upstream that apply. Versions are the ones in the Minecraft 26.3 native image (release 1.0.3). Release 1.0.2 shipped Minecraft 26.2; its versions are in this file at tag `1.0.2`. This is a notice file, not legal advice.

The native image is one library, `libminecraft.dylib`, plus the Klepton runtime and the visionOS frameworks. A player cannot remove a mod jar after install.

Our own code (the Klepton app shell, the runtime, the Vivecraft patches in this repo, the Darwin LTW port, the OpenXR shims, klglfw, klsdl, klegl) is MIT, Copyright (c) 2026 Max Thomas, in `LICENSE` at the repo root. Upstream Klepton is https://github.com/shinyquagsire23/Klepton. The fork this port builds is https://github.com/rebelancap/Klepton.

LGPL and GPL texts are in this repo: `LICENSES/LGPL-3.0.txt` and `LICENSES/GPL-3.0.txt`. Apache-2.0, MIT, and BSD-3-Clause texts are in the same directory. Several jars below omit a license file; the notice to ship is named in the row.

## Minecraft client

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| Minecraft client | 26.3 | Minecraft EULA and the Microsoft Services Agreement | https://aka.ms/MinecraftEULA and https://www.microsoft.com/msa |

The client jar is downloaded at build time (sha1 `e877b6a07acd633fb3bb475002175cec036e7b87`). Asset index 34 is the same. Neither is in git. Both are compiled or staged into the app. The ownership gate is what requires an owning account at launch. It does not change these terms.

Mojang libraries with no license file of their own, shipped as part of the client: `authlib` 10.0.77, `blocklist` 1.0.10, `patchy` 2.2.10, `text2speech` 1.19.12.

## Mods

| Mod | Version | License | Upstream |
| --- | --- | --- | --- |
| Vivecraft | 26.3-1.3.18 | LGPLv3 | https://github.com/Vivecraft/VivecraftMod (`LICENSE.md` on branch `Multiloader-26.3`). Modrinth: https://modrinth.com/mod/vivecraft |
| Sodium | 0.9.2+mc26.3 | Polyform Shield 1.0.0 | https://github.com/CaffeineMC/sodium |
| Lithium | 0.26.3+mc26.3 | LGPL-3.0-only | https://github.com/CaffeineMC/lithium-fabric |
| Fabric API | 0.165.2+26.3 | Apache-2.0 | https://github.com/FabricMC/fabric |
| Fabric Loader | 0.19.5 | Apache-2.0 | https://github.com/FabricMC/fabric-loader |
| Simple Voice Chat | 2.6.27+26.3 | All Rights Reserved | https://github.com/henkelmax/simple-voice-chat |
| Cloth Config | 26.3.159 | LGPLv3 | https://github.com/shedaniel/ClothConfig |
| Entity Culling | 1.11.3 | tr7zw Protective License | https://github.com/tr7zw/EntityCulling |
| FerriteCore | 9.0.0 | MIT | https://github.com/malte0811/FerriteCore |
| ImmediatelyFast | 1.17.1+26.3 | LGPL-3.0-or-later | https://github.com/RaphiMC/ImmediatelyFast |
| Krypton | 0.3.2 | LGPL-3.0 | https://github.com/astei/krypton |
| More Culling | 1.9.1 | GPL-3.0-only | https://github.com/fxmorin/moreculling |
| BadOptimizations | 2.4.1 | MIT | https://github.com/imthosea/BadOptimizations |
| Better Biome Blend | 1.4.1 | LGPL-3.0 | https://github.com/FionaTheMortal/better-biome-blend |

Vivecraft's upstream `LICENSE.md` also covers Minecrift (LGPL-3.0-or-later, Pete Tabram and Mark Browning, 2013), and it restates the notices for JNA, NightConfig, LWJGL, JavaOSC, and Fabric API. The jar itself has no license file. The 26.3 jar is built from upstream `Multiloader-26.3` at `15a9011f` (mod 1.3.18) plus `work/vivecraft-openxr-26.3/patches/` 0000–0006. Patch 0000 is upstream code, not ours: the Vivecraft authors' `openxr` branch (`13eafae1`) merged onto `15a9011f` and adapted to 26.3, LGPLv3 like the rest of Vivecraft. 0001–0006 are the changes this repo makes. The tree after 0006 is `b0488a1e424191a8d9b95afde53d46b54d294282`. The 1.0.3 image has patch 0006; the 1.0.1 and 1.0.2 images (Minecraft 26.2) did not (see UPSTREAM.md).

**Sodium.** Polyform Shield 1.0.0, not LGPL. The full text from the jar's `LICENSE.md` is [LICENSES/Polyform-Shield-1.0.0.md](LICENSES/Polyform-Shield-1.0.0.md). The same text ships in the IPA. Canonical URL: https://polyformproject.org/licenses/shield/1.0.0. The license requires that anyone who receives the software also receives these terms, or that URL. The jar has no extra `Required Notice:` lines.

**Simple Voice Chat.** `fabric.mod.json` says All Rights Reserved. The upstream `license` file is one line: Copyright (c) Max Henkel - All Rights Reserved. The project FAQ allows use in a modpack with credit: https://modrepo.de/minecraft/voicechat/faq. Homepage: https://modrepo.de/minecraft/voicechat/overview. Modrinth: https://modrinth.com/mod/simple-voice-chat. Codec notices inside the jar are listed under Native codecs below. They license the codecs, not the mod.

Re-checked for 2.6.27+26.3 on 2026-11-07: `fabric.mod.json` still says All Rights Reserved; the jar still has no license file of the mod's own; the upstream `license` file is still the same one line (last changed in commit `fb7a6d98`, 2022-11-09); GitHub reports no SPDX license for the repository; the FAQ still has the same modpack answer ("Yes. But please make sure to give credit.") and nothing on redistribution, reuploads or forks. Nothing in 2.6.27 changes the 26.2 finding. The FAQ grants modpack use with credit; it does not say anything about a mod compiled into a native image. The mod ships in this image by the project owner's decision, with credit here and in the README.

**Entity Culling.** The tr7zw Protective License grants use, modify, and compile. It does not grant distribute. It also refuses commercial advantage and monetary compensation. Text: https://github.com/tr7zw/EntityCulling/blob/1.18/LICENSE-EntityCulling. The Modrinth page says the jar may be redistributed in Modrinth and CurseForge modpacks and in YouTube videos, and not elsewhere. The jar also bundles occlusionculling 0.0.8-SNAPSHOT (LogisticsCraft) under MIT. That MIT grant is for occlusionculling.

**More Culling.** GPL-3.0-only. https://www.gnu.org/licenses/gpl-3.0.html. The project page says modpacks are fine with credit, and asks that a client or another mod not merge this mod without permission: https://modrinth.com/mod/moreculling.

**Krypton** is LGPL-3.0. The jar nests `velocity-native` 3.4.0-SNAPSHOT, which is GPL-3.0, including its Java compression and cipher classes. Upstream: https://github.com/PaperMC/Velocity (relicensed to GPL-3.0 on 2021-03-31). Those classes are on the baked classpath.

**BadOptimizations** and **FerriteCore** are MIT upstream. The jars omit the license text. Ship the MIT notice. BadOptimizations' contact URL is https://github.com/imthosea/BadOptimizations (Modrinth `g96Z4WVZ`).

**Cloth Config** shades toml4j 0.7.2 (MIT) and SnakeYAML 1.27 (Apache-2.0). Ship those notices with it.

## Mod libraries

These are their own jars on the classpath, or are shaded inside a mod that is.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| voicechat_api | 2.6.27 | All Rights Reserved, with Simple Voice Chat | https://github.com/henkelmax/simple-voice-chat |
| configbuilder | 2.1.0 | No license text | Shaded in Simple Voice Chat. The embedded pom has no license field. https://github.com/henkelmax/config-builder has no LICENSE file. |
| velocity-native | 3.4.0-SNAPSHOT | GPL-3.0 | https://github.com/PaperMC/Velocity |
| conditional-mixin | 0.6.3 | LGPL-3.0-only | https://github.com/Fallen-Breath/conditional-mixin |
| MixinExtras | 0.5.5 | MIT | https://github.com/LlamaLad7/MixinExtras. Nested in Fabric Loader 0.19.5 |
| cloth-basic-math | 0.6.1 | Unlicense | https://github.com/shedaniel/cloth-basic-math |
| night-config core | 3.6.6 | LGPL-3.0 | https://github.com/TheElectronWill/night-config |
| night-config toml | 3.6.6 | LGPL-3.0 | same |
| tact-java | 0.1.4 | Apache-2.0 | https://github.com/bhaptics/tact-java |
| javaosc-core | 0.9 | BSD-3-Clause | https://github.com/hoijui/JavaOSC. Copyright (c) 2002-2014, Chandrasekhar Ramakrishnan / Illposed Software |
| Java-WebSocket | 1.5.1 | MIT | https://github.com/TooTallNate/Java-WebSocket |
| TRansition | 1.0.26 | MIT | https://github.com/tr7zw/TRansition |
| TRender | 1.0.17 | MIT | https://github.com/tr7zw/TRender |
| sponge-mixin | 0.17.4+mixin.0.8.7 | MIT | The jar's `LICENSE.txt` is the MIT text |

Fabric API modules. All Apache-2.0, same grant as Fabric API, upstream https://github.com/FabricMC/fabric.

| Id | Version |
| --- | --- |
| fabric-advancement-api-v1 | 1.0.1+38c7a2d55d |
| fabric-api-base | 2.0.6+fcdff87f5d |
| fabric-api-lookup-api-v1 | 2.0.24+3434d6d95d |
| fabric-biome-api-v1 | 20.1.1+85210bca5d |
| fabric-block-api-v1 | 3.1.0+0ae6f9115d |
| fabric-block-getter-api-v2 | 2.0.9+3434d6d95d |
| fabric-command-api-v2 | 3.1.2+fcdff87f5d |
| fabric-content-registries-v0 | 15.0.7+74ed1ea55d |
| fabric-convention-tags-v2 | 4.10.5+187b64905d |
| fabric-crash-report-info-v1 | 1.0.7+fcdff87f5d |
| fabric-creative-tab-api-v1 | 5.0.22+fcdff87f5d |
| fabric-data-attachment-api-v1 | 2.2.34+99fb1a765d |
| fabric-data-generation-api-v1 | 27.4.3+187b64905d |
| fabric-debug-api-v1 | 1.0.5+5af7744b5d |
| fabric-dimensions-v1 | 5.1.20+5af7744b5d |
| fabric-entity-events-v1 | 6.0.4+3434d6d95d |
| fabric-events-interaction-v0 | 5.3.10+b9b88df45d |
| fabric-game-rule-api-v1 | 4.0.10+3434d6d95d |
| fabric-item-api-v1 | 14.8.1+8f511c9f5d |
| fabric-key-mapping-api-v1 | 2.0.8+3434d6d95d |
| fabric-lifecycle-events-v1 | 4.1.9+ffef5f675d |
| fabric-loot-api-v3 | 4.0.9+4068fd645d |
| fabric-menu-api-v1 | 2.0.31+ed66f0a35d |
| fabric-message-api-v1 | 7.0.10+3434d6d95d |
| fabric-model-loading-api-v1 | 8.0.37+fcdff87f5d |
| fabric-networking-api-v1 | 6.4.1+c58129785d |
| fabric-object-builder-api-v1 | 24.1.13+e2f63d3e5d |
| fabric-particles-v1 | 5.0.28+3434d6d95d |
| fabric-permission-api-v1 | 1.0.8+fcdff87f5d |
| fabric-recipe-api-v1 | 10.0.12+b9b88df45d |
| fabric-registry-sync-v0 | 8.0.5+1ca9f6365d |
| fabric-renderer-api-v1 | 17.0.17+79385d0b5d |
| fabric-renderer-indigo | 9.1.25+b9d63e335d |
| fabric-rendering-fluids-v1 | 6.0.6+3434d6d95d |
| fabric-rendering-v1 | 27.2.0+eb9fe70b5d |
| fabric-resource-conditions-api-v1 | 6.2.1+fd3b1d565d |
| fabric-resource-loader-v0 | 3.3.27+4fc5413f5d |
| fabric-resource-loader-v1 | 3.1.0+5af7744b5d |
| fabric-screen-api-v1 | 5.2.4+48607d035d |
| fabric-serialization-api-v1 | 2.0.7+74ed1ea55d |
| fabric-sound-api-v1 | 2.0.7+3434d6d95d |
| fabric-tag-api-v1 | 2.1.11+fcdff87f5d |
| fabric-transfer-api-v1 | 8.0.26+140095665d |
| fabric-transitive-access-wideners-v1 | 8.1.17+e2f63d3e5d |

## Mojang libraries that are openly licensed

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| brigadier | 1.3.11 | MIT | https://github.com/Mojang/brigadier |
| datafixerupper | 10.0.21 | MIT | https://github.com/Mojang/DataFixerUpper |
| logging (`com.mojang:logging`) | 1.7.12 | No license text published with this version | The jar, the Minecraft-libraries POM, and the Gradle module metadata contain no license field. |
| jtracy | 1.14.38 | BSD-3-Clause | https://github.com/wolfpld/tracy. Copyright (c) 2017-2026, Bartosz Taudul. The native jars contain `Tracy_LICENSE` |

## Other Java libraries

Apache-2.0, license file in the jar: `commons-codec` 1.22.0, `commons-compress` 1.28.0, `commons-io` 2.20.0, `commons-lang3` 3.20.0 (each also has `NOTICE.txt`), `failureaccess` 1.0.3, `guava` 33.6.0-jre, `log4j-api` 2.26.0, `log4j-core` 2.26.0, `log4j-slf4j2-impl` 2.26.0.

Apache-2.0, manifest `Bundle-License` only: `gson` 2.14.0, `jspecify` 1.0.0, and the Netty 4.2.16.Final jars: `netty-buffer`, `netty-codec-base`, `netty-codec-compression`, `netty-codec-http`, `netty-common`, `netty-handler`, `netty-resolver`, `netty-transport`, `netty-transport-classes-epoll`, `netty-transport-classes-kqueue`, `netty-transport-native-kqueue` (osx-aarch_64 and osx-x86_64), `netty-transport-native-unix-common`.

Apache-2.0, upstream SPDX, no file in the jar: `fastutil` 8.5.18 (https://github.com/vigna/fastutil), `lz4-java` 1.10.1 (`at.yawk.lz4`).

MIT, evidence in the jar: `jopt-simple` 5.0.4, `slf4j-api` 2.0.17, `msal4j` 1.24.1.

MIT, upstream SPDX, no file in the jar: `joml` 1.10.9 (https://github.com/JOML-CI/JOML; the jar has no license file; `build-t4.sh` drops its three class-version-46 `package-info` classes from the image's copy), `oshi-core` 6.9.0 (https://github.com/oshi/oshi), `azure-json` 1.4.0 (Azure SDK for Java).

`java-objc-bridge` 1.1 (`ca.weblite`): the jar's pom names Apache License, Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0.txt). The jar has no license file. The notice text is `LICENSES/Apache-2.0.txt`.

BSD-3-Clause, manifest only: `asm` 9.10.1, `asm-analysis`, `asm-commons`, `asm-tree`, `asm-util`. https://asm.ow2.io/LICENSE.txt.

`icu4j` 78.3. Unicode terms, http://www.unicode.org/copyright.html. The manifest reads "© 2016 and later: Unicode, Inc. and others."

`jorbis` 0.0.17 (`org.jcraft`). Upstream JCraft JOrbis is LGPL. The jar does not name the LGPL version. Ship the LGPL text.

JNA 5.17.0 and jna-platform 5.17.0. Dual license: Apache-2.0 OR LGPL-2.1-or-later. This project elects Apache-2.0. https://github.com/java-native-access/jna.

## LWJGL, OpenVR, OpenXR

LWJGL 3 is BSD-3-Clause. Copyright (c) 2012-present Lightweight Java Game Library. https://github.com/LWJGL/lwjgl3. Vivecraft's `LICENSE.md` quotes the BSD-3 text. Natives LWJGL vendors (FreeType, stb, and the others) keep the notices LWJGL documents.

On this classpath:

- LWJGL 3.4.3 (`lwjgl-3.4.3-unsafe` and the macOS arm64 native) and the modules freetype, jemalloc, openal, opengl, sdl, shaderc, spvc, stb, vma, vulkan, each with a macOS arm64 native jar. Source tag `3.4.3`, commit `30fac9b95f99cda97312232be25ba55297bf9951`. Minecraft 26.3 dropped `lwjgl-glfw` and `lwjgl-tinyfd` from its classpath (the app still carries the `liblwjgl_tinyfd` native built from the same tag).
- `lwjgl-openvr` 3.3.6 and its natives.
- `lwjgl-openxr` 3.4.3 and its natives. No macOS OpenXR native jar is on the classpath. The visionOS loader is the shim below.

Minecraft 26.3 creates its window, GL context and input through SDL3 (`lwjgl-sdl`). This app ships no SDL3 code. The visionOS build replaces `lwjgl-sdl-3.4.3.jar` with `lwjgl-sdl-klsdl-3.4.3.jar` (`work/native-image/scripts/t2/build-klsdl.sh`): LWJGL's own `SDL_*` struct and callback classes from the stock jar (BSD-3-Clause, LWJGL), and project function classes (`SDL`, `SDLVideo`, `SDLEvents`, …, MIT, `work/native-image/scripts/t4-src/java-sdl/`) over `libklsdl`, which builds on the klglfw C code. The stock `libSDL3.dylib` is used only by the host JVM during the bake. The 26.2 build swapped `lwjgl-glfw` for klglfw the same way.

FreeType 2.14.3, FreeType License (FTL, BSD-style with an advertising credit), commit `0a0221a1347e2f1e07c395263540026e9a0aa7c7` (tag `VER-2-14-3`, the commit LWJGL 3.4.3's `libfreetype.dylib.git` names). https://github.com/freetype/freetype. Text: [LICENSES/FTL.txt](LICENSES/FTL.txt) (`docs/FTL.TXT` from that tag). Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All rights reserved.

libffi 3.8.0, MIT, release tarball sha256 `7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4`. https://github.com/libffi/libffi. Copyright (c) 1996-2026 Anthony Green, Red Hat, Inc and others (the tarball's `LICENSE`; the grant is the MIT text in `LICENSES/MIT.txt`). Linked statically into `liblwjgl`. LWJGL 3.4.3 needs 3.8.0 (its bundled `ffi.h` and `ffi_call_plan_*`).

## Shader compiler natives (Minecraft 26.3)

Minecraft 26.3 compiles every shader GLSL → SPIR-V with shaderc and, on OpenGL, back to GLSL with SPIRV-Cross. LWJGL publishes these only for desktop, so `work/natives/scripts/build-shaderc-spvc.sh` builds them for visionOS from the commits the stock LWJGL 3.4.3 natives jars name. They ship as `libshaderc.dylib` and `libspirv-cross.dylib`.

| Component | Pin | License | Upstream |
| --- | --- | --- | --- |
| shaderc (`libshaderc`, target `shaderc_shared`) | `2c8cae778eec0283b44acbe7ed1a386865d78799` (v2026.3) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) | https://github.com/google/shaderc |
| glslang (linked into `libshaderc`) | `168d452a4f460d24b588fed08477a81c44ee27a1` | Several: BSD-3-Clause for most of glslang proper, plus BSD-2-Clause, MIT, Apache-2.0, the NVIDIA/Khronos MIT-style grants and a GPL-3.0-or-later file with the Bison exception, as its `LICENSE.txt` lays out. Full text: [LICENSES/glslang-LICENSE.txt](LICENSES/glslang-LICENSE.txt) | https://github.com/KhronosGroup/glslang |
| SPIRV-Tools (linked into `libshaderc`, optimizer included) | `b707790a898e44038547df54580022fc1cf89c3d` | Apache-2.0 (`LICENSES/Apache-2.0.txt`) | https://github.com/KhronosGroup/SPIRV-Tools |
| SPIRV-Headers (headers, compiled into both) | `29981f65241605e08b0ede4cfeb999fe3b723c6a` | Khronos MIT-style ("Materials") grant; `spir-v.xml` and some files under other terms, as its `LICENSE` lists. Full text: [LICENSES/SPIRV-Headers-LICENSE.txt](LICENSES/SPIRV-Headers-LICENSE.txt) | https://github.com/KhronosGroup/SPIRV-Headers |
| SPIRV-Cross (`libspirv-cross`, target `spirv-cross-c-shared`) | `6c09849fe88c48eaed08413aa022aaa136a3a057` | Apache-2.0 (`LICENSE`); the sources carry `SPDX-License-Identifier: Apache-2.0 OR MIT`, and `GLSL.std.450.h` is MIT (Khronos). This project uses Apache-2.0 (`LICENSES/Apache-2.0.txt`) | https://github.com/KhronosGroup/SPIRV-Cross |

The license files were copied from those pinned checkouts (`work/natives/downloads/shaderc-<commit>/`, `.../third_party/{glslang,spirv-tools,spirv-headers}`, `SPIRV-Cross-<commit>/`).

## Native code and data in the app

| Component | Version / pin | License | Upstream |
| --- | --- | --- | --- |
| ANGLE `libEGL` and `libGLESv2` (xros and xrsimulator, after `angle_retarget.sh`) | `25e721127e1c6c4c6fa0182b5c234b2c88971175` | BSD-3-Clause | https://chromium.googlesource.com/angle/angle. depot_tools `6afa997717b2c0e1382e1465bedbe1a6855b9388`. The delta is `angle-patches/klepton.patch` |
| GraalVM native-image runtime | JDK 25.0.4 | GraalVM Free Terms and Conditions | https://www.oracle.com/downloads/licenses/graal-free-license.html. The JDK tarball is not redistributed. The image contains the runtime |
| libltw | Pojav LTW `7703f1c29a1fd7f151484c38fb7563bb79ca8ccb` | LGPL-3.0, plus the Darwin port in this repo | https://github.com/PojavLauncherTeam/ltw. Commit `work/natives/ltw/darwin/` and the Makefile |
| libopenxr_loader, libvloader | this repo | MIT (the shims) | `work/natives/openxr-shim/` |
| libklegl | this repo | MIT | `work/native-image/scripts/t8b/` and `klegl_bridge.c` |
| libklsdl (SDL3 stand-in) | this repo | MIT | `work/native-image/scripts/t4-src/native/klsdl.c` (includes `klglfw.c`) |
| libshaderc, libspirv-cross | see Shader compiler natives | Apache-2.0 and the notices above | `work/natives/scripts/build-shaderc-spvc.sh` |
| opus4j | `362db14553d39c4dc080e4edd50630b17dbf9682` | JNI glue from the henkelmax repo; Opus is BSD | https://github.com/henkelmax/opus4j |
| rnnoise4j | `5f2097d504fc67f31d0d8900e9df0a5bdf4ec9ae` | JNI glue from the henkelmax repo; RNNoise is BSD-style | https://github.com/henkelmax/rnnoise4j |
| speex4j | `9428cb14b8ec373fcd36f5b5792899611042ece9` | JNI glue from the henkelmax repo; Speex is BSD-style | https://github.com/henkelmax/speex4j |
| lame4j | `b09599989878cbc7349075eca0b3aa4f105e5957` | JNI glue is LGPL-3.0; LAME is LGPL-2.0 | https://github.com/henkelmax/lame4j |
| Phonon HRTF | CIPIC subject 124 as shipped by Valve | Apache-2.0 | Valve steam-audio. File `runtime/data/phonon_hrtf_cipic_124.sofa`, sha256 `c28ff4a874ac889ec0c5885ca524762a70d56984232ff7aadcd9c15d32e1cfb6`. `make phonon-hrtf` refetches it |

Simple Voice Chat's jar also carries these codec notices. Credits: https://modrepo.de/minecraft/voicechat/credits.

| Notice in the jar | What it covers |
| --- | --- |
| `OPUS_LICENSE` | Opus. BSD (Xiph.Org, Skype, and others) |
| `CONCENTUS_LICENSE` | Concentus 1.0.1. BSD-style |
| `RNNOISE_LICENSE` | RNNoise. BSD-style (Jean-Marc Valin, Amazon, Mozilla, Xiph.Org, Mark Borgerding) |
| `SPEEX_LICENSE` | Speex. BSD-style |
| `LAME_LICENSE` | LAME. GNU Library General Public License, version 2 (June 1991) |
| `LAME4J_LICENSE` | LAME4J. LGPL-3.0 |
| `MINIMP3_LICENSE` | minimp3. CC0 1.0 |
| `NATIVE_UTILS_LICENSE` | native-utils 1.0.2. MIT. Copyright (c) 2025 Max Henkel. This grant is only for native-utils |

## LGPL in a native image

LGPL-3.0 section 4 covers a combined work. Shipping one of these libraries inside the static image means the recipient gets the LGPL text, the copyright notices, and a way to relink against a modified copy of that library. The way this repo does that is to publish the patches and the build scripts, and to have the builder supply their own Minecraft client. The client source is not published.

On that path: Vivecraft, Lithium, ImmediatelyFast, Better Biome Blend, Cloth Config, night-config, conditional-mixin, jorbis, Krypton's own code, lame4j, and Pojav LTW.

GPL-3.0 has no equivalent linking exception. More Culling and velocity-native are GPL-3.0 and are in this image. Polyform Shield is not LGPL. Publishing patches does not change Sodium's license. The Sodium text is in this repo because that license asks for the terms to travel with the binary.
