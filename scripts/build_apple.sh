#!/usr/bin/env bash
# Build self-contained macOS universal2 / iOS XCFramework artifacts on a Mac.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="${1:-all}"
case "$PLATFORM" in macos|ios|all) ;; *) echo 'Usage: build_apple.sh [macos|ios|all]' >&2; exit 2;; esac
[[ "$(uname -s)" == Darwin ]] || { echo 'This build requires macOS and Xcode.' >&2; exit 1; }
for tool in xcrun xcodebuild cmake ninja git curl; do command -v "$tool" >/dev/null; done
WORK="$ROOT/build/apple"
mkdir -p "$WORK"
cd "$WORK"
JOBS="${LTF_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
# Keep these versions in sync with .github/workflows/build.yml.
if [[ ! -d libtorrent-src ]]; then
  git clone --depth 1 --branch v2.1.1 --recurse-submodules https://github.com/arvidn/libtorrent.git libtorrent-src
fi
if [[ ! -d openssl-src ]]; then
  git clone --depth 1 --branch openssl-3.2.1 https://github.com/openssl/openssl.git openssl-src
fi
if [[ ! -d boost_1_84_0 ]]; then
  curl -fL --retry 3 https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz -o boost.tar.gz
  tar xf boost.tar.gz
fi
BOOST_ROOT="$WORK/boost_1_84_0"

build_slice() {
  local family="$1" arch="$2" sdk_name="$3" target="$4" ssl_target="$5"
  local key="${family}-${arch}" sdk ssl_root lt_build slice
  sdk="$(xcrun --sdk "$sdk_name" --show-sdk-path)"
  ssl_root="$WORK/openssl-$key"
  lt_build="$WORK/libtorrent-$key"
  slice="$WORK/$key"
  mkdir -p "$slice"
  if [[ ! -f "$ssl_root/lib/libssl.a" ]]; then
    (
      cd openssl-src
      if [[ -f Makefile ]]; then make distclean; fi
      CC="$(xcrun --sdk "$sdk_name" -find clang)" \
      CFLAGS="-arch $arch -target $target -isysroot $sdk -fPIC" \
      ./Configure "$ssl_target" no-shared no-tests no-ui-console no-docs no-asm \
        --prefix="$ssl_root" --libdir=lib
      make -j"$JOBS" build_libs
      make install_dev
    )
  fi
  local system=Darwin minimum=11.0
  if [[ "$family" != macos ]]; then system=iOS; minimum=13.0; fi
  cmake -S libtorrent-src -B "$lt_build" -G Ninja \
    -DCMAKE_SYSTEM_NAME="$system" -DCMAKE_OSX_SYSROOT="$sdk" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$minimum" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$WORK/lt-install-$key" \
    -DBUILD_SHARED_LIBS=OFF -Ddeprecated-functions=ON -Dwebtorrent=OFF -Dencryption=ON \
    -DBOOST_ROOT="$BOOST_ROOT" -DBoost_INCLUDE_DIR="$BOOST_ROOT" \
    -DOPENSSL_ROOT_DIR="$ssl_root" -DOPENSSL_INCLUDE_DIR="$ssl_root/include" \
    -DOPENSSL_SSL_LIBRARY="$ssl_root/lib/libssl.a" -DOPENSSL_CRYPTO_LIBRARY="$ssl_root/lib/libcrypto.a"
  cmake --build "$lt_build" --parallel "$JOBS"
  local lt_lib
  lt_lib="$(find "$lt_build" -name libtorrent-rasterbar.a -print -quit)"
  test -n "$lt_lib"
  cmake --install "$lt_build"
  local static_bridge=ON native_tests=OFF
  if [[ "$family" == macos ]]; then
    static_bridge=OFF
    if [[ "$arch" == "$(uname -m)" ]]; then native_tests=ON; fi
  fi
  cmake -S "$ROOT/src" -B "$slice" -G Ninja \
    -DCMAKE_SYSTEM_NAME="$system" -DCMAKE_OSX_SYSROOT="$sdk" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$minimum" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$WORK/lt-install-$key" \
    -DBOOST_ROOT="$BOOST_ROOT" -DBoost_INCLUDE_DIR="$BOOST_ROOT" \
    -DOPENSSL_INCLUDE_DIR="$ssl_root/include" -DOPENSSL_SSL_LIBRARY="$ssl_root/lib/libssl.a" \
    -DOPENSSL_CRYPTO_LIBRARY="$ssl_root/lib/libcrypto.a" \
    -DLTF_STATIC_BRIDGE="$static_bridge" -DLTF_BUILD_TESTS="$native_tests"
  cmake --build "$slice" --parallel "$JOBS"
  if [[ "$family" == macos ]]; then
    # The first line names the inspected file, which lives under build/apple.
    # Check only dependency entries, not that header.
    if otool -L "$slice/liblibtorrent_flutter.dylib" | sed '1d' | grep -E '/opt/homebrew|/usr/local|build/apple'; then
      echo 'Unexpected local runtime dependency in macOS dylib' >&2; exit 1
    fi
    if [[ "$native_tests" == ON ]]; then ctest --test-dir "$slice" --output-on-failure; fi
    python3 "$ROOT/scripts/check_native_exports.py" "$slice/liblibtorrent_flutter.dylib" --nm nm
  else
    # Preserve archive members with duplicate names; ar extraction loses them.
    # Keep the CMake output separate from the combined archive for safe rebuilds.
    xcrun libtool -static -o "$slice/combined.a" "$slice/liblibtorrent_flutter.a" \
      "$lt_lib" "$ssl_root/lib/libssl.a" "$ssl_root/lib/libcrypto.a"
    python3 "$ROOT/scripts/check_native_exports.py" "$slice/combined.a" --nm nm
  fi
}

if [[ "$PLATFORM" == macos || "$PLATFORM" == all ]]; then
  build_slice macos arm64 macosx arm64-apple-macos11.0 darwin64-arm64-cc
  build_slice macos x86_64 macosx x86_64-apple-macos11.0 darwin64-x86_64-cc
  mkdir -p "$ROOT/prebuilt/macos/universal"
  lipo -create macos-arm64/liblibtorrent_flutter.dylib macos-x86_64/liblibtorrent_flutter.dylib \
    -output "$ROOT/prebuilt/macos/universal/liblibtorrent_flutter.dylib"
  cp "$ROOT/prebuilt/macos/universal/liblibtorrent_flutter.dylib" "$ROOT/macos/"
fi
if [[ "$PLATFORM" == ios || "$PLATFORM" == all ]]; then
  build_slice ios arm64 iphoneos arm64-apple-ios13.0 ios64-xcrun
  build_slice simulator arm64 iphonesimulator arm64-apple-ios13.0-simulator iossimulator-xcrun
  build_slice simulator x86_64 iphonesimulator x86_64-apple-ios13.0-simulator iossimulator-xcrun
  mkdir -p headers simulator-universal "$ROOT/prebuilt/ios"
  cp "$ROOT/src/torrent_bridge.h" headers/
  lipo -create simulator-arm64/combined.a simulator-x86_64/combined.a \
    -output simulator-universal/liblibtorrent_flutter.a
  mkdir -p ios-device
  cp ios-arm64/combined.a ios-device/liblibtorrent_flutter.a
  output="$ROOT/prebuilt/ios/libtorrent_flutter.xcframework"
  if [[ -d "$output" ]]; then rm -rf "$output"; fi
  xcodebuild -create-xcframework -library "$WORK/ios-device/liblibtorrent_flutter.a" -headers "$WORK/headers" \
    -library "$WORK/simulator-universal/liblibtorrent_flutter.a" -headers "$WORK/headers" -output "$output"
  if [[ -d "$ROOT/ios/libtorrent_flutter.xcframework" ]]; then rm -rf "$ROOT/ios/libtorrent_flutter.xcframework"; fi
  cp -R "$output" "$ROOT/ios/"
fi
