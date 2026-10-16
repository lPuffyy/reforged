# Reforged v0.1.0 (early engineering preview)

A portable runtime and static translator for running original-hardware console software you own on modern devices. This first release makes it possible to
build a personal iOS app from your own disc image and play on a recent iPhone or iPad.

**Read [docs/USAGE.md](https://github.com/lPuffyy/reforged/blob/main/docs/USAGE.md) before you start.** It covers requirements, the five steps, controls, settings, updating and troubleshooting.

## What is in this release
- **The generic app** (`Reforged-v0.1.0.ipa`, with `.sig` and `.sha256`): the runtime, iOS shell, touch controls and the Vulkan-on-Metal layer. It contains no game code.
- **The companion tool** (`reforged-companion` for macOS and Linux, `reforged-companion.exe` for Windows): reads your disc image, translates the game's program on your
  computer, builds the game module for iOS, and writes a personal app you sideload (AltStore, SideStore or Sideloadly).
- **Toolchain packs** (compiler and linker, signed): downloaded once by the companion.

## What works
- One test title runs on an A17 Pro iPhone at its native 30 frames per second: menus, single-player and multiplayer, audio, HUD and touch controls.
- Pipeline and shader caches are saved between runs, so later starts are faster.
- The companion applies known per-program patches automatically (matched by a hash of the main executable) and says plainly when a build is not one it knows.

## Known problems
- Occasional wrong lighting or post-processing in some places on the phone's GPU path; some levels untested and may hang.
- Frame-rate dips while new textures load.
- Only one game per personal app (a multi-game library is planned).
- Hardware controllers and online play are not supported.
- The disc picker may grey out the file on some iOS versions; copy the image into the app's folder instead.
- Free Apple IDs expire sideloaded apps after 7 days.

## What this is not
It contains no game code, console firmware, keys or assets, and is not affiliated with or endorsed by any console maker or game publisher. You must supply a disc
you own. Everything the companion makes is for your own use; do not share personal apps. See [docs/LEGAL.md](https://github.com/lPuffyy/reforged/blob/main/docs/LEGAL.md).

## Reporting problems
Include device model, iOS version, sideloading tool, the companion's output and `reforged.log` from the app's folder. Never attach disc images or anything made from them.
