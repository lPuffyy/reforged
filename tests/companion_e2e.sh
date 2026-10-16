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
import hashlib, json
gid = hashlib.sha256(open('lift_test.xbe', 'rb').read()).hexdigest(); g16 = gid[:16]
need = ['Payload/Reforged.app/Reforged', 'Payload/Reforged.app/Info.plist', 'Payload/Reforged.app/Frameworks/game-%s.dylib' % g16,
        'Payload/Reforged.app/Games/%s/title/default.xbe' % g16, 'Payload/Reforged.app/Games/%s/title/no-disc.txt' % g16, 'Payload/Reforged.app/Games/games.json']
missing = [n for n in need if n not in names]
assert not missing, 'missing from the personal app: %s' % missing
assert z.read('Payload/Reforged.app/Games/%s/title/default.xbe' % g16) == open('lift_test.xbe', 'rb').read(), 'executable differs from the disc image copy'
assert (z.getinfo('Payload/Reforged.app/Reforged').external_attr >> 16) & 0o111, 'executable permission lost'
mod = z.read('Payload/Reforged.app/Frameworks/game-%s.dylib' % g16); assert mod[:4] in (b'\xcf\xfa\xed\xfe',), 'the module is not a Mach-O file'
m = json.loads(z.read('Payload/Reforged.app/Games/games.json')); g = m['games'][0]
assert g['id'] == gid and g['noDisc'] is True and g['module'] == 'Frameworks/game-%s.dylib' % g16 and g['title'] == 'Games/%s/title' % g16, g
assert 'Payload/Reforged.app/title/default.xbe' not in names and 'Payload/Reforged.app/Frameworks/reforged_game.dylib' not in names, 'the old single-game layout is still written'
print('companion output checks passed (module %d bytes)' % len(mod))
PYEOF

# Adding a second game to a personal app (the first stays), and converting an older single-game app to the library layout.
python -I - <<'PYEOF'
import struct
d = bytearray(open('lift_test.xbe', 'rb').read()); base, = struct.unpack_from('<I', d, 0x104); cert, = struct.unpack_from('<I', d, 0x118); co = cert - base
name = 'Second test program'.encode('utf-16le'); d[co + 0xC:co + 0xC + 80] = name + bytes(80 - len(name)); open('second.xbe', 'wb').write(d)
PYEOF
python -I "$root/tests/make_test_iso.py" second.xbe Second.iso
python -I "$root/tools/companion.py" --iso Second.iso --add-to personal.ipa --skip-symbols --no-disc --clang clang++ --ld lld --sse2neon "$SSE2NEON_DIR" --out library.ipa
python -I - <<'PYEOF'
import hashlib, json, zipfile
z = zipfile.ZipFile('library.ipa'); names = z.namelist()
m = json.loads(z.read('Payload/Reforged.app/Games/games.json')); games = m['games']
assert [g['name'] for g in games] == ['Lift test', 'Second test program'], games
for g in games:
    assert 'Payload/Reforged.app/' + g['module'] in names and 'Payload/Reforged.app/' + g['title'] + '/default.xbe' in names, g
print('adding a game keeps the first one: ok')
# an older single-game app (title/ and Frameworks/reforged_game.dylib) is converted, and its saved files stay linked to the game
old = zipfile.ZipFile('personal.ipa'); gid = hashlib.sha256(open('lift_test.xbe', 'rb').read()).hexdigest()
with zipfile.ZipFile('legacy.ipa', 'w') as out:
    for n in old.namelist():
        if '/Games/' in n or '/Frameworks/game-' in n: continue
        out.writestr(old.getinfo(n), old.read(n))
    out.writestr('Payload/Reforged.app/Frameworks/reforged_game.dylib', old.read('Payload/Reforged.app/Frameworks/game-%s.dylib' % gid[:16]))
    out.writestr('Payload/Reforged.app/title/default.xbe', open('lift_test.xbe', 'rb').read())
    out.writestr('Payload/Reforged.app/title/no-disc.txt', 'x')
PYEOF
python -I "$root/tools/companion.py" --iso Second.iso --add-to legacy.ipa --skip-symbols --no-disc --clang clang++ --ld lld --sse2neon "$SSE2NEON_DIR" --out converted.ipa
python -I - <<'PYEOF'
import json, zipfile
z = zipfile.ZipFile('converted.ipa'); names = z.namelist()
games = json.loads(z.read('Payload/Reforged.app/Games/games.json'))['games']
assert len(games) == 2 and games[0]['name'] == 'Lift test' and games[0]['legacyState'] is True, games
assert 'Payload/Reforged.app/title/default.xbe' not in names and 'Payload/Reforged.app/Frameworks/reforged_game.dylib' not in names, 'the old layout was not removed'
print('an older single-game app is converted: ok')
PYEOF
