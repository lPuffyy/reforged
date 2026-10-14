#!/usr/bin/env bash
# End-to-end test of the translator: generate a small test program, survey and translate it, run it two ways and compare with the expected output
# computed by the generator:
#   1. translated code linked into the runtime,
#   2. translated code built as a separate module that the runtime loads (how the iOS app uses it).
# On macOS the module is also cross-compiled for iOS arm64 with the iPhoneOS SDK (compile and link only: it cannot run here).
# Run from the repository root after the main build configured cmake (CMAKE_EXTRA carries the platform's extra cmake arguments).
set -euo pipefail
root="$(pwd)"
python -m pip install --quiet capstone
mkdir -p lt/work/lift
cd lt
python -I "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
python -I "$root/tools/lift_survey.py" lift_test.xbe > survey.log
python -I "$root/tools/lift.py" lift_test.xbe --out work/lift/gen > lift.log
tail -3 lift.log
tr -d '\r' < expected.txt > want.txt

check() {   # check <label>: compares the DbgPrint lines of run.log with want.txt
  grep '^\[DbgPrint\] ' run.log | sed 's/^\[DbgPrint\] //' | tr -d '\r' > got.txt || true
  if diff -u want.txt got.txt > diff.txt; then echo "[$1] translated program output matches ($(wc -l < got.txt) lines)"; return 0; fi
  echo "OUTPUT MISMATCH [$1]"; cat diff.txt; echo "--- runtime log"; head -60 run.log
  return 1
}

# 1. linked
cmake -S "$root" -B "$root/build-lt" -DCMAKE_BUILD_TYPE=Release -DREFORGED_LIFTED_DIR="$PWD/work/lift/gen" ${CMAKE_EXTRA:-}
cmake --build "$root/build-lt" --config Release --parallel --target reforged
exe="$root/build-lt/reforged"
if [ -f "$root/build-lt/Release/reforged.exe" ]; then exe="$root/build-lt/Release/reforged.exe"; fi
XBOX_LIFT=1 "$exe" none lift_test.xbe 3 > run.log 2>&1 || true
check linked

# 2. module
cmake -S "$root" -B "$root/build-lm" -DCMAKE_BUILD_TYPE=Release -DREFORGED_LIFTED_DIR="$PWD/work/lift/gen" -DREFORGED_LIFTED_AS_MODULE=ON ${CMAKE_EXTRA:-}
cmake --build "$root/build-lm" --config Release --parallel
exe="$root/build-lm/reforged"
if [ -f "$root/build-lm/Release/reforged.exe" ]; then exe="$root/build-lm/Release/reforged.exe"; fi
mod="$(find "$root/build-lm" -name 'lift_module.*' -type f | grep -E '\.(so|dylib|dll)$' | head -1)"
echo "module: $mod"
XBOX_LIFT=1 XBOX_LIFT_MODULE="$mod" "$exe" none lift_test.xbe 3 > run.log 2>&1 || true
grep -q "translated functions" run.log || { echo "the module was not loaded"; head -40 run.log; exit 1; }
check module

# 3. iOS arm64 cross compile (macOS only)
if [ "$(uname)" = "Darwin" ] && [ -n "${SSE2NEON_DIR:-}" ]; then
  sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
  mkdir -p ios_obj
  for f in work/lift/gen/*.cpp "$root/src/lift_module_main.cpp"; do
    xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=15.0 -isysroot "$sdk" -std=c++17 -O1 -fvisibility=hidden \
      -DLIFT_MODULE -DLIFT_NO_NATIVE -DLIFT_EXPLICIT_MMIO -I"$root/src" -Iwork/lift/gen -I"$SSE2NEON_DIR" -w -c "$f" -o "ios_obj/$(basename "$f").o"
  done
  xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=15.0 -isysroot "$sdk" -dynamiclib -undefined dynamic_lookup -o lift_module_ios.dylib ios_obj/*.o
  echo "iOS module:"; file lift_module_ios.dylib; xcrun vtool -show-build lift_module_ios.dylib | head -8
  xcrun nm -gU lift_module_ios.dylib | grep reforged_module_init
fi
