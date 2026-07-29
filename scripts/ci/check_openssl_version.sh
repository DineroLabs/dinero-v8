#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# CI assertion: the crypto baseline is the one policy pins — reject
# unsupported or unexpected OpenSSL versions instead of discovering them
# in a shipped binary.
#
# Checks two layers:
#   1. The vendored source the build consumed: OPENSSL_FULL_VERSION_STR in
#      third_party/openssl-<expected>/ must equal the expected version.
#   2. The built dinerod binary: the statically-linked OpenSSL embeds its
#      version string; it must contain "OpenSSL <expected>" and must NOT
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
    echo "note: no vendored opensslv.h found for ${EXPECTED} — relying on binary check"
fi

# Layer 2: the built binary's embedded version string.
BIN="${BUILD_DIR}/dinerod"
[[ -x "$BIN" ]] || fail "built binary not found at ${BIN}"

FOUND=$(strings "$BIN" | grep -Eo 'OpenSSL 3\.[0-9]+\.[0-9]+' | sort -u || true)
[[ -n "$FOUND" ]] || fail "no OpenSSL version string embedded in ${BIN} — not statically linked as expected?"

echo "OpenSSL version strings embedded in dinerod:"
echo "$FOUND" | sed 's/^/  /'

if [[ "$FOUND" != "OpenSSL ${EXPECTED}" ]]; then
    fail "dinerod links OpenSSL '${FOUND//$'\n'/, }' but policy pins exactly ${EXPECTED}"
fi

echo "openssl version assertion OK: dinerod statically links exactly OpenSSL ${EXPECTED}"
