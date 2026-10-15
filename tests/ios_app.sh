#!/usr/bin/env bash
# Builds the iOS app twice and runs one of them:
#   1. for a real device (arm64, unsigned) and packages it as an unsigned IPA,
#   2. for the iOS Simulator, installs it on a simulator, launches it and checks from the app's log that the runtime started.
# Run from the repository root on macOS with SSE2NEON_DIR and VULKAN_HEADERS_DIR set.
set -euo pipefail
root="$(pwd)"
python3 tools/gen_kernel.py

echo "=== device build"
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DSSE2NEON_DIR="$SSE2NEON_DIR" -DVULKAN_HEADERS_DIR="$VULKAN_HEADERS_DIR"
cmake --build build-ios --config Release --target ReforgedApp -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=
app="$(find build-ios -type d -name ReforgedApp.app -path '*Release-iphoneos*' | head -1)"
echo "app: $app"
test -d "$app"
file "$app/ReforgedApp"
xcrun vtool -show-build "$app/ReforgedApp" | head -6
rm -rf ipa && mkdir -p ipa/Payload && cp -R "$app" ipa/Payload/
(cd ipa && zip -qry ../Reforged-unsigned.ipa Payload)
ls -l Reforged-unsigned.ipa

echo "=== simulator build"
cmake -S . -B build-sim -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DSSE2NEON_DIR="$SSE2NEON_DIR" -DVULKAN_HEADERS_DIR="$VULKAN_HEADERS_DIR"
cmake --build build-sim --config Release --target ReforgedApp -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=
simapp="$(find build-sim -type d -name ReforgedApp.app -path '*Release-iphonesimulator*' | head -1)"
test -d "$simapp"

echo "=== run in the simulator"
udid="$(xcrun simctl list devices available | grep -m1 'iPhone' | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/')"
echo "simulator: $udid"
xcrun simctl boot "$udid" || true
xcrun simctl bootstatus "$udid" -b > /dev/null 2>&1 || true
xcrun simctl install "$udid" "$simapp"
xcrun simctl launch "$udid" dev.reforged.app
sleep 12
data="$(xcrun simctl get_app_container "$udid" dev.reforged.app data)"
echo "app data: $data"
ls -la "$data/Documents" || true
log="$data/Documents/reforged.log"
test -f "$log" || { echo "the runtime wrote no log"; exit 1; }
cat "$log"
grep -q "register window mapped" "$log" || { echo "the runtime did not finish initialising (guest memory, device models)"; exit 1; }   # logged after guest memory and the device windows are mapped
grep -q "XBE load failed" "$log" || { echo "the runtime did not reach the game load step"; exit 1; }

echo "=== a translated program running inside the app (Simulator)"
python3 -m pip install --quiet capstone
rm -rf simmod && mkdir -p simmod/work/lift
( cd simmod
  python3 -I "$root/tests/make_test_xbe.py" lift_test.xbe expected.txt
  python3 -I "$root/tools/lift_survey.py" lift_test.xbe > survey.log
  python3 -I "$root/tools/lift.py" lift_test.xbe --out work/lift/gen > lift.log
  python3 -I "$root/tools/build_module.py" --gen work/lift/gen --out reforged_game.dylib --target arm64-apple-ios15.0-simulator     --clang clang++ --ld system --sse2neon "$SSE2NEON_DIR"
  codesign -s - --force reforged_game.dylib )
xcrun simctl terminate "$udid" dev.reforged.app || true
mkdir -p "$simapp/Frameworks" "$simapp/title"
cp simmod/reforged_game.dylib "$simapp/Frameworks/reforged_game.dylib"
cp simmod/lift_test.xbe "$simapp/title/default.xbe"
codesign --force --deep -s - "$simapp"
xcrun simctl install "$udid" "$simapp"
xcrun simctl launch "$udid" dev.reforged.app
sleep 20
data="$(xcrun simctl get_app_container "$udid" dev.reforged.app data)"
log="$data/Documents/reforged.log"
test -f "$log" || { echo "no log"; exit 1; }
grep -E "lift\]|DbgPrint|FATAL|crash" "$log" | head -40
grep -q "translated functions" "$log" || { echo "the app did not load the game module"; head -40 "$log"; exit 1; }
grep '^\[DbgPrint\] ' "$log" | sed 's/^\[DbgPrint\] //' > simmod/got.txt || true
if ! diff -u simmod/expected.txt simmod/got.txt; then echo "OUTPUT MISMATCH inside the app"; head -60 "$log"; exit 1; fi
echo "translated program output matches inside the iOS app"
xcrun simctl shutdown "$udid" || true
echo "iOS app checks passed"
