#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$ROOT_DIR/build-shielded-proverkit-ios}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$ROOT_DIR/artifacts}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-15.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.7}"
OPENSSL_DEVICE_DIR="${OPENSSL_DEVICE_DIR:-$ROOT_DIR/third_party/openssl-${OPENSSL_VERSION}/prebuilt/ios-arm64}"
OPENSSL_SIMULATOR_DIR="${OPENSSL_SIMULATOR_DIR:-$ROOT_DIR/third_party/openssl-${OPENSSL_VERSION}/prebuilt/ios-simulator-arm64}"
# macOS slice. Derived from OPENSSL_VERSION like the iOS dirs above — an earlier
# pass hardcoded openssl-3.3.2 here, which is how the shipped macOS slice ended
# up a full OpenSSL major-minor behind the iOS ones.
OPENSSL_MACOS_DIR="${OPENSSL_MACOS_DIR:-$ROOT_DIR/third_party/openssl-${OPENSSL_VERSION}/prebuilt/macos-arm64}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-$(tr -d '[:space:]' < "$ROOT_DIR/.macos-deployment-target" 2>/dev/null || echo 13.0)}"
# Set BUILD_MACOS=0 to skip the macOS slice (iOS-only, the historical behaviour).
BUILD_MACOS="${BUILD_MACOS:-1}"

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
NMEDIT="$(xcrun --find nmedit)"
[ -x "$NMEDIT" ] || die "xcrun nmedit not found"

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
    -DCMAKE_C_VISIBILITY_PRESET=hidden \
    -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
    -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
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

# Native macOS configure. Same flag set as configure_and_build above, minus the
# iOS toolchain file / IOS_PLATFORM, plus the macOS arch + deployment target.
# The three visibility flags are load-bearing: without them the dylib leaks
# dinero:: globals and the ABI gate in build_dynamic_framework refuses to ship.
configure_and_build_macos() {
  local build_dir="$1"
  local openssl_dir="$2"

  # Deliberately NOT setting CMAKE_SYSTEM_NAME: this is a native macOS build,
  # and setting it switches CMake into cross-compiling mode, where
  # CMAKE_SYSTEM_PROCESSOR is no longer auto-detected. cmake/ThirdParty.cmake
  # compares the vendored OpenSSL's ARCH metadata against CMAKE_SYSTEM_PROCESSOR
  # and hard-fails with "...but this build targets ." when it comes back empty.
  cmake -S "$ROOT_DIR" -B "$build_dir" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
    -DCMAKE_C_VISIBILITY_PRESET=hidden \
    -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
    -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
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

# xcodebuild -create-xcframework rejects a flat (iOS-shaped) .framework for a
# macOS slice; macOS requires the versioned bundle layout. Convert in place.
versionize_macos_framework() {
  local fw="$1"
  [ -d "$fw/Versions" ] && return 0
  mkdir -p "$fw/Versions/A"
  local item
  for item in ShieldedProverKit Headers Modules Resources; do
    [ -e "$fw/$item" ] || continue
    mv "$fw/$item" "$fw/Versions/A/$item"
  done
  # Info.plist lives under Versions/A/Resources for a versioned macOS bundle.
  mkdir -p "$fw/Versions/A/Resources"
  [ -f "$fw/Info.plist" ] && mv "$fw/Info.plist" "$fw/Versions/A/Resources/Info.plist"
  ln -sfn A "$fw/Versions/Current"
  for item in ShieldedProverKit Headers Modules Resources; do
    [ -e "$fw/Versions/Current/$item" ] || continue
    ln -sfn "Versions/Current/$item" "$fw/$item"
  done
}

collect_archives() {
  local build_dir="$1"
  local archives=(
    "$build_dir/src/consensus/shielded/libdinero_shielded.a"
    "$build_dir/src/wallet/libdinero_wallet.a"
    "$build_dir/libdinero_crypto.a"
    "$build_dir/libdinero_consensus.a"
    "$build_dir/libdinero_zk.a"
  )

  local archive
  for archive in "${archives[@]}"; do
    [ -f "$archive" ] || die "missing expected archive $archive"
    printf '%s\n' "$archive"
  done
}

ABI_EXPORTS="$BUILD_ROOT/abi-exports.txt"
write_abi_exports() {
  mkdir -p "$BUILD_ROOT"
  # The complete public C ABI (the functions Swift calls). Everything else —
  # the bundled dinero:: core — must stay hidden so it cannot collide with
  # NodeCore.xcframework's copies.
  cat > "$ABI_EXPORTS" <<'EOF'
_dinero_shielded_build_unshield_bundle
_dinero_shielded_compute_note_commitment
_dinero_shielded_compute_nullifier
_dinero_shielded_derive_address
_dinero_shielded_free_result
EOF
}

# Build ShieldedProverKit as a DYNAMIC framework instead of a static archive.
# A static .a cannot hide its symbols (-fvisibility=hidden is a no-op there), so
# its bundled dinero:: core collides at app-link with NodeCore.xcframework's copy;
# if the two frameworks are from different commits the linker stitches a
# Frankenstein build -> ODR -> memory corruption (the "__next_prime overflow"
# genesis crash). A dylib DOES honor hidden visibility: -exported_symbols_list
# publishes only the C ABI, -dead_strip prunes to the ABI-reachable closure, and
# every internal dinero:: symbol becomes private. The kit then shares ZERO
# linker-visible symbols with NodeCore, so the two may drift commits safely.
build_dynamic_framework() {
  local build_dir="$1"
  local out_dir="$2"
  local openssl_dir="$3"
  local sdk="$4"          # iphoneos | iphonesimulator
  local min_flag="$5"     # -mios-version-min=... | -mios-simulator-version-min=...
  local platform_key="$6" # iPhoneOS | iPhoneSimulator

  local wrapper="$build_dir/src/shielded_prover_kit/libShieldedProverKit.a"
  [ -f "$wrapper" ] || die "missing $wrapper"

  local fw="$out_dir/ShieldedProverKit.framework"
  rm -rf "$fw"
  mkdir -p "$fw/Headers"

  # The dependency closure. -dead_strip keeps only what the ABI actually reaches
  # (measured ~4 MB), so over-listing here (rocksdb/sqlite/etc.) is harmless.
  xcrun -sdk "$sdk" clang++ -dynamiclib -arch arm64 "$min_flag" \
    -install_name "@rpath/ShieldedProverKit.framework/ShieldedProverKit" \
    -Wl,-dead_strip \
    -Wl,-exported_symbols_list,"$ABI_EXPORTS" \
    -Wl,-force_load,"$wrapper" \
    "$build_dir/src/consensus/shielded/libdinero_shielded.a" \
    "$build_dir/src/wallet/libdinero_wallet.a" \
    "$build_dir/libdinero_consensus.a" \
    "$build_dir/libdinero_crypto.a" \
    "$build_dir/libdinero_zk.a" \
    "$build_dir/libdinero_tx_primitives.a" \
    "$build_dir/src/consensus/pq/libdinero_pq.a" \
    "$build_dir/third_party/secp256k1-zkp/src/libsecp256k1.a" \
    "$build_dir/third_party/pqclean/libpqclean_ml_dsa_65.a" \
    "$build_dir/third_party/argon2/libargon2.a" \
    "$build_dir/_deps/jsoncpp-build/src/lib_json/libjsoncpp.a" \
    "$build_dir/_deps/rocksdb-build/librocksdb.a" \
    "$build_dir/lib/libsqlite3.a" \
    "$build_dir/libzstd.a" \
    "$openssl_dir/libcrypto.a" "$openssl_dir/libssl.a" \
    -framework Foundation -framework Security \
    -o "$fw/ShieldedProverKit"

  # Hard gate: the framework must export ONLY the C ABI. Any leaked dinero::
  # global would reintroduce the cross-framework collision.
  local total_globals abi_globals leaked
  total_globals="$(nm -gU "$fw/ShieldedProverKit" 2>/dev/null | grep -cE ' T ' || true)"
  abi_globals="$(nm -gU "$fw/ShieldedProverKit" 2>/dev/null | grep -cE ' T _dinero_shielded_' || true)"
  leaked=$(( total_globals - abi_globals ))
  [ "$leaked" -eq 0 ] || die "framework leaks $leaked non-ABI global symbol(s) — refusing to ship"

  cp "$ROOT_DIR/include/shielded_prover_kit/"*.h "$fw/Headers/"

  # Clang module map so the app imports the C ABI framework-style
  # (<ShieldedProverKit/shielded_prover_kit.h>), as required for a dynamic framework.
  mkdir -p "$fw/Modules"
  cat > "$fw/Modules/module.modulemap" <<'MODMAP'
framework module ShieldedProverKit {
    header "shielded_prover_kit.h"
    export *
}
MODMAP

  # macOS uses LSMinimumSystemVersion; MinimumOSVersion is an iOS-only key and
  # carrying the iOS deployment target into a macOS bundle is simply wrong.
  local min_version_entry
  if [ "$platform_key" = "MacOSX" ]; then
    min_version_entry="<key>LSMinimumSystemVersion</key><string>$MACOS_DEPLOYMENT_TARGET</string>"
  else
    min_version_entry="<key>MinimumOSVersion</key><string>$IOS_DEPLOYMENT_TARGET</string>"
  fi

  cat > "$fw/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>ShieldedProverKit</string>
  <key>CFBundleIdentifier</key><string>com.dinerolabs.ShieldedProverKit</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>ShieldedProverKit</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  $min_version_entry
  <key>CFBundleSupportedPlatforms</key><array><string>$platform_key</string></array>
</dict>
</plist>
PLIST
}

localize_duplicate_nodecore_symbols() {
  local archive="$1"
  local symbols_file
  symbols_file="$(mktemp)"
  cat > "$symbols_file" <<'SYMBOLS'
__ZNK6dinero11Transaction11GetBaseSizeEv
__ZNK6dinero11Transaction12SerializeHexENS_19TxSerializationModeE
__ZNK6dinero11Transaction12SerializeHexEb
__ZNK6dinero11Transaction7GetSizeEv
__ZNK6dinero11Transaction7GetTxidEv
__ZNK6dinero11Transaction8GetWtxidEv
__ZNK6dinero11Transaction9GetWeightEv
__ZNK6dinero11Transaction9SerializeENS_19TxSerializationModeE
__ZNK6dinero11Transaction9SerializeEb
SYMBOLS

  # NodeCore already exports these transaction convenience methods. ProverKit
  # needs its bundled transaction object for standalone serialization, but it
  # must not publish a second app-level owner or Xcode warns on every link.
  "$NMEDIT" -D -R "$symbols_file" "$archive"
  rm -f "$symbols_file"
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

write_abi_exports

echo "[1/5] Building iOS device slice (dynamic framework)"
configure_and_build OS "$DEVICE_BUILD" "$OPENSSL_DEVICE_DIR"
build_dynamic_framework "$DEVICE_BUILD" "$DEVICE_SLICE" "$OPENSSL_DEVICE_DIR" \
  iphoneos "-mios-version-min=$IOS_DEPLOYMENT_TARGET" iPhoneOS

echo "[2/5] Building iOS simulator slice (dynamic framework)"
configure_and_build SIMULATOR "$SIM_BUILD" "$OPENSSL_SIMULATOR_DIR"
build_dynamic_framework "$SIM_BUILD" "$SIM_SLICE" "$OPENSSL_SIMULATOR_DIR" \
  iphonesimulator "-mios-simulator-version-min=$IOS_DEPLOYMENT_TARGET" iPhoneSimulator

XCFRAMEWORK_SLICES=(
  -framework "$DEVICE_SLICE/ShieldedProverKit.framework"
  -framework "$SIM_SLICE/ShieldedProverKit.framework"
)

if [ "$BUILD_MACOS" != "0" ]; then
  MACOS_BUILD="$BUILD_ROOT/macos-arm64"
  MACOS_SLICE="$BUILD_ROOT/slices/macos-arm64"
  [ -f "$OPENSSL_MACOS_DIR/libcrypto.a" ] || die "missing $OPENSSL_MACOS_DIR/libcrypto.a"
  [ -f "$OPENSSL_MACOS_DIR/libssl.a" ] || die "missing $OPENSSL_MACOS_DIR/libssl.a"
  [ -d "$OPENSSL_MACOS_DIR/include/openssl" ] || die "missing $OPENSSL_MACOS_DIR/include/openssl"

  echo "[macos] Building macOS slice (dynamic framework)"
  configure_and_build_macos "$MACOS_BUILD" "$OPENSSL_MACOS_DIR"
  build_dynamic_framework "$MACOS_BUILD" "$MACOS_SLICE" "$OPENSSL_MACOS_DIR" \
    macosx "-mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET" MacOSX
  versionize_macos_framework "$MACOS_SLICE/ShieldedProverKit.framework"
  XCFRAMEWORK_SLICES+=(-framework "$MACOS_SLICE/ShieldedProverKit.framework")
fi

echo "[3/5] Headers are embedded in each .framework"

echo "[4/5] Creating ShieldedProverKit.xcframework (dynamic)"
xcodebuild -create-xcframework \
  "${XCFRAMEWORK_SLICES[@]}" \
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
echo "Framework info:"
lipo -info "$DEVICE_SLICE/ShieldedProverKit.framework/ShieldedProverKit"
lipo -info "$SIM_SLICE/ShieldedProverKit.framework/ShieldedProverKit"
echo "Exported symbols (must be ONLY the C ABI):"
nm -gU "$DEVICE_SLICE/ShieldedProverKit.framework/ShieldedProverKit" 2>/dev/null | grep -E ' T '
cat "$ARTIFACT_DIR/ShieldedProverKit.sha256"
