# Porting notes and open work

## Done
- Guest memory model with two layouts: identity (32-bit, game addresses == host addresses) and relocated (64-bit, `G(addr)`), explicit
  MMIO routing (no page-fault trapping), no native fallback in the relocated build.
- Static translator and survey; the relocated build runs a real title through its menus and gameplay.
- Kernel/DirectSound/input HLE, NV2A to Vulkan renderer, NIC emulation with a UDP relay for LAN play.

## Next (in order)
1. **Win32 abstraction**: put threads, events, semaphores, timers, files, memory and time behind a small `plat_*` layer so the
   runtime compiles without `windows.h`. GPU/window/audio stay platform-specific (replaced by the app shell on iOS).
2. **Linux arm64 CI job**: compile and run the portable core with sse2neon (CI on `ubuntu-24.04-arm`).
3. **arm64 translated module**: freestanding C (no libc), host function table, built with clang + lld on the user's PC.
4. **Companion builder**: desktop tool that takes the user's disc, runs survey + translate + compile, and injects `game.dylib` into the
   clean app bundle.
5. **iOS app shell**: Xcode project, Metal/MoltenVK, audio, touch/controller input, disc import. Built in CI on macOS runners (unsigned IPA).
6. **Per-title data** (hooks, patch lists, vtable addresses) moved out of code into generated per-title files.

## Known title-specific constants still in code (to move into per-title data)
- `src/audio_hle.cpp`, `src/dsound_hle.cpp`: address of the stream vtable in the executable.
- `src/lift_rt.cpp`: the thread root function address used by the bisection aid.
- `src/main.cpp`, `src/kernel.cpp`: default `work/title` directory layout.

## Rules
- Generated output (translated C, objects, `work/`) is never committed. `tools/check_clean.py` enforces it.
