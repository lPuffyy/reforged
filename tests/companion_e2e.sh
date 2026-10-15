#!/usr/bin/env bash
# End-to-end test of the companion tool with a generated test disc image and a stand-in app file (no network, no real game): it must read the disc image,
# translate the program, build the module with clang + lld, and write a personal app that contains the module and the executable and keeps the original
# app's files. Run from the repository root; SSE2NEON_DIR must be set.
set -euo pipefail
root="$(pwd)"
python -m pip install --quiet capstone
mkdir -p ce && cd ce
python -I "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
python -I "$root/tests/make_test_iso.py" lift_test.xbe Test.iso
python -I - <<'PYEOF'
import zipfile
with zipfile.ZipFile('base.ipa', 'w') as z:
    zi = zipfile.ZipInfo('Payload/Reforged.app/Reforged'); zi.external_attr = 0o755 << 16; z.writestr(zi, b'\xcf\xfa\xed\xfe stand-in executable')
    z.writestr('Payload/Reforged.app/Info.plist', '<plist/>')
PYEOF
python -I "$root/tools/companion.py" --iso Test.iso --ipa base.ipa --skip-symbols --no-disc --clang clang++ --ld lld --sse2neon "$SSE2NEON_DIR" --out personal.ipa
python -I - <<'PYEOF'
import zipfile
z = zipfile.ZipFile('personal.ipa'); names = z.namelist(); print('\n'.join(names))
need = ['Payload/Reforged.app/Reforged', 'Payload/Reforged.app/Info.plist', 'Payload/Reforged.app/Frameworks/reforged_game.dylib',
        'Payload/Reforged.app/title/default.xbe', 'Payload/Reforged.app/title/no-disc.txt']
missing = [n for n in need if n not in names]
assert not missing, 'missing from the personal app: %s' % missing
assert z.read('Payload/Reforged.app/title/default.xbe') == open('lift_test.xbe', 'rb').read(), 'executable differs from the disc image copy'
assert (z.getinfo('Payload/Reforged.app/Reforged').external_attr >> 16) & 0o111, 'executable permission lost'
mod = z.read('Payload/Reforged.app/Frameworks/reforged_game.dylib'); assert mod[:4] in (b'\xcf\xfa\xed\xfe',), 'the module is not a Mach-O file'
print('companion output checks passed (module %d bytes)' % len(mod))
PYEOF
