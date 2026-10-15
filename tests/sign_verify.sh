#!/usr/bin/env bash
# The release signing process, end to end: a key pair made with OpenSSL, a file signed the way the release workflow signs the app, and the companion's pure
# Python verifier (tools/ed25519.py) accepting it with the raw public key and rejecting a tampered file. Run from the repository root.
set -euo pipefail
root="$(pwd)"
mkdir -p sv && cd sv
openssl version
openssl genpkey -algorithm ed25519 -out key.pem
pub="$(openssl pkey -in key.pem -pubout -outform DER | tail -c 32 | xxd -p -c 64)"
echo "public key: $pub"
head -c 300000 /dev/urandom > app.bin
openssl pkeyutl -sign -rawin -inkey key.pem -in app.bin -out app.bin.sig
PUB="$pub" python -I - <<PYEOF
import os, sys
sys.path.insert(0, '$root/tools')
import ed25519
pub = bytes.fromhex(os.environ['PUB']); data = open('app.bin', 'rb').read(); sig = open('app.bin.sig', 'rb').read()
assert len(sig) == 64
assert ed25519.verify(pub, data, sig), 'a valid OpenSSL signature was rejected'
assert not ed25519.verify(pub, data[:-1] + bytes([data[-1] ^ 1]), sig), 'a tampered file was accepted'
print('signature interoperability checks passed')
PYEOF
