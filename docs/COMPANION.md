# Companion tool and iOS app: design

Status: implemented and used on a real device (see PORTING.md for what a first run showed). Nothing game-derived is ever published; the personal IPA and the game module exist only on the user's machine.

## Flow
1. The user puts the companion executable next to their disc image and runs it (double-click, or drag the image onto it).
2. It downloads the latest release IPA from this project's GitHub Releases page (public asset, no account; the "latest release" API finds it).
3. It finds the executable on the disc image, surveys and translates it locally, cross-compiles the result into a game module, and adds the module to the IPA.
4. It writes a personal IPA next to the disc image, which the user sideloads (AltStore, SideStore, Sideloadly).

## Decisions
- **IPA editing**: an IPA is a zip. Extract, add `Payload/<App>.app/Frameworks/<module>.dylib`, zip again.
- **Module loading**: the app loads the module itself at startup (`dlopen` of a fixed path inside its own bundle). The companion never edits the app binary's load commands. (Runtime side: `XBOX_LIFT_MODULE` for development; the app loads `Frameworks/game-<id>.dylib` from its bundle.)
- **Signing**: the companion signs nothing. Sideload tools re-sign the app and nested code with the user's Apple ID. To verify on a real device before release: that each tool also re-signs a bare dylib under `Frameworks/` (if one does not, ship the module as a framework bundle instead).
- **Integrity**: pin the app version the tool expects, and verify the download with a detached signature (ed25519) whose public key is compiled into the tool. A hash in the release notes alone is weaker: whoever can swap the asset can edit the notes.
- **Toolchain**: clang + lld are fetched on first run from the official LLVM releases (pinned version, hash-checked) rather than bundled. Cached after the first run.
- **Offline / old release**: `--ipa <file>` uses a local IPA instead of downloading.
- **Per-game data** (function lists, patches, hook addresses, sound/input entry points): generated on the user's machine from their disc. Never downloaded from the project.
- **The disc on the phone**: not bundled (4-5 GB breaks sideloading). The app opens it through the Files picker (in place, with a saved bookmark) or from its own Documents folder (Finder/iTunes file sharing). The companion prints that last step when it finishes.
- **User experience**: one executable per OS (Windows, macOS, Linux), a short progress display (download, translate, build, package), and a final "open this file in your sideload tool". No game titles or logos in the tool or on the release page.
- **Privacy**: the tool says plainly that nothing it produces is uploaded anywhere, and never uploads it.

## Engineering notes and risks (found while checking this against the code)
- **Build time and size.** The real translated program is large (tens of thousands of functions). Expect a long cross-compile (tens of minutes on a laptop) and a module in the 100 MB range; the progress display and `-Oz`/`-O1` choices matter. Translation itself takes minutes.
- **Cross-compiling without Apple's SDK.** The translated code needs about ten libm functions and `memset`. The module build must use the project's own minimal headers (`-nostdlibinc`), `--target=arm64-apple-ios`, and lld, with undefined symbols resolved when the app loads it. This is proven and tested in CI (Linux runner with the official LLVM release builds the module, a macOS runner inspects the result with Apple's tools).
- **One-file tool.** The translator and survey are Python (capstone). Per-OS executables come from freezing the tool in CI (three runners). The release asset names and the signature files are part of the release process.
- **Speed on phones.** The test title runs on an A17 Pro (see PORTING.md); frame-rate dips while new textures load remain.
- **Loading time and memory.** A module this size is mapped at launch; check launch time and memory pressure on a real device early.
- **Reading a 4-5 GB image in place.** The app must use security-scoped access and random reads; never copy the file.
- **Keep the personal IPA private.** It contains code derived from the user's own disc. Document that sharing it is not allowed.

## Open questions
- Release signing key rotation (the maintainer holds the private key; see RELEASING.md).
- Whether to also publish a "module only" download for users who already have the app (probably not: it would be game-derived).
- Minimum iOS version to target.

## Per-program patches
A few programs need small byte patches (for example, skipping an engine the runtime does not provide). They are listed in `tools/title_patches.json`, keyed by
the SHA-256 of the program's main executable, with an address and replacement bytes. The companion writes them to `title/patches.txt` before translating, so
the translated code includes them. A program not in the table is translated unmodified and the tool says so. The file holds hashes, addresses and a few bytes
only; adding a program means adding its hash and patches after testing it. `--title-data <folder>` overrides any file of the same name.

## Multi-game library
Constraint: iOS only runs code that is signed into the app, so every game module must be inside the app bundle when it is sideloaded; nothing can be
downloaded into the app's folder later.

How it works:
- **Bundle layout:** `Games/<id>/` per game (module, `title/` files), where `<id>` is the hash of the game's main executable, plus a small `games.json` manifest.
  Single-game apps keep working (a manifest with one entry).
- **App:** a launch screen lists installed games. Each game has its own disc image (found in the app's Documents folder or picked with the Files picker, remembered
  per game) and its own save and settings folders. The runtime keeps global state and cannot unload a module, so choosing a different game relaunches the app.
- **Companion:** `--add-to <personal ipa>` builds only the new game and repackages everything into one personal app, so the earlier games are not rebuilt; an older
  single-game app is converted to this layout (its saved files are moved into the game's folder the first time the game starts). One app instead of one per game keeps the number of sideloaded apps (limited on free Apple IDs) down.
- **Costs:** each game adds roughly 130 MB to the installed app, and adding a game means reinstalling the app (saves stay if the app identity is unchanged).
- **Compatibility:** each new game will need its own testing and patch entries; the table above is per executable hash for that reason.
- **Moving games into a newer app:** `--ipa <newer app> --add-to <old app>` carries the games of the old app (modules and files) into the newer app without rebuilding
  them; `--add-to` can be repeated, and a game that is already in one of those apps is reused instead of rebuilt. Use apps of the same release series.
- **Relay default:** `--relay <host:port>` (and `--relay-room`) builds a default System Link relay into the app's manifest (see RELAY.md).


## Game metadata
The companion reads what a program says about itself from its own executable: the title, title id, regions, version and build date from its certificate, and the
128x128 title image (`tools/xbe_meta.py`). They are written into `Games/games.json` and `Games/<id>/icon.png` of the personal app and shown in the library. Nothing is
downloaded. Richer data (cover art, descriptions, release years) would need an online database; that is not implemented and, if it is ever added, would be opt-in.
