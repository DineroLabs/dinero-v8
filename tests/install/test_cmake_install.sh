#!/usr/bin/env bash
# Phase C (Dinero Core 1.0) — `cmake --install` smoke test.
#
# Verifies that the install target produces the contract-required
# layout under both manual-mode and packaged-mode prefixes:
#   - ${PREFIX}/bin/dinerod                  (executable)
#   - ${PREFIX}/bin/dinero-cli               (executable)
#   - ${PREFIX}/share/doc/dinero/dinero.conf.example         (regular file)
#   - ${PREFIX}/share/doc/dinero/dinero.service.example      (regular file)
#   - ${PREFIX}/share/doc/dinero/journald-dinero.conf.example (regular file)
#
# This test does NOT exercise the .deb packaging (Phase E) or the
# fleet migration (Phase F). It only verifies the CMake install rules
# themselves. The rules use ${CMAKE_INSTALL_PREFIX} so the same install
# command works for `--prefix /usr/local` (manual mode) and
# `--prefix /usr` (packaged mode); the test runs both into temp dirs.
#
# Test passes when:
#   1. Manual-mode install succeeds and produces all 4 expected files
#   2. Packaged-mode install (DESTDIR + --prefix=/usr) produces the
#      same files at the /usr-prefixed paths
#   3. Installed dinerod binary runs and prints --version
#   4. Installed dinero-cli binary runs and prints --help text

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

if [[ ! -x "${BUILD_DIR}/dinerod" ]]; then
    echo "[FAIL] expected ${BUILD_DIR}/dinerod to exist (run cmake --build first)" >&2
    exit 1
fi
if [[ ! -x "${BUILD_DIR}/dinero-cli" ]]; then
    echo "[FAIL] expected ${BUILD_DIR}/dinero-cli to exist (run cmake --build first)" >&2
    exit 1
fi

pass() { echo "  [✓] $1"; }
fail() { echo "  [FAIL] $1" >&2; exit 1; }

assert_file() {
    [[ -f "$1" ]] || fail "missing file: $1"
}
assert_exec() {
    [[ -x "$1" ]] || fail "not executable: $1"
}

# ──────────────────────────────────────────────────────────────────
# Property #1 — Manual-mode install (--prefix /usr/local equivalent)
# ──────────────────────────────────────────────────────────────────
echo "Property #1: manual-mode install (default-prefix layout)"
TMP_MANUAL="$(mktemp -d -t dinero-install-manual.XXXXXX)"
trap 'rm -rf "${TMP_MANUAL}"' EXIT

cmake --install "${BUILD_DIR}" --prefix "${TMP_MANUAL}" >/dev/null 2>&1 || \
    fail "cmake --install (manual mode) failed"

assert_exec "${TMP_MANUAL}/bin/dinerod"
assert_exec "${TMP_MANUAL}/bin/dinero-cli"
assert_exec "${TMP_MANUAL}/bin/dinero-backup"
assert_exec "${TMP_MANUAL}/bin/dinero-prepare-upgrade"
assert_file "${TMP_MANUAL}/share/doc/dinero/dinero.conf.example"
assert_file "${TMP_MANUAL}/share/doc/dinero/dinero.service.example"
assert_file "${TMP_MANUAL}/share/doc/dinero/journald-dinero.conf.example"
pass "all 7 expected files present under manual-mode prefix"

# ──────────────────────────────────────────────────────────────────
# Property #2 — Installed dinerod prints --version (sanity: not a stub)
# ──────────────────────────────────────────────────────────────────
echo "Property #2: installed dinerod is functional"
VERSION_OUT="$("${TMP_MANUAL}/bin/dinerod" --version 2>&1 || true)"
echo "${VERSION_OUT}" | grep -qE "^dinerod " || \
    fail "dinerod --version did not produce 'dinerod <commit>' line: ${VERSION_OUT}"
pass "dinerod --version: $(echo "${VERSION_OUT}" | grep -E "^dinerod " | head -1)"

# ──────────────────────────────────────────────────────────────────
# Property #3 — Installed dinero-cli prints help text (sanity)
# ──────────────────────────────────────────────────────────────────
echo "Property #3: installed dinero-cli is functional"
HELP_OUT="$("${TMP_MANUAL}/bin/dinero-cli" --help 2>&1 | head -3 || true)"
echo "${HELP_OUT}" | grep -qi "dinero" || \
    fail "dinero-cli --help did not print 'dinero': ${HELP_OUT}"
pass "dinero-cli --help responsive"

# ──────────────────────────────────────────────────────────────────
# Property #4 — Config example is non-empty and references rpcport
# ──────────────────────────────────────────────────────────────────
echo "Property #4: dinero.conf.example documents core config keys"
CONF="${TMP_MANUAL}/share/doc/dinero/dinero.conf.example"
[[ -s "${CONF}" ]] || fail "config example is empty"
grep -q "rpcport" "${CONF}" || fail "config example missing 'rpcport' key"
grep -q "addnode" "${CONF}" || fail "config example missing 'addnode' key"
pass "config example documents rpcport, addnode (and more)"

# ──────────────────────────────────────────────────────────────────
# Property #5 — systemd unit example matches locked spec shape
# ──────────────────────────────────────────────────────────────────
echo "Property #5: systemd unit example matches Core 1.0 contract"
UNIT="${TMP_MANUAL}/share/doc/dinero/dinero.service.example"
[[ -s "${UNIT}" ]] || fail "systemd unit example is empty"
grep -qE "^User=dinero" "${UNIT}" || fail "unit missing 'User=dinero' (spec §1.1/§1.2)"
grep -qE "^ExecStart=" "${UNIT}" || fail "unit missing ExecStart="
grep -qE "^\[Install\]" "${UNIT}" || fail "unit missing [Install] section"
grep -qE "^WantedBy=multi-user\.target" "${UNIT}" || \
    fail "unit missing WantedBy=multi-user.target"
pass "systemd unit example: User=dinero, ExecStart, [Install], WantedBy"

# ──────────────────────────────────────────────────────────────────
# Property #6 — Packaged-mode prefix (/usr) lands files at expected paths
# ──────────────────────────────────────────────────────────────────
echo "Property #6: packaged-mode prefix (--prefix /usr) layout"
TMP_PKG="$(mktemp -d -t dinero-install-pkg.XXXXXX)"
trap 'rm -rf "${TMP_MANUAL}" "${TMP_PKG}"' EXIT

# Use DESTDIR so we don't need root. Effective install root becomes
# ${TMP_PKG}/usr/... — exactly what the .deb build will produce.
DESTDIR="${TMP_PKG}" cmake --install "${BUILD_DIR}" --prefix /usr >/dev/null 2>&1 || \
    fail "cmake --install (packaged mode, DESTDIR + --prefix /usr) failed"

assert_exec "${TMP_PKG}/usr/bin/dinerod"
assert_exec "${TMP_PKG}/usr/bin/dinero-cli"
assert_exec "${TMP_PKG}/usr/bin/dinero-backup"
assert_exec "${TMP_PKG}/usr/bin/dinero-prepare-upgrade"
assert_file "${TMP_PKG}/usr/share/doc/dinero/dinero.conf.example"
assert_file "${TMP_PKG}/usr/share/doc/dinero/dinero.service.example"
assert_file "${TMP_PKG}/usr/share/doc/dinero/journald-dinero.conf.example"
pass "packaged-mode layout: /usr/bin/{dinerod,dinero-cli,dinero-backup,dinero-prepare-upgrade} + /usr/share/doc/dinero/*"

# ──────────────────────────────────────────────────────────────────
# Property #7 (Phase D.1) — journald drop-in example shape
# ──────────────────────────────────────────────────────────────────
echo "Property #7: journald drop-in example matches retention contract"
JLD="${TMP_MANUAL}/share/doc/dinero/journald-dinero.conf.example"
[[ -s "${JLD}" ]] || fail "journald drop-in is empty"
grep -qE "^\[Journal\]" "${JLD}" || fail "journald drop-in missing [Journal] section"
grep -qE "^MaxRetentionSec=" "${JLD}" || \
    fail "journald drop-in missing MaxRetentionSec= (spec §1.1 row 6)"
grep -qE "^SystemMaxUse=" "${JLD}" || \
    fail "journald drop-in missing SystemMaxUse= (spec §1.1 row 6)"
pass "journald drop-in: [Journal] + MaxRetentionSec + SystemMaxUse"

echo
echo "✅ cmake --install: 7/7 properties hold"
