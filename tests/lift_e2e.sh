#!/usr/bin/env bash
# End-to-end test of the translator: generate a small test program, survey and translate it, build the translated code into the runtime, run it and
# compare its output with the expected output computed by the generator. Run from the repository root after the main build configured cmake
# (CMAKE_EXTRA carries the platform's extra cmake arguments).
set -euo pipefail
root="$(pwd)"
python -m pip install --quiet capstone
mkdir -p lt/work/lift
cd lt
python "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
python "$root/tools/lift_survey.py" lift_test.xbe > survey.log
python "$root/tools/lift.py" lift_test.xbe --out work/lift/gen > lift.log
tail -3 lift.log
cmake -S "$root" -B "$root/build-lt" -DCMAKE_BUILD_TYPE=Release -DREFORGED_LIFTED_DIR="$PWD/work/lift/gen" ${CMAKE_EXTRA:-}
cmake --build "$root/build-lt" --config Release --parallel --target reforged
exe="$root/build-lt/reforged"
if [ -f "$root/build-lt/Release/reforged.exe" ]; then exe="$root/build-lt/Release/reforged.exe"; fi
XBOX_LIFT=1 "$exe" none lift_test.xbe 3 > run.log 2>&1 || true
grep '^\[DbgPrint\] ' run.log | sed 's/^\[DbgPrint\] //' | tr -d '\r' > got.txt || true
tr -d '\r' < expected.txt > want.txt
if diff -u want.txt got.txt > diff.txt; then
  echo "translated program output matches ($(wc -l < got.txt) lines)"
  exit 0
fi
echo "OUTPUT MISMATCH"; cat diff.txt; echo "--- runtime log"; head -60 run.log
exit 1
