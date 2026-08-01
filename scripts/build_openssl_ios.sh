#!/usr/bin/env bash
set -euo pipefail

# Build vendored OpenSSL for iOS device + simulator slices, installing
# into the cache layout the ShieldedProverKit xcframework packager consumes:
#
#   third_party/openssl-<version>/prebuilt/ios-arm64/{libcrypto.a,libssl.a,include/openssl/}
#   third_party/openssl-<version>/prebuilt/ios-simulator-arm64/{...}
#
# Version follows the repository-wide crypto baseline (owner policy
# 2026-07-29): 3.5.7 everywhere a shipped configuration links OpenSSL.
# The source tarball is downloaded and SHA-256-verified when absent.
#
# Mirrors the inline iOS-OpenSSL logic in build_nodecore_xcframework.sh so the
# shielded-prover-kit lane can run a one-shot prerequisite instead of the full
# NodeCore xcframework build. Adds header sync because the prover-kit script
# requires include/openssl in each cache directory.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.7}"
OPENSSL_SRC="${ROOT_DIR}/third_party/openssl-${OPENSSL_VERSION}"
OPENSSL_SHA256_3_5_7="a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8"
IOS_DEPLOY_TARGET="${IOS_DEPLOY_TARGET:-15.0}"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

DEVICE_CACHE="${OPENSSL_SRC}/prebuilt/ios-arm64"
SIM_CACHE="${OPENSSL_SRC}/prebuilt/ios-simulator-arm64"

die() { echo "error: $*" >&2; exit 1; }
require() { command -v "$1" >/dev/null 2>&1 || die "$1 not found"; }

require xcrun
require make
require perl
ensure_source() {
    [ -f "${OPENSSL_SRC}/Configure" ] && return 0
    [ "${OPENSSL_VERSION}" = "3.5.7" ] || die "OpenSSL source not found at ${OPENSSL_SRC} and no pinned SHA for ${OPENSSL_VERSION}"
    echo "==> Downloading openssl-${OPENSSL_VERSION}.tar.gz (pinned SHA-256)"
    local tarball="${ROOT_DIR}/third_party/openssl-${OPENSSL_VERSION}.tar.gz"
    curl -fsSL -o "${tarball}" \
        "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
    echo "${OPENSSL_SHA256_3_5_7}  ${tarball}" | shasum -a 256 -c - \
        || die "openssl-${OPENSSL_VERSION}.tar.gz SHA-256 mismatch"
    tar -xzf "${tarball}" -C "${ROOT_DIR}/third_party/"
    [ -f "${OPENSSL_SRC}/Configure" ] || die "extraction did not produce ${OPENSSL_SRC}/Configure"
}
ensure_source

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

sync_headers() {
    local dst="$1"
    rm -rf "${dst}/include"
    mkdir -p "${dst}/include"
    cp -R "${OPENSSL_SRC}/include/openssl" "${dst}/include/openssl"
    find "${dst}/include/openssl" -name '*.in' -delete
}

build_slice() {
    local target="$1"
    local cache_dir="$2"
    local is_sim="$3"
    local version_flag

    if [ "$is_sim" = "sim" ]; then
        version_flag="-mios-simulator-version-min=${IOS_DEPLOY_TARGET}"
    else
        version_flag="-miphoneos-version-min=${IOS_DEPLOY_TARGET}"
    fi

    echo "==> Building OpenSSL for ${target} (${version_flag})"

    rm -rf "${cache_dir}"
    mkdir -p "${cache_dir}"

    pushd "${OPENSSL_SRC}" >/dev/null
    clean_openssl_tree

    ./Configure "${target}" \
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
        "${version_flag}"

    make -j"${NCPU}"

    [ -f libcrypto.a ] || die "${target}: libcrypto.a not produced"
    [ -f libssl.a ] || die "${target}: libssl.a not produced"

    cp libcrypto.a "${cache_dir}/"
    cp libssl.a "${cache_dir}/"
    popd >/dev/null

    sync_headers "${cache_dir}"

    local size_kb
    size_kb=$(( $(stat -f%z "${cache_dir}/libcrypto.a") / 1024 ))
    echo "    libcrypto.a ${size_kb} KB at ${cache_dir}/"
}

build_slice "ios64-xcrun" "${DEVICE_CACHE}" ""
build_slice "iossimulator-arm64-xcrun" "${SIM_CACHE}" "sim"

echo
echo "OpenSSL iOS slices ready:"
ls -la "${DEVICE_CACHE}"
ls -la "${SIM_CACHE}"
