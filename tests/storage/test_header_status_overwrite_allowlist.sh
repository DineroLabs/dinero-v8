#!/usr/bin/env bash
# May 2026 HAVE_UNDO recurrence guard.
#
# ChainDB::updateHeaderStatus() is a full overwrite of persisted header
# status_flags. A partial caller can accidentally strip unrelated bits such as
# BLOCK_HAVE_UNDO. Production code should use setHeaderStatusBits() or
# clearHeaderStatusBits() instead. This test fails if a production caller
# reintroduces a raw updateHeaderStatus() call outside the ChainDB
# implementation/declaration.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

matches="$(
    rg -n '(->|\.)updateHeaderStatus\(' "${REPO_ROOT}/src" "${REPO_ROOT}/include" \
        --glob '!src/storage/chain_db.cpp' \
        --glob '!include/storage/chain_db.h' || true
)"

if [[ -n "${matches}" ]]; then
    echo "[FAIL] raw ChainDB::updateHeaderStatus() production caller(s) found." >&2
    echo "       Use setHeaderStatusBits()/clearHeaderStatusBits(), or update this" >&2
    echo "       regression test with an explicit reviewed exception." >&2
    echo "${matches}" >&2
    exit 1
fi

echo "[✓] no raw updateHeaderStatus() production callers"
