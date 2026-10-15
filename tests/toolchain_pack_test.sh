#!/usr/bin/env bash
# Tests a toolchain pack the way the companion uses it: unpack it, cross-compile the iOS game module for the generated test program with only what is in the
# pack (clang, lld, the compiler headers), and check the result is an arm64 iOS library with the module entry point. Run from the repository root.
# usage: tests/toolchain_pack_test.sh <pack.zip>  
set -euo pipefail
root="$(pwd)"
pack="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
python -m pip install --quiet capstone
rm -rf tp && mkdir -p tp/pack tp/work/lift
cd tp
python -I - "$pack" <<'PYEOF'
import os, stat, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for info in z.infolist():
    path = os.path.join('pack', info.filename); os.makedirs(os.path.dirname(path), exist_ok=True)
    if info.is_dir(): continue
    with open(path, 'wb') as f: f.write(z.read(info))
    mode = (info.external_attr >> 16) & 0o777
    if mode: os.chmod(path, mode)
print(z.read('manifest.json').decode())
PYEOF
ext=""; [ -f pack/bin/clang.exe ] && ext=".exe"
pack_clang="$PWD/pack/bin/clang$ext"; pack_lld="$PWD/pack/bin/lld$ext"; scanner="$PWD/pack/xbsym/XbSymbolDatabaseCLI$ext"
"$pack_clang" --version | head -2
"$pack_lld" -flavor darwin --version 2>&1 | head -2 || true
"$scanner" 2>&1 | head -2 || true   # prints its usage
python -I "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
python -I "$root/tools/lift_survey.py" lift_test.xbe > survey.log
python -I "$root/tools/lift.py" lift_test.xbe --out work/lift/gen > lift.log
python -I "$root/tools/build_module.py" --gen work/lift/gen --out game_module.dylib --clang "$pack_clang" --lld "$pack_lld" --sse2neon "$PWD/pack/include"
python -I "$root/tools/macho_info.py" game_module.dylib --expect-ios --expect-symbol _reforged_module_init
echo "toolchain pack checks passed"
