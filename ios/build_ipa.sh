#!/usr/bin/env bash
# Builds the generic app for a device (arm64, unsigned) and packages it as Reforged-unsigned.ipa, with the Vulkan-on-Metal layer embedded.
# Run from the repository root on macOS with SSE2NEON_DIR and VULKAN_HEADERS_DIR set. VERSION (default 0.1) becomes the app's version string.
set -euo pipefail
version="${VERSION:-0.1}"
MVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar"
MVK_SHA256="b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1"   # pinned: the build refuses a different package

python3 tools/gen_kernel.py
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREFORGED_VERSION="$version" \
  -DSSE2NEON_DIR="$SSE2NEON_DIR" -DVULKAN_HEADERS_DIR="$VULKAN_HEADERS_DIR"
cmake --build build-ios --config Release --target ReforgedApp -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=
app="$(find build-ios -type d -name ReforgedApp.app -path '*Release-iphoneos*' | head -1)"
test -d "$app"
file "$app/ReforgedApp"
xcrun vtool -show-build "$app/ReforgedApp" | head -6

curl -sL -o MoltenVK-ios.tar "$MVK_URL"
echo "$MVK_SHA256  MoltenVK-ios.tar" | shasum -a 256 -c -
rm -rf mvk && mkdir -p mvk && tar -xf MoltenVK-ios.tar -C mvk
mkdir -p "$app/Frameworks"
cp -R mvk/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework "$app/Frameworks/"
cp mvk/MoltenVK/LICENSE "$app/MoltenVK-LICENSE.txt"
file "$app/Frameworks/MoltenVK.framework/MoltenVK"

rm -rf ipa && mkdir -p ipa/Payload && cp -R "$app" ipa/Payload/
(cd ipa && zip -qry ../Reforged-unsigned.ipa Payload)
ls -l Reforged-unsigned.ipa
echo "$app" > .device_app_path
