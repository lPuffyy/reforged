# Reforged

A portable runtime and static translator for running original-hardware console software you own, on modern devices.

**Status: early engineering preview.** It boots and runs a real retail title natively on Windows (x64 and x86). The long-term goal is a
clean app for iOS that needs no emulator JIT: the user's own disc is translated to native code on their PC and injected into the app.

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
4. **Target**: a native build today; an arm64 module injected into a clean, user-signed app for iOS is the goal.

## Layout
| Path | What |
|---|---|
| `src/` | Runtime: kernel HLE, memory model, translated-code runtime (`lift_rt`), GPU/audio/net/input layers |
| `tools/` | Survey, translator, kernel stub generator, XBE/ISO inspection, LAN relay |
| `docs/` | Legal rules, porting notes |

## Building
Windows only for now (MSVC, Vulkan headers). The portable layer is in progress: see [docs/PORTING.md](docs/PORTING.md).
CI runs a hygiene check on every push (`python -I tools/check_clean.py`) and a compile check.

## Contributing
Read [docs/LEGAL.md](docs/LEGAL.md) first. Never add game-derived data. Test with homebrew, not retail discs.
