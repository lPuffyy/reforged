# Companion tool and iOS app: design

Status: design agreed, not implemented. Nothing game-derived is ever published; the personal IPA and the game module exist only on the user's machine.

## Flow
1. The user puts the companion executable next to their disc image and runs it (double-click, or drag the image onto it).
2. It downloads the latest release IPA from this project's GitHub Releases page (public asset, no account; the "latest release" API finds it).
3. It finds the executable on the disc image, surveys and translates it locally, cross-compiles the result into a game module, and adds the module to the IPA.
4. It writes a personal IPA next to the disc image, which the user sideloads (AltStore, SideStore, Sideloadly).

## Decisions
- **IPA editing**: an IPA is a zip. Extract, add `Payload/<App>.app/Frameworks/<module>.dylib`, zip again.
- **Module loading**: the app loads the module itself at startup (`dlopen` of a fixed path inside its own bundle). The companion never edits the app binary's load commands. (Runtime side already exists: `XBOX_LIFT_MODULE`; the app will default to the bundle path.)
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
- **Cross-compiling without Apple's SDK.** The translated code needs about ten libm functions and `memset`. The module build must use the project's own minimal headers (`-nostdlibinc`), `--target=arm64-apple-ios`, and lld, with undefined symbols resolved when the app loads it. This is not yet proven; it will be tested in CI (Linux runner with the official LLVM release builds the module, a macOS runner inspects the result with Apple's tools).
- **One-file tool.** The translator and survey are Python (capstone). Per-OS executables come from freezing the tool in CI (three runners). The release asset names and the signature files are part of the release process.
- **Speed is unproven on phones.** Translated code has only run on desktop CPUs. Performance work may be needed.
- **Loading time and memory.** A module this size is mapped at launch; check launch time and memory pressure on a real device early.
- **Reading a 4-5 GB image in place.** The app must use security-scoped access and random reads; never copy the file.
- **Keep the personal IPA private.** It contains code derived from the user's own disc. Document that sharing it is not allowed.

## Open questions
- Release signing key: who holds it, and how it is rotated.
- Whether to also publish a "module only" download for users who already have the app (probably not: it would be game-derived).
- Minimum iOS version to target.
