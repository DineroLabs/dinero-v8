#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$ROOT_DIR/build-shielded-proverkit-ios}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$ROOT_DIR/artifacts}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-15.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
OPENSSL_DEVICE_DIR="${OPENSSL_DEVICE_DIR:-$ROOT_DIR/third_party/openssl-3.3.2/prebuilt/ios-arm64}"
OPENSSL_SIMULATOR_DIR="${OPENSSL_SIMULATOR_DIR:-$ROOT_DIR/third_party/openssl-3.3.2/prebuilt/ios-simulator-arm64}"

die() {
  echo "error: $*" >&2
  exit 1
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "$1 not found"
}

require_tool cmake
require_tool xcodebuild
require_tool xcrun
require_tool shasum
require_tool zip

LIBTOOL="$(xcrun --find libtool)"
[ -x "$LIBTOOL" ] || die "xcrun libtool not found"

for openssl_dir in "$OPENSSL_DEVICE_DIR" "$OPENSSL_SIMULATOR_DIR"; do
  [ -f "$openssl_dir/libcrypto.a" ] || die "missing $openssl_dir/libcrypto.a"
  [ -f "$openssl_dir/libssl.a" ] || die "missing $openssl_dir/libssl.a"
  [ -d "$openssl_dir/include/openssl" ] || die "missing $openssl_dir/include/openssl"
done

rm -rf "$BUILD_ROOT" "$ARTIFACT_DIR/ShieldedProverKit.xcframework" \
       "$ARTIFACT_DIR/ShieldedProverKit.xcframework.zip" \
       "$ARTIFACT_DIR/ShieldedProverKit.sha256"
mkdir -p "$BUILD_ROOT" "$ARTIFACT_DIR"

configure_and_build() {
  local platform="$1"
  local build_dir="$2"
  local openssl_dir="$3"

  cmake -S "$ROOT_DIR" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/ios.toolchain.cmake" \
    -DIOS_PLATFORM="$platform" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
    -DBUILD_NODECORE=ON \
    -DDINERO_VENDORED_OPENSSL_DIR="$openssl_dir" \
    -DENABLE_ZK=ON \
    -DENABLE_GPU_MINING=OFF \
    -DENABLE_HARDWARE_WALLETS=OFF \
    -DDINERO_BUILD_QT=OFF \
    -DDINERO_BUILD_MINER=OFF \
    -DDINERO_BUILD_SEEDER=OFF

  cmake --build "$build_dir" --target dinero_shielded_prover_kit -j"$JOBS"
}

collect_archives() {
  local build_dir="$1"
  find "$build_dir" -type f -name '*.a' \
    ! -path '*/CMakeFiles/*' \
    ! -name 'libgtest*.a' \
    ! -name 'libgmock*.a' \
    ! -name 'libbenchmark*.a' \
    | sort
}

merge_archives() {
  local build_dir="$1"
  local out_dir="$2"
  mkdir -p "$out_dir"

  local archives=()
  local found_archive
  while IFS= read -r found_archive; do
    archives+=("$found_archive")
  done < <(collect_archives "$build_dir")
  local wrapper="$build_dir/src/shielded_prover_kit/libShieldedProverKit.a"
  [ -f "$wrapper" ] || die "missing $wrapper"
  [ "${#archives[@]}" -gt 0 ] || die "no static archives found in $build_dir"

  # Put the wrapper first for readability in ar listings; libtool handles
  # duplicate object names better than hand-unpacking archives.
  local merged=("$wrapper")
  local archive
  for archive in "${archives[@]}"; do
    if [ "$archive" != "$wrapper" ]; then
      merged+=("$archive")
    fi
  done

  "$LIBTOOL" -static -o "$out_dir/libShieldedProverKit.a" "${merged[@]}"
}

copy_headers() {
  local headers_dir="$1"
  rm -rf "$headers_dir"
  mkdir -p "$headers_dir/shielded_prover_kit"
  cp "$ROOT_DIR/include/shielded_prover_kit/"*.h \
     "$headers_dir/shielded_prover_kit/"
}

DEVICE_BUILD="$BUILD_ROOT/ios-arm64"
SIM_BUILD="$BUILD_ROOT/ios-simulator-arm64"
DEVICE_SLICE="$BUILD_ROOT/slices/ios-arm64"
SIM_SLICE="$BUILD_ROOT/slices/ios-simulator-arm64"
HEADERS_DIR="$BUILD_ROOT/Headers"

echo "[1/5] Building iOS device slice"
configure_and_build OS "$DEVICE_BUILD" "$OPENSSL_DEVICE_DIR"
merge_archives "$DEVICE_BUILD" "$DEVICE_SLICE"

echo "[2/5] Building iOS simulator slice"
configure_and_build SIMULATOR "$SIM_BUILD" "$OPENSSL_SIMULATOR_DIR"
merge_archives "$SIM_BUILD" "$SIM_SLICE"

echo "[3/5] Preparing headers"
copy_headers "$HEADERS_DIR"

echo "[4/5] Creating ShieldedProverKit.xcframework"
xcodebuild -create-xcframework \
  -library "$DEVICE_SLICE/libShieldedProverKit.a" -headers "$HEADERS_DIR" \
  -library "$SIM_SLICE/libShieldedProverKit.a" -headers "$HEADERS_DIR" \
  -output "$ARTIFACT_DIR/ShieldedProverKit.xcframework"

echo "[5/5] Zipping and hashing"
(cd "$ARTIFACT_DIR" && zip -qry ShieldedProverKit.xcframework.zip ShieldedProverKit.xcframework)
shasum -a 256 "$ARTIFACT_DIR/ShieldedProverKit.xcframework.zip" \
  > "$ARTIFACT_DIR/ShieldedProverKit.sha256"

echo
echo "Artifacts:"
echo "  $ARTIFACT_DIR/ShieldedProverKit.xcframework"
echo "  $ARTIFACT_DIR/ShieldedProverKit.xcframework.zip"
echo "  $ARTIFACT_DIR/ShieldedProverKit.sha256"
echo
echo "Library info:"
lipo -info "$DEVICE_SLICE/libShieldedProverKit.a"
lipo -info "$SIM_SLICE/libShieldedProverKit.a"
cat "$ARTIFACT_DIR/ShieldedProverKit.sha256"
