#!/usr/bin/env bash
# Tests the frozen companion program (a single file): it must start, read a generated disc image, and run the bundled survey and translator. The module build is
# pointed at a compiler that does not exist, so the run must stop at exactly that step; with a real clang + lld available (COMPANION_FULL=1 and SSE2NEON_DIR set)
# it must also write a complete personal app.
# usage: tests/companion_frozen.sh <path to reforged-companion>
set -euo pipefail
root="$(pwd)"
exe="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
python -m pip install --quiet capstone
rm -rf cf && mkdir cf && cd cf
python -I "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
python -I "$root/tests/make_test_iso.py" lift_test.xbe Test.iso
python -I - <<'PYEOF'
import zipfile
with zipfile.ZipFile('base.ipa', 'w') as z:
    zi = zipfile.ZipInfo('Payload/Reforged.app/Reforged'); zi.external_attr = 0o755 << 16; z.writestr(zi, b'stand-in executable')
    z.writestr('Payload/Reforged.app/Info.plist', '<plist/>')
PYEOF
"$exe" --help | head -3
set +e
"$exe" --iso Test.iso --ipa base.ipa --skip-symbols --no-disc --clang /nonexistent/clang --lld /nonexistent/lld --out nope.ipa > run.log 2>&1
rc=$?
set -e
cat run.log
test "$rc" -ne 0 || { echo "the run should have stopped at the module build"; exit 1; }
grep -q "lifted 6 of 6 functions" run.log || { echo "the bundled translator did not run"; exit 1; }
grep -q "found default.xbe" run.log || { echo "the disc image was not read"; exit 1; }
if [ "${COMPANION_FULL:-}" = "1" ]; then
  "$exe" --iso Test.iso --ipa base.ipa --skip-symbols --no-disc --clang clang++ --ld lld --sse2neon "$SSE2NEON_DIR" --out personal.ipa
  python -I -c "
import zipfile
z = zipfile.ZipFile('personal.ipa'); n = z.namelist()
assert 'Payload/Reforged.app/Games/games.json' in n and any(x.startswith('Payload/Reforged.app/Frameworks/game-') and x.endswith('.dylib') for x in n) and any(x.endswith('/title/default.xbe') for x in n), n
print('frozen companion produced a complete personal app')"
fi
echo "frozen companion checks passed"
