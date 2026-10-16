# Releasing

The generic app is published as a GitHub release by `.github/workflows/release.yml` when a tag like `v0.1.0` is pushed. The companion tool downloads it
and checks an Ed25519 signature before using it.

## One-time setup (the maintainer, on their own computer)
1. Make the release key pair. Keep the private key secret and never commit it:
   ```
   openssl genpkey -algorithm ed25519 -out reforged-release.pem
   openssl pkey -in reforged-release.pem -pubout -outform DER | tail -c 32 | xxd -p -c 64
   ```
   The second command prints the public key (64 hex characters).
2. Put the public key in `tools/companion.py` as `RELEASE_PUBLIC_KEY`. Until then the companion refuses to download anything (it can still use `--ipa <file>`).
3. In the repository's Settings, Secrets and variables, Actions: add a secret named `RELEASE_SIGNING_KEY` whose value is the whole contents of `reforged-release.pem`.

## Making a release
```
git tag v0.1.0
git push origin v0.1.0
```
The workflow builds the unsigned app, signs it, checks the signature with the companion's own verifier, and publishes `Reforged-v0.1.0.ipa`, its `.sig` and its `.sha256`.

## Before tagging a release (checklist)
1. CI is green on the commit to tag (all platforms, the Simulator test, the companion, signing and toolchain checks).
2. Build a personal app **with no private files** (`--title-data` omitted) from the newest preview build and play it on a real device: boot, a level start, and the
   in-game menus. The companion should print "known build" for the tested disc.
3. Read `docs/USAGE.md` against the actual behaviour (file names, steps, controls, known problems) and fix anything stale. Update `docs/RELEASE_NOTES_v0.1.0.md`
   (the release workflow uses it as the release text).
4. `python -I tools/check_clean.py` passes: no game names, game-derived files or keys in tracked files.
5. **The signed toolchain packs are published.** The companion downloads its compiler from a release tagged `toolchain`, which is only created by running the `toolchain` workflow manually (Actions, toolchain, Run workflow) with the signing secret set. Do this *before* announcing the release, and confirm the release `toolchain` has three `.zip` files with `.sig` files.
6. The release secret and public key are in place (see one-time setup) and the companion in the release verifies the signed app.
7. Tag and push (below). After the workflow finishes, download the released companion for one OS and run it once, with an empty cache, against a generated test disc (`tests/make_test_iso.py`): it must download the app and the toolchain, verify both signatures and write a personal app.

## What is in the app
The runtime, the iOS shell and the Vulkan-on-Metal layer (MoltenVK, Apache-2.0, pinned by SHA-256 in `ios/build_ipa.sh`, license included in the bundle).
Nothing game-derived. The personal copy made by the companion adds the game module and executable on the user's computer.

## If the key is lost or leaked
Make a new pair, update `RELEASE_PUBLIC_KEY`, publish a new companion, and tell users to update it: old companions will refuse apps signed with the new key.
