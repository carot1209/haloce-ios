#!/bin/bash
# Builds the iOS app: out/HaloCE-unsigned.ipa (sign it when sideloading).
#
# On macOS with Xcode. The steps (ios/README.md):
#   1. OpenCE at upstream.ref, with ios/patches/0*.patch
#   2. wasi-sdk (clang for wasm32) and wasm2c (WABT at ios/wabt.ref, with
#      ios/patches/wabt-atomic-wait.patch)
#   3. the guest module: the Android port's guest image, compiled for wasm32
#      (ios/tools/build_guest.py), translated to C (wasm2c)
#   4. SDL3 for iOS (the release OpenCE's builds use)
#   5. the host (ios/host) and the translated guest, compiled for arm64 iOS
#      and linked; the app bundled and zipped
#
# Environment: HALO_CACHE (a folder kept between builds: downloads, WABT,
# SDL3), JOBS.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out"
BUILD="$ROOT/build"
CACHE="${HALO_CACHE:-$BUILD/cache}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
UPSTREAM_REF="$(cat "$ROOT/upstream.ref")"
WABT_REF="$(cat "$ROOT/ios/wabt.ref")"
WASI_SDK_VERSION=27
WASI_SDK_RELEASE="wasi-sdk-$WASI_SDK_VERSION.0"
SDL_TAG="release-3.4.16"
IOS_MIN=15.0
mkdir -p "$OUT" "$BUILD" "$CACHE"

echo "== OpenCE $UPSTREAM_REF"
UP="$BUILD/opence"
if [ ! -d "$UP/.git" ]; then
    git clone -q https://github.com/OpenCommunityEdition/OpenCE.git "$UP"
fi
git -C "$UP" fetch -q --depth 1 origin "$UPSTREAM_REF" 2>/dev/null || git -C "$UP" fetch -q origin
git -C "$UP" checkout -q -f "$UPSTREAM_REF"
git -C "$UP" clean -qfdx -e build/android/third_party
for patch in "$ROOT"/ios/patches/0*.patch; do
    git -C "$UP" apply "$patch"
done

echo "== wasi-sdk $WASI_SDK_RELEASE"
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) WASI_HOST=arm64-macos ;;
    Darwin-x86_64) WASI_HOST=x86_64-macos ;;
    Linux-x86_64) WASI_HOST=x86_64-linux ;;
    *) echo "no wasi-sdk for this machine"; exit 1 ;;
esac
WASI_SDK="$CACHE/$WASI_SDK_RELEASE-$WASI_HOST"
if [ ! -x "$WASI_SDK/bin/clang" ]; then
    curl -sSfL -o "$CACHE/wasi-sdk.tar.gz" \
        "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$WASI_SDK_VERSION/$WASI_SDK_RELEASE-$WASI_HOST.tar.gz"
    tar xzf "$CACHE/wasi-sdk.tar.gz" -C "$CACHE"
    rm "$CACHE/wasi-sdk.tar.gz"
fi

echo "== wasm2c (WABT $WABT_REF)"
WABT="$CACHE/wabt-$WABT_REF-$(shasum -a 256 "$ROOT/ios/patches/wabt-atomic-wait.patch" 2>/dev/null | cut -c1-8 || sha256sum "$ROOT/ios/patches/wabt-atomic-wait.patch" | cut -c1-8)"
if [ ! -x "$WABT/build/wasm2c" ]; then
    rm -rf "$WABT"
    git init -q "$WABT"
    git -C "$WABT" remote add origin https://github.com/WebAssembly/wabt.git
    git -C "$WABT" fetch -q --depth 1 origin "$WABT_REF"
    git -C "$WABT" checkout -q FETCH_HEAD
    git -C "$WABT" submodule update -q --init --depth 1 third_party/picosha2
    git -C "$WABT" apply "$ROOT/ios/patches/wabt-atomic-wait.patch"
    cmake -S "$WABT" -B "$WABT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
        -DBUILD_LIBWASM=OFF -DWITH_WASI=OFF -DUSE_INTERNAL_SHA256=ON > "$BUILD/wabt-configure.log"
    ninja -C "$WABT/build" wasm2c wasm-objdump > "$BUILD/wabt-build.log"
fi

echo "== the guest module (wasm32)"
GUEST="$BUILD/guest"
python3 "$ROOT/ios/tools/build_guest.py" --opence "$UP" --wasi-sdk "$WASI_SDK" \
    --gl-include "$ROOT/ios/third_party/khronos" --out "$GUEST" --jobs "$JOBS" > "$BUILD/guest.log" 2>&1 \
    || { tail -n 80 "$BUILD/guest.log"; exit 1; }
tail -n 3 "$BUILD/guest.log"

echo "== wasm2c"
W2C="$BUILD/w2c"
rm -rf "$W2C"; mkdir -p "$W2C"
"$WABT/build/wasm2c" --enable-threads --num-outputs=32 -n guest "$GUEST/halo_guest.wasm" -o "$W2C/guest.c"
python3 "$ROOT/ios/tools/gen_imports.py" "$W2C/guest.h" "$UP/port/android/guest/runtime/guest_host.h" \
    "$UP/build/android/guest/gen/guest_gl.c" "$UP/build/android/guest/gen/guest_posix.c" "$W2C/ios_imports.c"

echo "== SDL3 for iOS ($SDL_TAG)"
SDL_SRC="$UP/build/android/third_party/SDL3"
SDL_IOS="$CACHE/sdl3-ios-$SDL_TAG"
if [ ! -f "$SDL_IOS/lib/libSDL3.a" ]; then
    cmake -S "$SDL_SRC" -B "$BUILD/sdl3-ios" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
        -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$SDL_IOS" \
        > "$BUILD/sdl3-configure.log"
    ninja -C "$BUILD/sdl3-ios" install > "$BUILD/sdl3-build.log"
fi

echo "== the app ($JOBS jobs)"
IOS_BUILD="$BUILD/ios"
mkdir -p "$IOS_BUILD"
python3 "$ROOT/ios/tools/gen_app_ninja.py" --root "$ROOT" --opence "$UP" --w2c "$W2C" --wabt "$WABT" \
    --sdl "$SDL_IOS" --out "$IOS_BUILD" --ios-min "$IOS_MIN"
ninja -C "$IOS_BUILD" -j "$JOBS" | { grep -v '^\[.*W2C ' || true; }
[ -x "$IOS_BUILD/HaloCE" ] || { echo "no executable"; exit 1; }

echo "== bundle"
APP="$BUILD/Payload/HaloCE.app"
rm -rf "$BUILD/Payload"; mkdir -p "$APP"
cp "$IOS_BUILD/HaloCE" "$APP/HaloCE"
strip -x "$APP/HaloCE" 2>/dev/null || true
cp "$ROOT/ios/Info.plist" "$APP/Info.plist"
cp "$UP/port/assets/network/brokers.txt" "$APP/brokers.txt"
cp "$ROOT"/ios/icons/*.png "$APP/" 2>/dev/null || true
printf 'APPL????' > "$APP/PkgInfo"
( cd "$BUILD" && rm -f "$OUT/HaloCE-unsigned.ipa" && zip -qry "$OUT/HaloCE-unsigned.ipa" Payload )

{
    echo "upstream : OpenCE $UPSTREAM_REF"
    echo "wabt     : $WABT_REF"
    echo "date     : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    xcodebuild -version 2>/dev/null | tr '\n' ' '; echo
    echo "guest    : $(stat -f %z "$GUEST/halo_guest.wasm" 2>/dev/null) bytes of wasm"
    echo "ipa      : $(stat -f %z "$OUT/HaloCE-unsigned.ipa" 2>/dev/null) bytes"
    file "$APP/HaloCE"
} | tee "$OUT/build-info.txt"
