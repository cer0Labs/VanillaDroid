#!/bin/bash
# Phase 22 (gui): VanillaDroid Setup for macOS, universal (Apple silicon and Intel), for the release CI (22.5) on a
# macOS runner with Xcode's command line tools and CMake. Written without a Mac: its first CI run is its first test.
#
#   tools/setup/gui/macos/build-macos.sh [X.Y.Z]
#
# One build per architecture (astc-encoder picks its SIMD per architecture: NEON on arm64, SSE4.1 on x86_64), the
# tools alone (VD_BUILD_GAME=OFF: no renderer, shaders or tests), then lipo joins each program of the two bundles into
# one. The result: build/macos-dist/VanillaDroid Setup.app (with vd_baker and vdsetup inside, ad-hoc signed: Apple
# silicon refuses unsigned code; there is no paid certificate, so Gatekeeper still asks the first time) and
# build/macos-dist/VanillaDroidSetup-macos.zip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
VERSION="${1:-0.1.0}"
CONFIG=Release
DIST="$ROOT/build/macos-dist"
APP="VanillaDroid Setup.app"

for ARCH in arm64 x86_64; do
    # CMAKE_APPLE_SILICON_PROCESSOR makes CMAKE_SYSTEM_PROCESSOR the slice's architecture on an Apple silicon runner
    # (cmake/Dependencies.cmake chooses astc-encoder's SIMD by it).
    cmake -S "$ROOT" -B "$ROOT/build/macos-$ARCH" \
        -DCMAKE_BUILD_TYPE=$CONFIG \
        -DCMAKE_OSX_ARCHITECTURES=$ARCH \
        -DCMAKE_APPLE_SILICON_PROCESSOR=$ARCH \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
        -DVD_BUILD_GAME=OFF \
        -DVD_BUILD_TESTS=OFF \
        -DVD_DIST=ON \
        -DVD_VERSION="$VERSION"
    cmake --build "$ROOT/build/macos-$ARCH" --target VanillaDroidSetup --parallel
done

rm -rf "$DIST"
mkdir -p "$DIST"
cp -R "$ROOT/build/macos-arm64/tools/setup/$APP" "$DIST/"
for PROGRAM in "VanillaDroid Setup" vd_baker vdsetup; do
    lipo -create \
        "$ROOT/build/macos-arm64/tools/setup/$APP/Contents/MacOS/$PROGRAM" \
        "$ROOT/build/macos-x86_64/tools/setup/$APP/Contents/MacOS/$PROGRAM" \
        -output "$DIST/$APP/Contents/MacOS/$PROGRAM"
    lipo -info "$DIST/$APP/Contents/MacOS/$PROGRAM"
done
strip -x "$DIST/$APP/Contents/MacOS/"*

codesign --force --deep --sign - "$DIST/$APP"
codesign --verify --deep "$DIST/$APP"
(cd "$DIST" && ditto -c -k --keepParent "$APP" VanillaDroidSetup-macos.zip)
echo "$DIST/VanillaDroidSetup-macos.zip"
