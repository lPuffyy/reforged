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

## Next (in order)
1. **Freestanding module build** so the companion can cross-compile for iOS on Windows/Linux without Apple's SDK (own minimal headers for the few libm
   functions and `memset` the translated code uses, `-nostdlibinc`, lld).
2. **Companion builder**: desktop tool that takes the user's disc, runs survey + translate + compile, and injects `game.dylib` into the
   clean app bundle.
3. **iOS app shell**: Xcode project, Metal/MoltenVK, audio, touch/controller input, disc import. Built in CI on macOS runners (unsigned IPA).
4. **Per-title data** (hooks, patch lists, vtable addresses) moved out of code into generated per-title files.

## Known title-specific constants still in code (to move into per-title data)
- `src/audio_hle.cpp`, `src/dsound_hle.cpp`: address of the stream vtable in the executable.
- `src/lift_rt.cpp`: the thread root function address used by the bisection aid.
- `src/main.cpp`, `src/kernel.cpp`: default `work/title` directory layout.

## Rules
- Generated output (translated C, objects, `work/`) is never committed. `tools/check_clean.py` enforces it.
