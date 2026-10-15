# Porting notes and open work

## Done
- Guest memory model with two layouts: identity (32-bit, game addresses == host addresses) and relocated (64-bit, `G(addr)`), explicit
  MMIO routing (no page-fault trapping), no native fallback in the relocated build.
- Static translator and survey; the relocated build runs a real title through its menus and gameplay.
- Kernel/DirectSound/input HLE, NV2A to Vulkan renderer, NIC emulation with a UDP relay for LAN play.

- **Platform layer** (`src/plat.h`, `plat_win.cpp`, `plat_posix.cpp`): time, threads, waitable objects (events/semaphores/mutexes/threads),
  virtual memory, shared memory aliases, files and directories, dynamic libraries, audio output, process spawning, crash handler. The
  runtime no longer includes `windows.h` outside the Windows backends (`plat_win`, `input_win`, `display_win`) and the native-fallback
  debugging code that only exists in the 32-bit identity build.
- **Backends split by platform**: `display.h` (window/Vulkan surface), `input.h` (pad state: XInput + keyboard on Windows, pushed by the
  app shell elsewhere), `net_sock.h` (Winsock vs BSD sockets).
- **Portable build**: `CMakeLists.txt` (`LIFT_NO_NATIVE`, `LIFT_EXPLICIT_MMIO`); CI builds it on Windows x64, Linux x64 and Linux arm64.

- **Verified in CI** (Windows x64, Linux x64, Linux arm64): the runtime builds and links, `tests/plat_test.cpp` (95 behavioural checks of the
  platform layer) passes, and the runtime starts, maps the guest address space and exits cleanly with no game present.

- **Translated code runs on arm64 and as a module** (CI, all four platforms): `tests/make_test_xbe.py` generates an original test program; CI surveys and
  translates it, runs it linked into the runtime and again as a separate module loaded through the host function table (`HostApi` in `lift_rt.h`,
  `XBOX_LIFT_MODULE=path`), and checks the output against values computed independently. On the macOS runner the module is also cross-compiled
  and linked for iOS arm64 with the iPhoneOS SDK. Portable builds never request executable memory (W^X on Apple Silicon / iOS).
- **iOS module builds without Apple's SDK** (CI, Linux runner): `tools/build_module.py` uses plain clang + lld, our own minimal headers
  (`src/freestanding/`) and `--target=arm64-apple-ios15.0`; the result is checked as an arm64 iOS Mach-O that exports `reforged_module_init` and
  imports only libc/libm functions (allow-list in `tests/ios_module_cross.sh`). The test program's module builds in 4 seconds.

- **Companion tool, iOS app and release pipeline** (all in CI): the personal IPA is built by `tools/companion.py` (frozen to one file per OS), the app
  runs the translated test program in the iOS Simulator, releases are signed (Ed25519) and verified by the companion, and the toolchain packs are
  built per OS and tested by cross-compiling the test module with only what is inside them.
- **First run on a phone**: a retail title boots, reaches the menus and plays a match at about 28 fps on an A17 Pro. Things only a real device showed:
  Apple's arm64 ABI packs stack arguments (9th onward) by size, not in 8-byte slots (kernel import bridge, `lift_rt.cpp`); a static-library link
  dropped files that only self-register hooks (the app force-loads the runtime library); the translator must be given the title's byte patches;
  the survey must follow function-pointer tables that point at bare `jmp` stubs and packed leaf functions with no padding.

## Open work
1. **Rendering differences on the phone** (MoltenVK/Metal): a wrong tint on some full-screen post effects, white surfaces in places. Plan: dump the
   final frame and the intermediate render targets on the device and compare them with the same frame on Windows; switches in a `reforged.env`
   file for suspects (shader precision, blend factors, render-to-texture sampling, vertex colour order, swapchain colour space).
2. **Some campaign levels hang on load** (black screen, the game stays responsive to input).
3. **Disc picker**: the Files picker still shows the disc image as not selectable; placing it in the app's Documents folder as `game.iso` works.
4. **Shader compile stalls** in the first seconds (pipeline cache), then performance work.
5. **Per-title data** (hooks, patch lists, vtable addresses) moved out of code into generated per-title files; the companion's `--title-data`
   option is the first step. The title-specific constants below are still in code.

## Next
- First signed release, after the checks above.

## Known title-specific constants still in code (to move into per-title data)
- `src/audio_hle.cpp`, `src/dsound_hle.cpp`: address of the stream vtable in the executable.
- `src/lift_rt.cpp`: the thread root function address used by the bisection aid.
- `src/main.cpp`, `src/kernel.cpp`: default `work/title` directory layout.

## Rules
- Generated output (translated C, objects, `work/`) is never committed. `tools/check_clean.py` enforces it.
