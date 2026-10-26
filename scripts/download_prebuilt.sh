#!/usr/bin/env bash
# download_prebuilt.sh — Download prebuilt native libraries from a GitHub Release
# Usage: LIBTORRENT_FLUTTER_RELEASE_BASE_URL=<release-directory-url> bash scripts/download_prebuilt.sh
set -euo pipefail

BASE_URL="${LIBTORRENT_FLUTTER_RELEASE_BASE_URL:?Set to the matching release directory, e.g. https://github.com/OWNER/REPO/releases/download/v2.0.1-dev.1}"
cd "$(dirname "$0")/.."
DOWNLOAD_DIR="$(mktemp -d)"
trap 'rm -rf "$DOWNLOAD_DIR"' EXIT

download_and_extract() {
  local name="$1"
  local dest="$2"
  local zip="${name}.zip"
  echo "→ Downloading ${zip}..."
  curl -fsSL "${BASE_URL}/${zip}" -o "${DOWNLOAD_DIR}/${zip}"
  mkdir -p "$dest"
  unzip -o "${DOWNLOAD_DIR}/${zip}" -d "$dest"
  rm "${DOWNLOAD_DIR}/${zip}"
  echo "  ✅ Extracted to ${dest}"
}

# Linux
download_and_extract "linux-native-lib-x64"   "prebuilt/linux/x64"
download_and_extract "linux-native-lib-arm64" "prebuilt/linux/arm64"

# macOS
download_and_extract "macos-native-lib" "prebuilt/macos"
mkdir -p macos
cp prebuilt/macos/universal/liblibtorrent_flutter.dylib macos/

# Windows
download_and_extract "windows-native-lib-x64"   "prebuilt/windows/x64"
download_and_extract "windows-native-lib-arm64" "prebuilt/windows/arm64"

# Android
download_and_extract "android-native-lib-arm64-v8a"   "prebuilt/android/arm64-v8a"
download_and_extract "android-native-lib-armeabi-v7a" "prebuilt/android/armeabi-v7a"
download_and_extract "android-native-lib-x86_64"      "prebuilt/android/x86_64"

# iOS (xcframework)
mkdir -p prebuilt/ios
download_and_extract "ios-native-lib" "prebuilt/ios/libtorrent_flutter.xcframework"
# Also copy to ios/ for CocoaPods
rm -rf ios/libtorrent_flutter.xcframework
cp -R prebuilt/ios/libtorrent_flutter.xcframework ios/

# Keep the reviewed certificate bundle shipped with this source version.
echo "All platform artifacts downloaded from ${BASE_URL}"
