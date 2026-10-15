#!/usr/bin/env bash
# Proves the iOS game module can be produced with a plain LLVM toolchain on a non-Apple machine: translate the generated test program, build it with
# tools/build_module.py (clang + lld, own freestanding headers, no SDK) for arm64 iOS, then inspect the Mach-O: architecture, platform, the exported
# entry point, and that the only symbols it expects from the system are libc/libm functions. Run from the repository root; SSE2NEON_DIR must be set.
set -euo pipefail
root="$(pwd)"
python -m pip install --quiet capstone
mkdir -p xc/work/lift
cd xc
python -I "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
python -I "$root/tools/lift_survey.py" lift_test.xbe > survey.log
python -I "$root/tools/lift.py" lift_test.xbe --out work/lift/gen > lift.log
tail -2 lift.log
python -I "$root/tools/build_module.py" --gen work/lift/gen --out game_module.dylib --clang clang++ --sse2neon "$SSE2NEON_DIR"

tool() { command -v "$1" 2>/dev/null || ls /usr/lib/llvm-*/bin/"$1" 2>/dev/null | tail -1; }
objdump="$(tool llvm-objdump)"; nm="$(tool llvm-nm)"
echo "--- Mach-O headers"
hdr="$("$objdump" --macho --private-headers game_module.dylib)"   # captured first: grep -q exiting early would kill a pipeline writer (SIGPIPE) under pipefail
grep -E "cputype|filetype|platform|minos|name @rpath" <<< "$hdr" | head
grep -q "ARM64" <<< "$hdr" || { echo "not an arm64 binary"; exit 1; }
grep -qi "platform.*IOS\|platform 2" <<< "$hdr" || { echo "not built for the iOS platform"; exit 1; }
echo "--- exports"
"$nm" -gU game_module.dylib | tee exports.txt
grep -q "_reforged_module_init" exports.txt || { echo "entry point not exported"; exit 1; }
echo "--- undefined symbols (resolved from the system when the module is loaded)"
"$nm" -u game_module.dylib | sed 's/^ *//' | sort -u | tee undefined.txt
allowed="_memcpy _memmove _memset _memcmp _strlen _malloc _free _posix_memalign _abort _fegetround _fesetround _sin _cos _tan _atan _atan2 _sqrt _pow _log _log2 _log10 _exp _exp2 _ldexp _frexp _fabs _floor _ceil _trunc _fmod _rint _nearbyint _round _copysign _sinf _cosf _tanf _sqrtf _fabsf _floorf _ceilf _powf _lrint _llrint _sys_icache_invalidate _sys_dcache_flush dyld_stub_binder"
bad=0
while read -r sym; do
  [ -z "$sym" ] && continue
  case " $allowed " in *" $sym "*) ;; *) echo "UNEXPECTED undefined symbol: $sym"; bad=1;; esac
done < undefined.txt
[ "$bad" = 0 ] || exit 1
echo "iOS module checks passed"
