# Reforged

A portable runtime and static translator for running original-hardware console software you own, on modern devices.

**Status: early engineering preview.** A real retail title boots and plays on Windows (x64) and, as of the latest builds, on an iPhone
(A17 Pro): title screen, menus and a multiplayer match run at about 28 frames per second from a personal app built on the user's own machine.
Known problems: some post-processing colours are wrong on the phone's GPU path, some surfaces render white, and some campaign levels hang on load.
See [docs/PORTING.md](docs/PORTING.md) for the open work.

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
clang + lld, and writes a **personal IPA** next to the disc image. You sideload that file with AltStore, SideStore or Sideloadly and put the disc
image in the app's folder (Files app) or pick it in the app. Nothing it produces is uploaded, and the personal IPA must not be shared.
Releases are signed with an Ed25519 key; the companion checks the signature on what it downloads. Details: [docs/COMPANION.md](docs/COMPANION.md),
[docs/RELEASING.md](docs/RELEASING.md).

Until the first release exists, CI publishes unsigned *preview* builds (generic app and toolchain packs, no game code) as pre-releases that the
companion can use with `--ipa` and `--toolchain`. Per-title files the scanner cannot derive can be passed with `--title-data`.

## Layout
| Path | What |
|---|---|
| `src/` | Runtime: kernel HLE, memory model, translated-code runtime (`lift_rt`), GPU/audio/net/input layers |
| `tools/` | Survey, translator, companion tool, module builder, kernel stub generator, XBE/ISO inspection, LAN relay |
| `ios/` | The iOS app shell (UIKit, Metal layer via MoltenVK, touch controls, disc picker) and the IPA build script |
| `docs/` | Legal rules, porting notes, companion design, release process |

## Building
The runtime builds with CMake on Windows, Linux and macOS (arm64 and x64); the iOS app builds with the Xcode generator (`ios/build_ipa.sh`, unsigned).
CI runs a hygiene check on every push (`python -I tools/check_clean.py`), builds on four platforms, builds and runs the iOS app in the Simulator
with an original test program, and tests the companion, signing and toolchain packs. See [docs/PORTING.md](docs/PORTING.md).

## Contributing
Read [docs/LEGAL.md](docs/LEGAL.md) first. Never add game-derived data. Test with homebrew, not retail discs.
