# Reforged

A portable runtime and static translator for running original-hardware console software you own, on modern devices.

**Status: early engineering preview.** One real retail title boots and plays on Windows (x64) and on an iPhone (A17 Pro): menus, campaign and System Link
multiplayer (also over the Internet through a relay) run from a personal app built on the user's own machine. Other titles are untested and will probably need work.
Known problems: occasional wrong lighting or post-processing on the phone's GPU path, frame-rate dips while new textures load, and no real MSAA or anisotropic
filtering yet. See **[docs/USAGE.md](docs/USAGE.md)** to get it running and [docs/PORTING.md](docs/PORTING.md) for the open work.

## What the iOS app has
- **A game library**: one app holds several games, each with its icon, title and details read from your own disc, and its own saves, settings and caches.
- **A settings screen**: on-screen controls (automatic, always, never), a frame-rate counter, render resolution (1x, 2x, 3x), System Link relay options and a Diagnostics switch.
- **Controls**: on-screen sticks, buttons and a direction pad; they hide while a physical controller is connected and return when it disconnects.
- **System Link over the Internet**: connect to a relay server or host one from the app ([docs/RELAY.md](docs/RELAY.md)). A relay is filled in by default and is off until you turn it on.
- **Stays awake while playing, and survives leaving the app**: the game pauses in the background and the picture comes back when you return.

## Start here
| I want to... | Go to |
|---|---|
| **Play** on an iPhone or iPad | [docs/USAGE.md](docs/USAGE.md): the step-by-step guide |
| **Sideload** the app (install it without the App Store) | [AltStore guide](https://faq.altstore.io), [SideStore guide](https://docs.sidestore.io), [Sideloadly](https://sideloadly.io) |
| See **what has been tested** | [Compatibility list](https://github.com/lPuffyy/reforged-compat) |
| **Compile** the runtime, the iOS app or the companion myself | [docs/BUILDING.md](docs/BUILDING.md) |
| Play **System Link over the Internet** (relay server) | [docs/RELAY.md](docs/RELAY.md) |
| Understand the **rules** and the legal basis | [docs/LEGAL.md](docs/LEGAL.md) |
| See how it **works** and what is still open | [docs/COMPANION.md](docs/COMPANION.md), [docs/PORTING.md](docs/PORTING.md) |

## What this is not
- It contains **no game code, no console firmware, no keys and no assets.** You bring your own legally obtained disc image.
- It is not affiliated with or endorsed by any console maker or game publisher.
- It does not connect to any official online service.

See [docs/LEGAL.md](docs/LEGAL.md).

## How it works
1. **Survey**: `tools/lift_survey.py` finds every function in the user's executable (recursive descent, data pointers, tables, padding).
2. **Translate**: `tools/lift.py` turns each x86 function into portable C (registers and flags as locals, guest memory through `G()`),
   run once, locally, on the user's machine. The output is personal-use only and never leaves it.
3. **Runtime** (`src/`): kernel, graphics (NV2A to Vulkan), audio, network and input layers that the translated code calls into.
4. **Target**: a native build on Windows, and on iOS a clean generic app plus an arm64 game module that the companion tool builds from the
   user's own disc and injects into a personal copy of the app (see below).

## iOS: the companion tool
The released app contains no game code. `tools/companion.py` (one file per OS from the release page) takes your disc image and, on your computer:
reads the executable, scans it for audio and controller routines, translates it, cross-compiles the game module for iOS with a downloaded
clang + lld, and writes a **personal IPA** next to the disc image. You sideload that file with [AltStore](https://faq.altstore.io), [SideStore](https://docs.sidestore.io) or [Sideloadly](https://sideloadly.io) and put the disc
image in the app's folder (Files app) or pick it in the app. Nothing it produces is uploaded, and the personal IPA must not be shared.
Releases are signed with an Ed25519 key; the companion checks the signature on what it downloads. Details: [docs/COMPANION.md](docs/COMPANION.md),
[docs/RELEASING.md](docs/RELEASING.md).

Signed releases are on the releases page. In between, CI publishes unsigned *preview* builds of the current code (generic app and toolchain packs, no game code)
as pre-releases that the companion can use with `--ipa` and `--toolchain`. Small per-program byte patches live in `tools/title_patches.json`, keyed by the SHA-256 of the main
executable (a program that is not listed is translated without patches); `--title-data` still accepts a folder of your own files.

Full instructions, controls, settings and troubleshooting: **[docs/USAGE.md](docs/USAGE.md)**. To put several games in one app, run the companion again for each
disc with `--add-to <your app>` (games already in it are kept, and are not rebuilt); with `--ipa <newer app> --add-to <old app>` the games are moved into a newer app
without rebuilding them. See [docs/COMPANION.md](docs/COMPANION.md).

**Compatibility list:** what has been tried and how it ran is kept in a separate repository, [reforged-compat](https://github.com/lPuffyy/reforged-compat).

## Layout
| Path | What |
|---|---|
| `src/` | Runtime: kernel HLE, memory model, translated-code runtime (`lift_rt`), GPU/audio/net/input layers |
| `tools/` | Survey, translator, companion tool, module builder, kernel stub generator, XBE/ISO inspection, LAN relay |
| `ios/` | The iOS app shell (UIKit, Metal layer via MoltenVK, touch controls, disc picker) and the IPA build script |
| `docs/` | Usage and building guides, legal rules, porting notes, companion design, release process |

## Building
Full, step-by-step instructions for every platform are in **[docs/BUILDING.md](docs/BUILDING.md)**. The short version (CMake 3.20+, Python 3.9+, a C++17 compiler):
```
git clone https://github.com/lPuffyy/reforged.git
git clone --depth 1 --branch v1.3.280 https://github.com/KhronosGroup/Vulkan-Headers.git vk-headers
cd reforged
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVULKAN_HEADERS_DIR=../vk-headers/include
cmake --build build --parallel
build/reforged none none 2      # starts, says it has nothing to load, exits with code 1: the build works
```
(Windows: add `--config Release` to the second command; the program is `build/Release/reforged.exe`. arm64 machines also need sse2neon: see the guide.)
CI runs a hygiene check on every push (`python -I tools/check_clean.py`), builds on four platforms, builds and runs the iOS app in the Simulator
with an original test program, and tests the companion, signing and toolchain packs.

## Contributing
Read [docs/LEGAL.md](docs/LEGAL.md) first. Never add game-derived data. Test with homebrew, not retail discs.
