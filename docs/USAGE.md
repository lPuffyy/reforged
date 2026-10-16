# Using Reforged on an iPhone or iPad

This guide takes you from nothing to a running game on your own device. It assumes you own the disc you are going to use and have made an image of it
yourself (see [LEGAL.md](LEGAL.md)). Reforged contains no game code, firmware, keys or assets, and never will.

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
Download the file for your system from the project's Releases page: `reforged-companion` (macOS and Linux) or `reforged-companion.exe` (Windows).
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
Open `*-personal.ipa` with AltStore, SideStore or Sideloadly and install it, as you would any sideloaded app. Keep this file private.

### 5. Give the app your disc image
Pick one:
- **Copy it into the app's folder.** In Finder (macOS, device selected, Files tab) or iTunes (Windows) file sharing, or on the device in the Files app
  under *On My iPhone > Reforged*, copy the `.iso` in. Any file name works as long as it ends in `.iso`; if there are several, the first by name is used.
  Copying a large file takes a while; wait for it to finish before opening the app.
- **Choose it in the app.** If the app has no disc yet it shows a **Choose disc image** button; pick the file with the Files picker. The app remembers
  the choice and reads the file in place (no copy). If your `.iso` appears greyed out in the picker, use the copy method instead and tell us your iOS version.

### 6. Play
Open the app. It starts the game straight away. The first minutes are slower while shader and pipeline caches are built and saved
(`pipeline_cache.bin` in the app's folder); later launches start faster.

## Controls
Touch controls fill the screen, laid out like a controller:
- Left stick (bottom left) and right stick (bottom right) with **L3** / **R3** buttons beside them.
- **A B X Y** on the right, **LT** and **RT** (triggers) at the top corners, **WHT** and **BLK** (the two shoulder-style buttons), **BACK** and **START** at the bottom centre.
- Short taps press buttons; the sticks work by dragging.

Hardware controllers are not supported yet.

## Settings and diagnostics
The app writes these files into its folder (visible in the Files app):
| File | What |
|---|---|
| `reforged.log` | What the runtime did: start-up, errors, a line per second with frame rate and draw counts. **Attach it when you report a problem.** |
| `pipeline_cache.bin` | GPU pipeline cache. Safe to delete (it is rebuilt, with a slow first minute). |
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
- Only one game can be installed per personal app for now (a multi-game library is planned; see [COMPANION.md](COMPANION.md)).
- Online play is not available.
- The disc picker may not let you select some files on some iOS versions; the copy method always works.

## Troubleshooting
| Symptom | What to do |
|---|---|
| The companion says no disc image was found | Put the `.iso` in the same folder, or run `reforged-companion --iso <file>`. |
| "not one the project has tested" | The disc is a different build from the tested one. It may still work; if it does not, there is nothing to tune yet. |
| The download step fails | Check your connection; run again. To use a file you already have: `--ipa <app file>` and `--toolchain <unpacked folder>`. |
| The build step fails | Make sure there are 10 GB free, and run again; send the last lines of the tool's output (never the disc image or the generated files). |
| The sideloading tool says the app is invalid | The personal app must be installed exactly as written (do not unzip or rename it); try another sideloading tool. |
| The app shows "no disc image yet" | Do step 5. |
| The app opens and closes | Open the Files app, copy out `reforged.log` and read the last lines; attach it when reporting. |

## Compatibility reports
The companion prints the **executable hash (SHA-256)** of your disc's main program near the start. That value identifies the exact build, so it is what a compatibility report is keyed by. The compatibility list lives in a separate repository, [reforged-compat](https://github.com/lPuffyy/reforged-compat) (results only, no game files): look your hash up there, and send your own result with its report form.

## Reporting problems
Open an issue on the project's page with: device model and iOS version, which sideloading tool, the companion's output, and `reforged.log`.
**Never attach the disc image, the personal app, or anything made from them.** Screenshots and short clips are welcome.
