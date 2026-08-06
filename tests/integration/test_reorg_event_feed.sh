#!/usr/bin/env bash
#
# Four gates for the reorg event feed (reorg.status).
#
# Compilation is NOT one of them: every dead subsystem found in this
# repository compiled cleanly. What matters is whether the handler is
# actually wired to something callers can reach, and whether ITS EVENT LOOP
# HAS EVER RUN. A prior manual curl proved reachability but got back
# {"events":[],"total":0} — meaning not one of the four field assignments
# (seq, timestamp, disconnected, connected) had executed on real data. A
# misspelled key, or disconnected/connected swapped, would produce exactly
# the same reply. Gate 2 is the only thing in this project that exercises
# those fields.
#
# Gate 1: the method responds at all (not -32601), and a fresh node reports
#         total 0.
# Gate 2: a real, depth-controlled reorg produces exactly one matching
#         event — asserted against the depths actually forced, not just
#         "an event appeared".
# Gate 2b: an ordinary connect-only advance does NOT count as a reorg. The
#          recorder is keyed on disconnect_path alone; the adjacent log line
#          fires on (disconnect || connect), so a future edit that reused
#          that condition would count every block as a reorg. Nothing else
#          in the suite would catch that.
# Gate 3: a restart records nothing (the ring and total both reset), and
#         boot_id changes, so a consumer can tell a reset apart from data
#         loss. ChainstateService exposes no initial-block-download flag, so
#         whether activation replays the reorg path on startup cannot be
#         settled by reading the code alone -- this gate settles it
#         empirically. If it fails, that is a real finding (the recorder
#         needs a replay guard), not a broken test -- report it, don't
#         weaken the gate.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./reorg_harness.sh
. "${SCRIPT_DIR}/reorg_harness.sh"     # start_node, stop_node, rpc, force_reorg, extend_chain

KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    exit 1
}

cleanup() {
    local rc=$?   # preserve the real verdict; nothing below may override it
    stop_node 2>/dev/null || true
    if [ "${KEEP_ON_FAIL}" != "1" ] && [ -n "${REORG_HARNESS_DATA_DIR:-}" ]; then
        rm -rf "${REORG_HARNESS_DATA_DIR}" "${REORG_HARNESS_LOG:-}" 2>/dev/null || true
    elif [ -n "${REORG_HARNESS_DATA_DIR:-}" ]; then
        info "preserved datadir for inspection: ${REORG_HARNESS_DATA_DIR}"
        info "preserved log for inspection:     ${REORG_HARNESS_LOG:-}"
    fi
    return "${rc}"
}
trap cleanup EXIT

# ── Gate 1: the method answers at all ───────────────────────────────────────
info "Gate 1: reorg.status is reachable on a fresh node"
start_node
answer="$(rpc reorg.status)"
echo "${answer}" | grep -q '"boot_id"' \
    || fail "reorg.status did not return a result: ${answer}"
echo "${answer}" | grep -q -- '-32601' \
    && fail "reorg.status is not registered: ${answer}"
echo "${answer}" | grep -q '"total" *: *0' \
    || fail "a fresh node should report total 0: ${answer}"
pass "reorg.status answers and a fresh node reports total 0"

# ── Gate 2: a real reorg produces a matching event ──────────────────────────
info "Gate 2: forcing a depth-controlled reorg (disconnect=2, connect=3)"
force_reorg --disconnect 2 --connect 3
answer="$(rpc reorg.status)"
echo "${answer}" | grep -q '"total" *: *1' \
    || fail "expected exactly one recorded reorg: ${answer}"
echo "${answer}" | grep -q '"disconnected" *: *2' \
    || fail "recorded depth does not match the forced reorg: ${answer}"
echo "${answer}" | grep -q '"connected" *: *3' \
    || fail "recorded connect depth does not match: ${answer}"
pass "forced reorg recorded with the exact depths forced"

first_boot="$(echo "${answer}" | sed -n 's/.*"boot_id" *: *"\([^"]*\)".*/\1/p')"
[ -n "${first_boot}" ] || fail "could not read boot_id from: ${answer}"

# ── Gate 2b: an ordinary block does NOT count as a reorg ────────────────────
info "Gate 2b: an ordinary connect-only advance must not be counted"
extend_chain --blocks 3            # connect-only, no disconnect
answer="$(rpc reorg.status)"
echo "${answer}" | grep -q '"total" *: *1' \
    || fail "an ordinary block advance was counted as a reorg: ${answer}"
pass "ordinary block advance left the reorg count unchanged"

# ── Gate 3: a restart records nothing ───────────────────────────────────────
info "Gate 3: a restart must not manufacture phantom reorgs"
stop_node
start_node
answer="$(rpc reorg.status)"
echo "${answer}" | grep -q '"total" *: *0' \
    || fail "restart manufactured phantom reorgs: ${answer}"
echo "${answer}" | grep -q '"events" *: *\[\]' \
    || fail "restart left events in the ring: ${answer}"

second_boot="$(echo "${answer}" | sed -n 's/.*"boot_id" *: *"\([^"]*\)".*/\1/p')"
[ -n "${second_boot}" ] || fail "could not read boot_id after restart from: ${answer}"
[ "${first_boot}" != "${second_boot}" ] \
    || fail "boot_id did not change across a restart, so a consumer cannot tell a reset from data loss"
pass "restart recorded nothing and boot_id changed (${first_boot} -> ${second_boot})"

stop_node
echo "OK: all four gates passed"
