# Reforged v0.2.0 (early engineering preview)

A portable runtime and static translator for running original-hardware console software you own on modern devices. This release adds a game library, a settings
screen, controller handling, System Link over the Internet, and a number of rendering and stability fixes.

**Read [docs/USAGE.md](https://github.com/lPuffyy/reforged/blob/main/docs/USAGE.md) before you start.** To build from source see
[docs/BUILDING.md](https://github.com/lPuffyy/reforged/blob/main/docs/BUILDING.md).

**Upgrading from v0.1.1:** download the new companion and run it again with your disc image (an older single-game app is converted to the new library layout the first time
you add a game to it with `--add-to`). The app keeps its saves as long as you install the new one over the old one.

## What's new
- **Game library.** The app shows the games inside it, with each game's icon, title, title id, regions and build date, all read from your own disc. Disc images are
  recognised by their contents (any `.iso` or `.xiso` name), the file picker has an *open in place* and a *copy into the app* mode and tells you why it fails, and each game
  keeps its own saves, settings and caches. `reforged-companion --add-to <your app>` adds another game without rebuilding the first.
- **Settings screen** (replaces most of the environment variables): on-screen controls (automatic, always, never), a frame-rate counter, render resolution (1x, 2x, 3x:
  higher values smooth jagged edges but use more battery), System Link relay settings, and a Diagnostics switch for bug reports.
- **Controllers.** The on-screen controls hide while a physical controller is connected and come back when it disconnects.
- **System Link over the Internet.** The runtime can connect to a relay server (rooms, abuse limits, no encryption: see
  [docs/RELAY.md](https://github.com/lPuffyy/reforged/blob/main/docs/RELAY.md)), or host one on the device. The app comes with the project's relay filled in, off until you turn it on.
  The `--relay` companion option builds a different default into an app. A phone found and joined a System Link game hosted on a computer through the relay and the match started; a long match has not been tested.
- **Fixes found by testing on a phone:** worlds that went black or blown out to white (depth sharing between render targets, partial-channel screen clears), the shield-bar
  graphic, System Link queries being ignored (the console LAN key is now the same on every console), the picture freezing after minimising the app (the swapchain is rebuilt),
  and a crash from a function the translator had missed (the survey now finds long runs of back-to-back functions).
- The screen stays awake while a game runs. The companion keeps a window open at the end when double-clicked.

## Known problems
- Occasional wrong lighting or post-processing in some places on the phone; not every part of the test title has been played.
- Frame-rate dips while new textures load. Real MSAA and anisotropic filtering are not implemented; render resolution is the antialiasing.
- Leaving the app pauses the game, and a System Link match drops about 30 seconds later.
- Other titles are untested and will probably need work (missing kernel functions, graphics features, audio hooks, translator gaps). See the compatibility list.
- Hardware controller support is written but has not been tried with a real controller. Official online services are not supported. Free Apple IDs expire sideloaded apps after 7 days.

## What this is not
It contains no game code, console firmware, keys or assets, and is not affiliated with or endorsed by any console maker or game publisher. You must supply a disc you own.
Everything the companion makes is for your own use; do not share personal apps. See
[docs/LEGAL.md](https://github.com/lPuffyy/reforged/blob/main/docs/LEGAL.md).

## Compatibility and reports
Results for tested programs are kept in [reforged-compat](https://github.com/lPuffyy/reforged-compat). The companion prints the executable hash that identifies your build.
When reporting a problem include device, iOS version, sideloading tool, the companion's output and `reforged.log` (turn Diagnostics on first). Never attach disc images or
anything made from them.
