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

## What is in the app
The runtime, the iOS shell and the Vulkan-on-Metal layer (MoltenVK, Apache-2.0, pinned by SHA-256 in `ios/build_ipa.sh`, license included in the bundle).
Nothing game-derived. The personal copy made by the companion adds the game module and executable on the user's computer.

## If the key is lost or leaked
Make a new pair, update `RELEASE_PUBLIC_KEY`, publish a new companion, and tell users to update it: old companions will refuse apps signed with the new key.
