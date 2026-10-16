# Building Reforged from source

This page is for people who want to compile the runtime, the iOS app or the companion tool themselves. If you only want to **play**, you do not need any of this:
see [USAGE.md](USAGE.md).

## What you can build
| Part | What it is | Where it runs to build |
|---|---|---|
| **The runtime** (`reforged`) | The command-line runtime: kernel, graphics, audio, network and input layers. Builds on its own, with no game. | Windows x64, Linux x64/arm64, macOS |
| **A game module** | The translated game code, built as a library the runtime loads. Made from *your* disc by the translator; never part of this repository. | Any of the above; the companion does it for you |
| **The iOS app** (`ReforgedApp`) | The generic app: the runtime plus the iOS shell, built unsigned. | macOS with Xcode |
| **The companion tool** | Python program that makes a personal app from your disc. Runs from source, or frozen into one file. | Anywhere Python runs |

## Tools you need
- **Git**, **CMake 3.20 or newer**, **Python 3.9 or newer**.
- A **C++17 compiler**: Visual Studio 2022 (or its Build Tools, "Desktop development with C++") on Windows; `clang` or `gcc` on Linux; Xcode (command-line tools) on macOS.
- **Vulkan-Headers** (headers only, nothing to install): clone the pinned version next to the repository, see below.
- **sse2neon** (arm64 builds only: Apple silicon Macs, Linux arm64, iOS): a single header, clone the pinned version, see below.
- The **Python package `capstone`** (`python -m pip install capstone`) for the translator and the companion.

At run time the runtime loads your system's Vulkan library (a graphics driver on Windows and Linux; MoltenVK on macOS, for example from the LunarG Vulkan SDK).

## 1. Get the sources and the two small dependencies
Run these from a folder *outside* the repository (the versions below are the ones CI uses):
```
git clone https://github.com/lPuffyy/reforged.git
git clone --depth 1 --branch v1.3.280 https://github.com/KhronosGroup/Vulkan-Headers.git vk-headers
git clone --depth 1 --branch v1.8.0 https://github.com/DLTcollab/sse2neon.git sse2neon      # arm64 only
```

## 2. Build the runtime
All commands are run from inside the `reforged` folder. The paths below assume the two clones sit beside it (`../vk-headers`, `../sse2neon`).

### Windows (x64)
Open "x64 Native Tools Command Prompt for VS 2022", or any shell with CMake and the compiler on the path:
```
cmake -S . -B build -DVULKAN_HEADERS_DIR=..\vk-headers\include
cmake --build build --config Release --parallel
```
Result: `build\Release\reforged.exe`.

### Linux (x64 or arm64) and macOS
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVULKAN_HEADERS_DIR=../vk-headers/include
cmake --build build --parallel
```
On arm64 (Apple silicon, Linux arm64) add `-DSSE2NEON_DIR=../sse2neon` to the first command. Result: `build/reforged`.

### Check that it works (no game needed)
```
build/reforged none none 2          # Windows: build\Release\reforged.exe none none 2
ctest --test-dir build -C Release   # the platform-layer tests
```
The first command should start, report that it has no program to load, and exit with code 1. That is the expected "healthy but empty" result (it is CI's smoke test).

## 3. Try the translator with a test program (no disc)
The repository can generate a tiny original program and run it through the whole pipeline, which proves your toolchain end to end:
```
python -m pip install capstone
bash tests/lift_e2e.sh        # run from the repository root, after step 2 has configured CMake
```
It generates the program, translates it, builds the translated code into the runtime *and* as a loadable module, runs both, and compares the output with what the
generator expects. On Windows run it from Git Bash. To do the steps by hand, read the script: it is short and commented.

## 4. The pieces of a real game build
Running a real game needs the same pipeline on *your* disc image. The companion tool does all of it for iOS; for a desktop build use it as the reference:
1. **Survey** (`tools/lift_survey.py default.xbe`) finds the functions.
2. **Translate** (`tools/lift.py default.xbe --out work/lift/gen`) writes C++ for them, locally. This output is game-derived and personal: never commit or share it.
3. **Build** it into the runtime (`-DREFORGED_LIFTED_DIR=<that folder>`) or as a module (`-DREFORGED_LIFTED_AS_MODULE=ON`).
4. **Run** with `XBOX_LIFT=1 reforged <disc image> default.xbe <seconds>`, with `XBOX_TITLE_DIR` pointing at the folder of per-program files the companion writes
   (`tools/gen_title_data.py`, plus any entry from `tools/title_patches.json`).

## 5. Build the iOS app (macOS, Xcode)
```
export SSE2NEON_DIR=$PWD/../sse2neon
export VULKAN_HEADERS_DIR=$PWD/../vk-headers/include
bash ios/build_ipa.sh            # produces Reforged-unsigned.ipa in the repository folder
```
The app is built unsigned on purpose: your sideloading tool signs it. The script also downloads and embeds the Vulkan-on-Metal layer (MoltenVK), checking a pinned
SHA-256 first. `bash tests/ios_app.sh` builds it again for the iOS Simulator and checks that it starts. To make a playable app you still need the companion
(next section) to add a game module to it.

## 6. Run or package the companion tool
```
python -m pip install capstone
python -I tools/companion.py --help                      # options
python -I tools/companion.py --iso game.iso --ipa Reforged-unsigned.ipa --toolchain <unpacked toolchain folder>
```
- Without `--ipa` it downloads the latest signed release; without `--toolchain` it downloads and caches the signed compiler pack.
- To build one self-contained program: `python -m pip install pyinstaller` then `python tools/freeze_companion.py` (output in `dist/`).
- To rebuild the toolchain pack or run the companion's tests, see `tools/make_toolchain_pack.py` and `tests/companion_e2e.sh`.

## Common problems
| Problem | Fix |
|---|---|
| CMake cannot find Python | Install Python 3 and make sure `python` (or `python3`) is on the path. |
| `vulkan/vulkan.h` not found | Pass `-DVULKAN_HEADERS_DIR=<folder containing vulkan/vulkan.h>` (the `include` folder of the clone). |
| `sse2neon.h` not found on arm64 | Pass `-DSSE2NEON_DIR=<folder containing sse2neon.h>`. |
| The iOS build says to run `gen_kernel.py` | Run `python tools/gen_kernel.py` first (the script `ios/build_ipa.sh` does it for you). |
| The runtime says Vulkan was not found | Install your graphics driver's Vulkan support (Windows/Linux) or the Vulkan SDK / MoltenVK (macOS). |
| `check_clean.py` fails | You added a game-derived file or a game name to a tracked file; remove it. Test with homebrew only. |

## Rules for what goes in the repository
Run `python -I tools/check_clean.py` before you commit. It refuses disc images, executables, translated game code, keys, saves and game names. Read [LEGAL.md](LEGAL.md).
