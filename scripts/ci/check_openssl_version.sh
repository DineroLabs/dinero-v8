#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# CI assertion: the crypto baseline is the one policy pins — reject
# unsupported or unexpected OpenSSL versions instead of discovering them
# in a shipped binary.
#
# Checks three layers:
#   1. The vendored source the build consumed: OPENSSL_FULL_VERSION_STR in
#      third_party/openssl-<expected>/ must equal the expected version.
#   2. The selected static archive and the header resolved by a production
#      compile command must both equal the expected version.
#   3. The built dinerod binary: when the statically-linked OpenSSL embeds a
#      version string, it must contain "OpenSSL <expected>" and must NOT
#      contain any other "OpenSSL 3.x.y" version.
#
# Usage: check_openssl_version.sh <build-dir> [expected-version]
set -euo pipefail

BUILD_DIR="${1:?usage: check_openssl_version.sh <build-dir> [expected-version]}"
EXPECTED="${2:-3.5.7}"

fail() { echo "OPENSSL VERSION ASSERTION FAILED: $*" >&2; exit 1; }

# Layer 1: vendored headers. The tarball generates opensslv.h at build
# time, so the authoritative copy lives under prebuilt/<target>/include.
HDR=""
for cand in third_party/openssl-${EXPECTED}/prebuilt/*/include/openssl/opensslv.h \
            third_party/openssl-${EXPECTED}/include/openssl/opensslv.h; do
    [[ -f "$cand" ]] && { HDR="$cand"; break; }
done
if [[ -n "$HDR" ]]; then
    grep -q "OPENSSL_FULL_VERSION_STR \"${EXPECTED}\"" "$HDR" \
        || fail "vendored ${HDR} does not declare ${EXPECTED}"
    echo "vendored headers declare OpenSSL ${EXPECTED} (${HDR})"
else
    echo "note: no repository-local vendored opensslv.h found for ${EXPECTED} — relying on consumed-library and compiler-resolution checks"
fi

# Layer 2: the library the build ACTUALLY consumed, per CMakeCache. This is
# the portable guarantee: static archives always retain the version-string
# member even when the final link's selective member extraction drops it
# from the binary (observed on Linux: statically-linked dinerod carries no
# OpenSSL version string at all — see PR #419 verification notes).
CACHE="${BUILD_DIR}/CMakeCache.txt"
[[ -f "$CACHE" ]] || fail "CMakeCache.txt not found in ${BUILD_DIR}"
# The authoritative pin the configure resolved:
CACHE_VER=$(grep -E '^DINERO_VENDORED_OPENSSL_VERSION:' "$CACHE" | head -1 | cut -d= -f2 || true)
SYS=$(grep -E '^USE_SYSTEM_OPENSSL:' "$CACHE" | head -1 | cut -d= -f2 || true)
if [[ "$SYS" == "ON" ]]; then
    # System builds are range-enforced at configure time by ThirdParty.cmake;
    # this script's exact-pin check applies to vendored builds.
    echo "note: USE_SYSTEM_OPENSSL=ON — exact-pin check delegated to the CMake range enforcement"
else
    [[ "$CACHE_VER" == "$EXPECTED" ]] \
        || fail "configure resolved DINERO_VENDORED_OPENSSL_VERSION='${CACHE_VER}', policy pins ${EXPECTED}"
    # Explicit dir override if present, else every prebuilt slice of the
    # pinned tree — all must embed exactly the expected version.
    VDIR=$(grep -E '^DINERO_VENDORED_OPENSSL_DIR:' "$CACHE" | head -1 | cut -d= -f2 || true)
    LIBS=()
    if [[ -n "$VDIR" && -f "$VDIR/libcrypto.a" ]]; then
        LIBS=("$VDIR/libcrypto.a")
    else
        for lc in third_party/openssl-${EXPECTED}/prebuilt/*/libcrypto.a; do
            [[ -f "$lc" ]] && LIBS+=("$lc")
        done
    fi
    [[ ${#LIBS[@]} -gt 0 ]] || fail "no prebuilt libcrypto.a found for openssl-${EXPECTED}"
    for lc in "${LIBS[@]}"; do
        AF=$(strings -a "$lc" | grep -Eo 'OpenSSL 3\.[0-9]+\.[0-9]+' | sort -u || true)
        [[ "$AF" == "OpenSSL ${EXPECTED}" ]] \
            || fail "libcrypto at ${lc} embeds '${AF:-nothing}', expected exactly 'OpenSSL ${EXPECTED}'"
    done
    echo "consumed libraries verified: ${#LIBS[@]} prebuilt libcrypto.a slice(s) embed exactly OpenSSL ${EXPECTED}"

    # Layer 2b: preprocess opensslv.h using the exact include ordering from a
    # production target. Checking the configured OPENSSL_INCLUDE_DIR alone is
    # insufficient: a generic include directory can shadow it, causing older
    # headers to compile against the selected library.
    python3 scripts/ci/check_openssl_header_resolution.py \
        "${BUILD_DIR}" "${EXPECTED}"
fi

# Layer 3: the built binary — decisive when a version string IS embedded
# (any mismatching version fails); absence alone is tolerated with a note,
# because selective archive extraction can legitimately omit the version
# object from a static Linux link. Layers 1-2 carry the guarantee then.
BIN=""
for cand in "${BUILD_DIR}/dinerod" "${BUILD_DIR}/bin/dinerod"; do
    [[ -x "$cand" ]] && { BIN="$cand"; break; }
done
[[ -n "$BIN" ]] || fail "built dinerod not found under ${BUILD_DIR} (tried ./ and bin/)"
FOUND=$(strings -a "$BIN" | grep -Eo 'OpenSSL 3\.[0-9]+\.[0-9]+' | sort -u || true)
if [[ -n "$FOUND" ]]; then
    [[ "$FOUND" == "OpenSSL ${EXPECTED}" ]] \
        || fail "dinerod embeds OpenSSL version string(s) '${FOUND//$'\n'/, }' but policy pins exactly ${EXPECTED}"
    echo "binary check: dinerod embeds exactly OpenSSL ${EXPECTED}"
else
    echo "note: dinerod embeds no OpenSSL version string (selective static extraction) — headers + consumed-library layers carry the guarantee"
fi

echo "openssl version assertion OK: build consumed exactly OpenSSL ${EXPECTED}"
