# Using Reforged on an iPhone or iPad

This guide takes you from nothing to a running game on your own device. It assumes you own the disc you are going to use and have made an image of it
yourself (see [LEGAL.md](LEGAL.md)). To compile the project yourself instead, see [BUILDING.md](BUILDING.md). Reforged contains no game code, firmware, keys or assets, and never will.

**Status: early engineering preview.** One test title is known to run (about 28 frames per second on a recent iPhone). Other titles are untested and
will probably need work. Expect rough edges, and read [Known problems](#known-problems) first.

## What you need
| | |
|---|---|
| A computer | Windows, macOS or Linux, 64-bit, with about 10 GB of free space and an internet connection for the first run. |
| A disc image | An `.iso` of a disc you own, made with a tool of your choice (the extracted-folder form is not supported yet). |
| An iPhone or iPad | Recent and fast: an A15 or newer is recommended, iOS 15 or later. The test device is an iPhone with an A17 Pro. |
| A sideloading tool | [AltStore](https://altstore.io), [SideStore](https://sidestore.io) or [Sideloadly](https://sideloadly.io), with a (free) Apple ID. Free Apple IDs expire apps after 7 days; re-sign them with the same tool. |
| Storage on the device | About 200 MB for the app (the personal app file is about 35 MB; the game module inside it is about 130 MB once installed) plus the size of the disc image (4-8 GB). |

## How it fits together
The app you download from this project is **generic**: it has the runtime but no game. The **companion tool** runs on your computer, reads your disc image,
translates the game's program into code for your device, and adds it to a personal copy of the app. You install that personal copy with your sideloading
tool and give it the disc image. Nothing is uploaded at any step, and the personal copy contains code made from your disc, so **do not share it**.

## Step by step

### 1. Get the companion tool
Download the file for your system from the project's [Releases page](https://github.com/lPuffyy/reforged/releases/latest): `reforged-companion-windows-x64.exe`, `reforged-companion-macos-arm64` (Apple silicon) or `reforged-companion-linux-x64`. On macOS and Linux make it executable first (`chmod +x <file>`); on macOS you may have to allow it in System Settings > Privacy & Security the first time, because it is not signed by Apple. `companion.sha256` on the same page lets you check the download.
The tool checks an Ed25519 signature on everything it downloads from this project.

### 2. Put the disc image next to it
Copy your `.iso` into the same folder. If the folder has more than one `.iso`, the first one (by name) is used; to choose, run the tool from a terminal with
`--iso <file>`.

### 3. Run it
- Double-click it, or run it in a terminal. A window shows seven steps and stays open at the end so you can read the result.
- First run only: it downloads a signed toolchain pack (a compiler and linker, a few hundred MB) and the generic app, and keeps them for later runs.
- The slow part is step 6, building the game module. Expect tens of minutes on a laptop; the module is large (about 130 MB).
- If your disc is a build the project has tested, the tool applies its small patch list automatically and says so ("known build"). Any other build is
  translated without patches and may not run; the tool tells you.

When it finishes it prints the path of the file it made, `<your image name>-personal.ipa`, next to the disc image.

### 4. Install the personal app
Open `*-personal.ipa` with your sideloading tool and install it, as you would any sideloaded app. Keep this file private. Each tool has its own install guide:
- [AltStore](https://faq.altstore.io) (needs AltServer running on a computer on the same network to refresh apps)
- [SideStore](https://docs.sidestore.io) (refreshes on the device itself after setup)
- [Sideloadly](https://sideloadly.io) (a desktop app; you reinstall every 7 days with a free Apple ID)

### 5. Give the app your disc image
When you open the app it shows your **library**: the games inside this copy of the app, each with its icon, title and a line about its disc image. Pick one:
- **Copy it into the app's folder.** In Finder (macOS, device selected, Files tab) or iTunes (Windows) file sharing, or on the device in the Files app under
  *On My iPhone > Reforged*, copy the `.iso` in. Any file name works as long as it ends in `.iso` or `.xiso`; the app recognises which game it belongs to by looking
  inside it. Copying a large file takes a while; wait for it to finish before opening the app.
- **Choose it in the app.** Tap the **(i)** button next to the game. *Open in place* uses the file where it is (nothing is copied; the app remembers it).
  *Copy into the app* makes a copy inside the app (needs free space for a second copy) and works even when the picker greys the file out. If the file you pick is
  not this game's disc, the app says so.

### 6. Play
Tap the game. The first minutes are slower while shader and pipeline caches are built and saved (`pipeline_cache.bin` in the game's folder); later launches start faster.
To play a different game, close the app and open it again (the runtime can only run one game per start).

### Adding more games
The companion can add a game to an app you already have: run it with `--add-to <your personal app file>` and the new game's disc image. The games already in the app
stay, and you install the result over the old app. Each game keeps its own saves and settings in `Games/<id>/` inside the app's folder, and a single-game app made by an
older companion is converted automatically the first time you add a game to it (its saves stay with it). Each game adds about 130 MB to the app.

To update the app itself without rebuilding your games, download or build a newer app and run the companion with `--ipa <newer app> --add-to <your old personal app>` (add
`--add-to` more than once to bring several apps together). The games are moved into the newer app; use apps from the same release series. If the disc you give is a game
that is already in one of those apps, its translated code is reused and nothing is rebuilt.

## Controls
Touch controls fill the screen, laid out like a controller:
- Left stick (bottom left) and right stick (bottom right) with **L3** / **R3** buttons beside them, and a **direction pad** (four arrow buttons) above the left stick.
- **A B X Y** on the right, **LT** and **RT** (triggers) at the top corners, **WHT** and **BLK** (the two shoulder-style buttons), **BACK** and **START** at the bottom centre.
- Short taps press buttons; the sticks work by dragging.

**Physical controllers** (Xbox, PlayStation, and other MFi-compatible pads) work: connect one in iOS Settings > Bluetooth and the on-screen controls hide automatically; they come back when the controller disconnects. You can change this in the app's Settings (*Automatic*, *Always show*, *Always hide*). The shoulder buttons are the original pad's white and black buttons.

## Settings
Tap **Settings** in the library:
- **On-screen controls:** automatic, always show, or always hide.
- **Display:** a frame-rate counter (top left while a game runs) and **render resolution** (1x, 2x, 3x). Higher resolutions draw the game larger and scale it down, which
  smooths jagged edges (it works like supersampling antialiasing) but uses more battery and may lower the frame rate. It applies the next time a game starts.
- **System Link over the Internet:** relay server and room name (see below).
- **Diagnostics:** saves occasional frame captures and detailed logs (turn it on when asked to report a problem).

## System Link over the Internet
The game's own System Link feature can run over the Internet through a relay server that one of the players hosts. In Settings, turn on *Use a relay server* and enter
the relay's address and a room name that everyone who plays together shares, then use the game's System Link menu. Setting up the relay and the safety notes are in
[RELAY.md](RELAY.md). The project's relay (`relay.puffsco.com`) is already filled in; you only need to turn the switch on. This is early and has been tested between two copies of the runtime on a computer, not yet on a phone.

## Files and diagnostics
The app writes these files into its folder (visible in the Files app):
| File | What |
|---|---|
| `reforged.log` | What the runtime did: start-up, errors, a line per second with frame rate and draw counts. **Attach it when you report a problem.** |
| `Games/<id>/` | One folder per game: its virtual disk, saves, console identity, `pipeline_cache.bin` and any diagnostic captures. |
| `pipeline_cache.bin` | GPU pipeline cache (inside each game's folder). Safe to delete (it is rebuilt, with a slow first minute). |
| `frame_*.bmp` | Occasional captured frames (every 2400 frames and whenever the screen suddenly goes black). Safe to delete. |
| `tex*.bmp` | Small debug texture dumps. Safe to delete. |
| `reforged.env` | **You create this.** Lines of `NAME=value` set runtime options before the game starts; lines starting with `#` are comments. The log tells you whether it was found. |

Options you may want in `reforged.env` (all optional):
```
# turn the automatic frame capture off
XBOX_FRAMEDUMP=
XBOX_DRAWLOG=
XBOX_DARKWATCH=
```
More options exist for debugging the renderer; they are listed in the source (`src/gpu_vk.cpp`) and change often.

## Updating
- **A new app release:** download the new companion from the Releases page and run it again with the same disc image. It builds a new personal app;
  install it over the old one with the same sideloading tool (your save data stays as long as the app's identity does not change).
- **Re-signing:** with a free Apple ID the app expires after 7 days. AltStore and SideStore can refresh it automatically; Sideloadly needs you to reinstall.

## Privacy and legal
- The companion tool and the app never upload anything. The app does not connect to any official online service.
- The personal app contains code derived from your disc: it is for your own use on your own devices and must not be shared or posted.
- This project is not affiliated with any console maker or game publisher. See [LEGAL.md](LEGAL.md).

## Known problems
This is a preview. Things you should expect:
- Some visual effects are still wrong on the phone's graphics path (occasional black or too-bright areas in some places, and a few missing post-processing effects).
- Not every part of the test title has been played; some levels may hang or crash. Please report them with `reforged.log`.
- Frame rate is limited by the game itself (30 frames per second); dips happen when new textures load.
- A game runs once per app start; to switch games, close the app and open it again.
- Official online play is not available. System Link over the Internet through a relay is early (see [RELAY.md](RELAY.md)).
- The file picker may grey out the disc image for some storage locations; use *Copy into the app* or copy the file with the Files app. If the picker fails the app tells you why (that message is useful in a report).

## Troubleshooting
| Symptom | What to do |
|---|---|
| The companion says no disc image was found | Put the `.iso` in the same folder, or run `<companion file> --iso <file>`. |
| "not one the project has tested" | The disc is a different build from the tested one. It may still work; if it does not, there is nothing to tune yet. |
| The download step fails | Check your connection; run again. To use a file you already have: `--ipa <app file>` and `--toolchain <unpacked folder>`. |
| The build step fails | Make sure there are 10 GB free, and run again; send the last lines of the tool's output (never the disc image or the generated files). |
| The sideloading tool says the app is invalid | The personal app must be installed exactly as written (do not unzip or rename it); try another sideloading tool. |
| A game shows "no disc image yet" | Do step 5. |
| The app opens and closes | Open the Files app, copy out `reforged.log` and read the last lines; attach it when reporting. |

## Compatibility reports
The companion prints the **executable hash (SHA-256)** of your disc's main program near the start. That value identifies the exact build, so it is what a compatibility report is keyed by. The compatibility list lives in a separate repository, [reforged-compat](https://github.com/lPuffyy/reforged-compat) (results only, no game files): look your hash up there, and send your own result with its report form.

## Reporting problems
Open an issue on the project's page with: device model and iOS version, which sideloading tool, the companion's output, and `reforged.log`.
**Never attach the disc image, the personal app, or anything made from them.** Screenshots and short clips are welcome.
