#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIXTURE_BUILDER="${ROOT_DIR}/build/test_shielded_reindex_equivalence"
if [[ -x "${ROOT_DIR}/build/tests/integration/shielded_tip_marker_mutator" ]]; then
    MARKER_MUTATOR="${ROOT_DIR}/build/tests/integration/shielded_tip_marker_mutator"
elif [[ -x "${ROOT_DIR}/build/shielded_tip_marker_mutator" ]]; then
    MARKER_MUTATOR="${ROOT_DIR}/build/shielded_tip_marker_mutator"
else
    MARKER_MUTATOR="${ROOT_DIR}/shielded_tip_marker_mutator"
fi
if [[ -x "${ROOT_DIR}/build/tests/integration/shielded_tip_marker_probe" ]]; then
    MARKER_PROBE="${ROOT_DIR}/build/tests/integration/shielded_tip_marker_probe"
elif [[ -x "${ROOT_DIR}/build/shielded_tip_marker_probe" ]]; then
    MARKER_PROBE="${ROOT_DIR}/build/shielded_tip_marker_probe"
else
    MARKER_PROBE="${ROOT_DIR}/shielded_tip_marker_probe"
fi

TMP_ROOT="$(mktemp -d /tmp/dinero_shielded_tip_marker_equivalence.XXXXXX)"
DATA_DIR="${TMP_ROOT}/datadir"
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -d "${TMP_ROOT}" ]]; then
        find "${TMP_ROOT}" -maxdepth 1 -type f -print -exec cat {} \; >&2 2>/dev/null || true
    fi
    exit 1
}
cleanup() {
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${TMP_ROOT}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${FIXTURE_BUILDER}" ]] || fail "fixture builder missing at ${FIXTURE_BUILDER}"
    [[ -x "${MARKER_MUTATOR}" ]] || fail "marker mutator missing at ${MARKER_MUTATOR}"
    [[ -x "${MARKER_PROBE}" ]] || fail "marker probe missing at ${MARKER_PROBE}"
}

emit_fixture() {
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    info "Emitting shielded fixture datadir"
"${FIXTURE_BUILDER}" --emit-datadir "${DATA_DIR}" >/dev/null
}

require_tools

emit_fixture

info "Verifying aligned ShieldedTipMarker against stored tip"
"${MARKER_PROBE}" --datadir "${DATA_DIR}" --verify-tip-height 5 | tail -n 1 > "${TMP_ROOT}/baseline.json"
jq -e '
    .marker_height == 5 and
    .marker_root == .current_root and
    .marker_tree_size == .current_tree_size and
    .marker_nullifier_count == .current_nullifier_count and
    .current_tree_size == 2 and
    .current_nullifier_count == 2
' "${TMP_ROOT}/baseline.json" >/dev/null || fail "baseline shielded marker/state mismatch"
pass "Aligned ShieldedTipMarker verified at stored tip"

info "Retargeting ShieldedTipMarker to height 4 (transparent-only gap to tip)"
"${MARKER_MUTATOR}" --datadir "${DATA_DIR}" --retarget-height 4 >/dev/null
"${MARKER_PROBE}" --datadir "${DATA_DIR}" --verify-tip-height 5 | tail -n 1 > "${TMP_ROOT}/heal.json"
jq -e '
    .marker_height == 5 and
    .marker_root == .current_root and
    .marker_tree_size == .current_tree_size and
    .marker_nullifier_count == .current_nullifier_count and
    .current_tree_size == 2 and
    .current_nullifier_count == 2
' "${TMP_ROOT}/heal.json" >/dev/null || fail "transparent-gap heal did not restore marker to tip"
pass "Harmless stale ShieldedTipMarker auto-healed across shielded-inactive gap"

info "Rewinding shielded state to active tip 0 for startup replay"
"${MARKER_PROBE}" --datadir "${DATA_DIR}" --verify-tip-height 5 --rewind-active-height 0 | tail -n 1 > "${TMP_ROOT}/rewind.json"
jq -e '
    .marker_height == 0 and
    .marker_root == .current_root and
    .marker_tree_size == .current_tree_size and
    .marker_nullifier_count == .current_nullifier_count and
    .current_tree_size == 0 and
    .current_nullifier_count == 0
' "${TMP_ROOT}/rewind.json" >/dev/null || fail "startup rewind did not roll shielded state back to active tip"
pass "Shielded state rewinds cleanly to active tip for startup replay"

emit_fixture

info "Retargeting ShieldedTipMarker to height 2 (shielded-active gap to tip)"
"${MARKER_MUTATOR}" --datadir "${DATA_DIR}" --retarget-height 2 >/dev/null
if "${MARKER_PROBE}" --datadir "${DATA_DIR}" --verify-tip-height 5 > "${TMP_ROOT}/fail.json" 2>&1; then
    fail "harmful stale ShieldedTipMarker unexpectedly validated across shielded-active gap"
fi
pass "Harmful stale ShieldedTipMarker is rejected across shielded-active gap"
