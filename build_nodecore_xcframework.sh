#!/bin/bash
set -e

# ==============================================================================
# Build NodeCore.xcframework for Apple platforms
#
# Builds the full C++ node as a static library for:
#   1. iOS device (arm64)
#   2. iOS Simulator (arm64, Apple Silicon)
#   3. macOS (universal arm64 + x86_64)
#
# Then packages all three platform slices into NodeCore.xcframework.
#
# Output: build-nodecore/NodeCore.xcframework/
# ==============================================================================

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build-nodecore"
XCFRAMEWORK_OUTPUT="${BUILD_ROOT}/NodeCore.xcframework"
IOS_DEPLOY_TARGET="15.0"
MACOS_DEPLOY_TARGET="$(tr -d '[:space:]' < "${PROJECT_ROOT}/.macos-deployment-target")"

echo "=========================================="
echo " NodeCore.xcframework Build"
echo "=========================================="
echo "Project: ${PROJECT_ROOT}"
echo "Output:  ${XCFRAMEWORK_OUTPUT}"
echo ""

# Single source of truth for the vendored OpenSSL, override with OPENSSL_VERSION=…
# Mirrors scripts/build_openssl_ios.sh and scripts/build-shielded-proverkit-xcframework.sh,
# which already default to 3.5.7. This was pinned to 3.5.6, so every NodeCore
# xcframework silently linked 3.5.6 unless the line was hand-edited first.
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.7}"
OPENSSL_SRC="${PROJECT_ROOT}/third_party/openssl-${OPENSSL_VERSION}"
OPENSSL_SHA256_3_5_7="a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8"
NCPU="${NCPU:-$(sysctl -n hw.ncpu)}"
case "${NCPU}" in
    ''|*[!0-9]*|0)
        echo "ERROR: NCPU must be a positive integer (got '${NCPU}')" >&2
        exit 1
        ;;
esac

# openssl-3.5.7 is a build input, not a committed vendor tree like 3.5.6, so
# fetch it (pinned SHA-256) when absent — otherwise a fresh clone silently
# builds against whatever OpenSSL happens to be on disk.
ensure_openssl_source() {
    [ -f "${OPENSSL_SRC}/Configure" ] && return 0
    [ "${OPENSSL_VERSION}" = "3.5.7" ] || {
        echo "ERROR: no OpenSSL source at ${OPENSSL_SRC} and no pinned SHA for ${OPENSSL_VERSION}" >&2
        exit 1
    }
    echo "==> Downloading openssl-${OPENSSL_VERSION}.tar.gz (pinned SHA-256)"
    local tarball="${PROJECT_ROOT}/third_party/openssl-${OPENSSL_VERSION}.tar.gz"
    curl -fsSL -o "${tarball}" \
        "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
    echo "${OPENSSL_SHA256_3_5_7}  ${tarball}" | shasum -a 256 -c - \
        || { echo "ERROR: openssl-${OPENSSL_VERSION}.tar.gz SHA-256 mismatch" >&2; exit 1; }
    tar -xzf "${tarball}" -C "${PROJECT_ROOT}/third_party/"
    [ -f "${OPENSSL_SRC}/Configure" ] || {
        echo "ERROR: extraction did not produce ${OPENSSL_SRC}/Configure" >&2; exit 1
    }
}
ensure_openssl_source

sync_artifact() {
    local SRC=$1
    local DST=$2
    local TMP="${DST}.tmp.$$"
    mkdir -p "$(dirname "${DST}")"
    cp "${SRC}" "${TMP}"
    mv "${TMP}" "${DST}"
}

clean_openssl_tree() {
    # MUST exclude prebuilt/ because it lives inside OPENSSL_SRC and holds
    # completed macOS + iOS slices that other builds depend on.
    #
    # WARNING: `-prune` does NOT work here because `-delete` auto-enables
    # `-depth`, so find descends into prebuilt/ before evaluating prune on the
    # dir itself, deleting cached .a files as collateral. Use `! -path` instead.
    make distclean >/dev/null 2>&1 || make clean >/dev/null 2>&1 || true
    find . \( -name '*.o' -o -name '*.d' -o -name '*.a' -o -name '*.dylib' \) \
        -type f ! -path './prebuilt/*' -delete >/dev/null 2>&1 || true
    rm -f Makefile Makefile.in configdata.pm builddata.pm
}

sync_openssl_headers() {
    local DST=$1
    rm -rf "${DST}/include"
    mkdir -p "${DST}/include"
    cp -R "${OPENSSL_SRC}/include/openssl" "${DST}/include/openssl"
    find "${DST}/include/openssl" -name '*.in' -delete
}

# ==============================================================================
# Step 0: Clean previous xcframework
# ==============================================================================
rm -rf "${XCFRAMEWORK_OUTPUT}"
mkdir -p "${BUILD_ROOT}"

# ==============================================================================
# Helper: Build vendored OpenSSL for a given iOS target
# ==============================================================================
build_openssl_ios() {
    local OPENSSL_TARGET=$1   # e.g. ios64-xcrun, iossimulator-arm64-xcrun
    local OUTPUT_DIR=$2       # where to copy the built .a files
    local MIN_IOS=$3          # e.g. 15.0
    local IS_SIMULATOR=$4     # "sim" for simulator, empty for device

    echo "  Building OpenSSL for ${OPENSSL_TARGET}..."

    # Same-arch archives can still target different Apple platforms. Rebuild
    # cleanly so stale macOS objects cannot be packaged into iOS slices.
    rm -rf "${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"

    cd "${OPENSSL_SRC}"

    clean_openssl_tree

    # Use correct version flag for device vs simulator
    local VERSION_FLAG
    if [ "${IS_SIMULATOR}" = "sim" ]; then
        VERSION_FLAG="-mios-simulator-version-min=${MIN_IOS}"
    else
        VERSION_FLAG="-miphoneos-version-min=${MIN_IOS}"
    fi

    ./Configure "${OPENSSL_TARGET}" \
        no-shared \
        no-tests \
        no-apps \
        no-ui-console \
        no-async \
        no-engine \
        no-module \
        enable-ec \
        enable-ecdh \
        enable-ecdsa \
        "${VERSION_FLAG}" \
        2>&1 | tail -3

    make -j"${NCPU}" 2>&1 | tail -5

    if [ ! -f "libcrypto.a" ] || [ ! -f "libssl.a" ]; then
        echo "  ERROR: OpenSSL build failed for ${OPENSSL_TARGET}"
        exit 1
    fi

    cp libcrypto.a "${OUTPUT_DIR}/"
    cp libssl.a "${OUTPUT_DIR}/"
    sync_openssl_headers "${OUTPUT_DIR}"

    local SIZE=$(stat -f%z "${OUTPUT_DIR}/libcrypto.a" 2>/dev/null || echo "0")
    echo "  OpenSSL built: libcrypto.a ($(( SIZE / 1024 / 1024 )) MB)"

    cd "${PROJECT_ROOT}"
}

# ==============================================================================
# Helper: Build vendored OpenSSL for macOS target
# ==============================================================================
build_openssl_macos() {
    local OPENSSL_TARGET=$1   # e.g. darwin64-arm64-cc
    local OUTPUT_DIR=$2
    local MIN_MACOS=$3

    echo "  Building OpenSSL for ${OPENSSL_TARGET}..."

    rm -rf "${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"

    cd "${OPENSSL_SRC}"

    clean_openssl_tree

    ./Configure "${OPENSSL_TARGET}" \
        no-shared \
        no-tests \
        no-apps \
        no-ui-console \
        no-async \
        no-engine \
        no-module \
        enable-ec \
        enable-ecdh \
        enable-ecdsa \
        "-mmacosx-version-min=${MIN_MACOS}" \
        2>&1 | tail -3

    make -j"${NCPU}" 2>&1 | tail -5

    if [ ! -f "libcrypto.a" ] || [ ! -f "libssl.a" ]; then
        echo "  ERROR: OpenSSL build failed for ${OPENSSL_TARGET}"
        exit 1
    fi

    cp libcrypto.a "${OUTPUT_DIR}/"
    cp libssl.a "${OUTPUT_DIR}/"
    sync_openssl_headers "${OUTPUT_DIR}"

    local SIZE=$(stat -f%z "${OUTPUT_DIR}/libcrypto.a" 2>/dev/null || echo "0")
    echo "  OpenSSL built: libcrypto.a ($(( SIZE / 1024 / 1024 )) MB)"

    cd "${PROJECT_ROOT}"
}

# ==============================================================================
# Step 1: Build OpenSSL for iOS Device + Simulator
# ==============================================================================
echo ""
echo "[1/6] Building vendored OpenSSL for iOS + macOS..."

OPENSSL_DEVICE_DIR="${BUILD_ROOT}/openssl-device"
OPENSSL_SIM_DIR="${BUILD_ROOT}/openssl-simulator"
OPENSSL_MAC_ARM64_DIR="${BUILD_ROOT}/openssl-macos-arm64"
OPENSSL_MAC_X86_64_DIR="${BUILD_ROOT}/openssl-macos-x86_64"
OPENSSL_DEVICE_CACHE="${OPENSSL_SRC}/prebuilt/ios-arm64"
OPENSSL_SIM_CACHE="${OPENSSL_SRC}/prebuilt/ios-simulator-arm64"
OPENSSL_MAC_ARM64_CACHE="${OPENSSL_SRC}/prebuilt/macos-arm64"
OPENSSL_MAC_X86_64_CACHE="${OPENSSL_SRC}/prebuilt/macos-x86_64"

build_openssl_ios "ios64-xcrun" "${OPENSSL_DEVICE_DIR}" "${IOS_DEPLOY_TARGET}" ""
build_openssl_ios "iossimulator-arm64-xcrun" "${OPENSSL_SIM_DIR}" "${IOS_DEPLOY_TARGET}" "sim"
build_openssl_macos "darwin64-arm64-cc" "${OPENSSL_MAC_ARM64_DIR}" "${MACOS_DEPLOY_TARGET}"
build_openssl_macos "darwin64-x86_64-cc" "${OPENSSL_MAC_X86_64_DIR}" "${MACOS_DEPLOY_TARGET}"

sync_artifact "${OPENSSL_DEVICE_DIR}/libcrypto.a" "${OPENSSL_DEVICE_CACHE}/libcrypto.a"
sync_artifact "${OPENSSL_DEVICE_DIR}/libssl.a" "${OPENSSL_DEVICE_CACHE}/libssl.a"
sync_artifact "${OPENSSL_SIM_DIR}/libcrypto.a" "${OPENSSL_SIM_CACHE}/libcrypto.a"
sync_artifact "${OPENSSL_SIM_DIR}/libssl.a" "${OPENSSL_SIM_CACHE}/libssl.a"
sync_artifact "${OPENSSL_MAC_ARM64_DIR}/libcrypto.a" "${OPENSSL_MAC_ARM64_CACHE}/libcrypto.a"
sync_artifact "${OPENSSL_MAC_ARM64_DIR}/libssl.a" "${OPENSSL_MAC_ARM64_CACHE}/libssl.a"
sync_artifact "${OPENSSL_MAC_X86_64_DIR}/libcrypto.a" "${OPENSSL_MAC_X86_64_CACHE}/libcrypto.a"
sync_artifact "${OPENSSL_MAC_X86_64_DIR}/libssl.a" "${OPENSSL_MAC_X86_64_CACHE}/libssl.a"

echo "[1/6] OpenSSL builds complete"

# ==============================================================================
# Step 2: Build for iOS Device (arm64)
# ==============================================================================
echo ""
echo "[2/6] Building for iOS device (arm64)..."
DEVICE_BUILD="${BUILD_ROOT}/ios-device"
mkdir -p "${DEVICE_BUILD}"

cmake -S "${PROJECT_ROOT}" -B "${DEVICE_BUILD}" \
    -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/ios.toolchain.cmake" \
    -DIOS_PLATFORM=OS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${IOS_DEPLOY_TARGET} \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NODECORE=ON \
    -DDINERO_ENABLE_PORTMAPPING=OFF \
    -DDINERO_VENDORED_OPENSSL_DIR="${OPENSSL_DEVICE_DIR}" \
    -G "Unix Makefiles"

cmake --build "${DEVICE_BUILD}" --target nodecore_ffi --config Release -j${NCPU}

echo "[2/6] Device build complete"

# ==============================================================================
# Step 3: Build for iOS Simulator (arm64 for Apple Silicon)
# ==============================================================================
echo ""
echo "[3/6] Building for iOS Simulator (arm64)..."
SIM_BUILD="${BUILD_ROOT}/ios-simulator"
mkdir -p "${SIM_BUILD}"

cmake -S "${PROJECT_ROOT}" -B "${SIM_BUILD}" \
    -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/ios.toolchain.cmake" \
    -DIOS_PLATFORM=SIMULATOR \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${IOS_DEPLOY_TARGET} \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NODECORE=ON \
    -DDINERO_ENABLE_PORTMAPPING=OFF \
    -DDINERO_VENDORED_OPENSSL_DIR="${OPENSSL_SIM_DIR}" \
    -G "Unix Makefiles"

cmake --build "${SIM_BUILD}" --target nodecore_ffi --config Release -j${NCPU}

echo "[3/6] Simulator build complete"

# ==============================================================================
# Step 4: Build for macOS (arm64 + x86_64)
# ==============================================================================
echo ""
echo "[4/6] Building for macOS (arm64 + x86_64)..."
MAC_ARM64_BUILD="${BUILD_ROOT}/macos-arm64"
MAC_X86_64_BUILD="${BUILD_ROOT}/macos-x86_64"
mkdir -p "${MAC_ARM64_BUILD}" "${MAC_X86_64_BUILD}"

cmake -S "${PROJECT_ROOT}" -B "${MAC_ARM64_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NODECORE=ON \
    -DDINERO_ENABLE_PORTMAPPING=OFF \
    -DDINERO_RELEASE=ON \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOS_DEPLOY_TARGET} \
    -DDINERO_VENDORED_OPENSSL_DIR="${OPENSSL_MAC_ARM64_DIR}" \
    -G "Unix Makefiles"

cmake --build "${MAC_ARM64_BUILD}" --target nodecore_ffi --config Release -j${NCPU}

cmake -S "${PROJECT_ROOT}" -B "${MAC_X86_64_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NODECORE=ON \
    -DDINERO_ENABLE_PORTMAPPING=OFF \
    -DDINERO_RELEASE=ON \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOS_DEPLOY_TARGET} \
    -DDINERO_VENDORED_OPENSSL_DIR="${OPENSSL_MAC_X86_64_DIR}" \
    -G "Unix Makefiles"

cmake --build "${MAC_X86_64_BUILD}" --target nodecore_ffi --config Release -j${NCPU}

echo "[4/6] macOS build complete"

# ==============================================================================
# Step 3: Create fat static libraries
# ==============================================================================
echo ""
echo "[5/6] Collecting static libraries..."

# Find all .a files we need to merge
# The nodecore_ffi target depends on: dinero_core, dinero_consensus, dinero_crypto,
# rocksdb, secp256k1, openssl, zstd, jsoncpp, etc.
# We create a single fat .a containing everything.

create_fat_lib() {
    local PLATFORM=$1
    local BUILD_DIR=$2
    local OUTPUT=$3
    local OPENSSL_DIR=$4

    echo "  Creating fat lib for ${PLATFORM}..."

    # IMPORTANT: Do NOT merge every .a in BUILD_DIR.
    # Host builds contain many test archives that duplicate consensus symbols
    # (e.g. HashUTXO/getCommitment). That can silently select stale logic.
    # Build a deterministic production-only list instead.
    local ALL_LIBS=()

    add_if_exists() {
        local candidate=$1
        if [ -f "${candidate}" ]; then
            ALL_LIBS+=("${candidate}")
        fi
    }

    # NodeCore + production dependency closure (ordered)
    add_if_exists "${BUILD_DIR}/lib/libnodecore_ffi.a"
    add_if_exists "${BUILD_DIR}/libnodecore_ffi.a"
    add_if_exists "${BUILD_DIR}/libdinero_core.a"
    add_if_exists "${BUILD_DIR}/libdinero_rpc_handlers.a"
    add_if_exists "${BUILD_DIR}/libdinerod_proto.a"
    add_if_exists "${BUILD_DIR}/src/storage/libdinero_chainstate.a"
    add_if_exists "${BUILD_DIR}/libdinero_bridge.a"
    add_if_exists "${BUILD_DIR}/libdinero_pool.a"
    add_if_exists "${BUILD_DIR}/libdinero_lightning_keys.a"
    add_if_exists "${BUILD_DIR}/src/wallet/libdinero_wallet.a"
    add_if_exists "${BUILD_DIR}/src/consensus/pq/libdinero_pq.a"
    add_if_exists "${BUILD_DIR}/src/consensus/shielded/libdinero_shielded.a"
    add_if_exists "${BUILD_DIR}/libdinero_consensus.a"
    add_if_exists "${BUILD_DIR}/libdinero_tx_primitives.a"
    add_if_exists "${BUILD_DIR}/libdinero_crypto.a"
    add_if_exists "${BUILD_DIR}/libdinero_zk.a"
    add_if_exists "${BUILD_DIR}/libdinero_gpu_mining.a"
    add_if_exists "${BUILD_DIR}/libvalidation_oracles.a"
    add_if_exists "${BUILD_DIR}/third_party/argon2/libargon2.a"
    add_if_exists "${BUILD_DIR}/_deps/rocksdb-build/librocksdb.a"
    add_if_exists "${BUILD_DIR}/third_party/rocksdb/librocksdb.a"
    add_if_exists "${BUILD_DIR}/_deps/rocksdb-install/lib/librocksdb.a"
    add_if_exists "${BUILD_DIR}/_deps/rocksdb-install/lib64/librocksdb.a"
    add_if_exists "${BUILD_DIR}/third_party/secp256k1-zkp/src/libsecp256k1.a"
    add_if_exists "${BUILD_DIR}/third_party/pqclean/libpqclean_ml_dsa_65.a"
    add_if_exists "${BUILD_DIR}/third_party/hidapi/src/mac/libhidapi.a"
    add_if_exists "${BUILD_DIR}/_deps/jsoncpp-build/src/lib_json/libjsoncpp.a"
    add_if_exists "${BUILD_DIR}/lib/libsqlite3.a"
    add_if_exists "${BUILD_DIR}/libzstd.a"
    add_if_exists "${OPENSSL_DIR}/libcrypto.a"
    add_if_exists "${OPENSSL_DIR}/libssl.a"

    if [ ${#ALL_LIBS[@]} -eq 0 ]; then
        echo "  ERROR: No static libraries found for ${PLATFORM}"
        exit 1
    fi

    echo "  Found ${#ALL_LIBS[@]} production static libraries"

    # Use libtool to merge production libs into one archive.
    libtool -static -o "${OUTPUT}" "${ALL_LIBS[@]}" 2>/dev/null

    local SIZE=$(stat -f%z "${OUTPUT}" 2>/dev/null || echo "0")
    echo "  Fat lib: ${OUTPUT} ($(( SIZE / 1024 / 1024 )) MB)"
}

DEVICE_FAT="${BUILD_ROOT}/libnodecore-device.a"
SIM_FAT="${BUILD_ROOT}/libnodecore-simulator.a"
MAC_ARM64_FAT="${BUILD_ROOT}/libnodecore-macos-arm64.a"
MAC_X86_64_FAT="${BUILD_ROOT}/libnodecore-macos-x86_64.a"
MAC_UNIVERSAL_FAT="${BUILD_ROOT}/libnodecore-macos-universal.a"

create_fat_lib "device" "${DEVICE_BUILD}" "${DEVICE_FAT}" "${OPENSSL_DEVICE_DIR}"
create_fat_lib "simulator" "${SIM_BUILD}" "${SIM_FAT}" "${OPENSSL_SIM_DIR}"
create_fat_lib "macos-arm64" "${MAC_ARM64_BUILD}" "${MAC_ARM64_FAT}" "${OPENSSL_MAC_ARM64_DIR}"
create_fat_lib "macos-x86_64" "${MAC_X86_64_BUILD}" "${MAC_X86_64_FAT}" "${OPENSSL_MAC_X86_64_DIR}"
lipo -create "${MAC_ARM64_FAT}" "${MAC_X86_64_FAT}" -output "${MAC_UNIVERSAL_FAT}"
lipo -verify_arch arm64 x86_64 "${MAC_UNIVERSAL_FAT}" || {
    echo "ERROR: universal macOS NodeCore archive is missing an architecture" >&2
    exit 1
}

# ==============================================================================
# Step 4: Create xcframework
# ==============================================================================
echo ""
echo "[6/6] Creating xcframework..."

# Prepare headers
HEADERS_DIR="${BUILD_ROOT}/Headers"
rm -rf "${HEADERS_DIR}"
mkdir -p "${HEADERS_DIR}"
cp "${PROJECT_ROOT}/include/nodecore/nodecore_ffi.h" "${HEADERS_DIR}/"

# NOTE: No module.modulemap here — DineroDPI imports via bridging header,
# and a modulemap would collide with DPI.xcframework's modulemap in Xcode.

# Build xcframework
xcodebuild -create-xcframework \
    -library "${DEVICE_FAT}" \
    -headers "${HEADERS_DIR}" \
    -library "${SIM_FAT}" \
    -headers "${HEADERS_DIR}" \
    -library "${MAC_UNIVERSAL_FAT}" \
    -headers "${HEADERS_DIR}" \
    -output "${XCFRAMEWORK_OUTPUT}"

echo ""
echo "=========================================="
echo " Build Complete"
echo "=========================================="
echo ""
echo "Output: ${XCFRAMEWORK_OUTPUT}"
echo ""
ls -la "${XCFRAMEWORK_OUTPUT}/"
echo ""
echo "Xcode integration:"
echo "  1. Drag NodeCore.xcframework into your Xcode project"
echo "  2. Link Binary With Libraries: libc++.tbd, libz.tbd, libsqlite3.tbd"
echo "  3. Other Linker Flags: -force_load \$(SRCROOT)/.../NodeCore.xcframework/..."
echo "  4. Import in Swift: import NodeCore"
echo ""
echo "Swift usage:"
echo "  nodecore_start(datadir, config_json)"
echo "  nodecore_set_event_callback(callback, nil)"
echo "  nodecore_watch_script(script, len, label)"
echo "  let status = nodecore_get_status_json()"
echo "  nodecore_stop()"
echo ""
