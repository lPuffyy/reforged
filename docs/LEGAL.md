# Legal and repository rules

This is an engineering project: a portable runtime that executes software the user already owns. It is not legal advice; get an
IP lawyer's opinion before distributing anything beyond this source.

## What this repository contains
- Original source code: a runtime (kernel/graphics/audio/network emulation layers) and tooling (a static translator and its survey).
- Nothing from any game or console: no disc images, executables, BIOS or boot ROM, encryption keys, save data, captured network traffic,
  or code generated from a game.

## What users supply and what stays on their machine
- The user provides their own legally obtained disc image. All translated output is generated locally from it, is personal-use only,
  and is never uploaded, committed or redistributed.
- `.gitignore` and `tools/check_clean.py` (run in CI on every push) enforce this: the build fails if a forbidden file type,
  directory, generated symbol or game name appears in a tracked file.

## Rules for contributors (and for the tooling that helps write this code)
- Never add game-derived data, even "just for a test". Use small homebrew test programs instead.
- No trademarks or game names in the project name, icon, app metadata or documentation.
- Never connect to a console manufacturer's online services. Any online play goes only through community-run services that allow it.
- Network features must never ship credentials, keys or account data.
