#!/usr/bin/env bash
#
# Four gates for the reorg event feed (reorg.status).
#
# Compilation is NOT one of them: every dead subsystem found in this
# repository compiled cleanly. What matters is whether the handler is
# actually wired to something callers can reach, whether ITS EVENT LOOP HAS
# EVER RUN, and whether CI ACTUALLY RUNS THIS FILE. A prior manual curl
# proved reachability but got back {"events":[],"total":0} — meaning not one
# of the four field assignments (seq, timestamp, disconnected, connected)
# had executed on real data. A misspelled key, or disconnected/connected
# swapped, would produce exactly the same reply. Gate 2 is the only thing in
# this project that exercises those fields — and it only proves that IF its
# own assertions are exact and IF it is wired into a CI lane that actually
# selects it. Round 1 of this test shipped with three separate ways to pass
# vacuously; see the fix-round note below each gate.
#
# Gate 1: the method responds at all (not -32601), and a fresh node reports
#         total 0.
# Gate 2: a real, depth-controlled reorg produces exactly one matching
#         event — asserted with exact JSON comparison (not substring grep:
#         "total" *: *1 also matches 10..19), and disconnected/connected/seq/
#         timestamp are all read off the SAME event by index, since
#         independent greps could each match a different event once the ring
#         holds more than one. seq and timestamp are asserted because
#         Gate 2 is the ONLY thing exercising those two field assignments;
#         without asserting them, renaming seq to sequence in the handler
#         would keep the whole suite green.
# Gate 2b: an ordinary connect-only advance does NOT count as a reorg. The
#          recorder is keyed on disconnect_path alone; the adjacent log line
#          fires on (disconnect || connect), so a future edit that reused
#          that condition would count every block as a reorg. Nothing else
#          in the suite would catch that -- PROVIDED the positive control
#          below actually mined blocks: generatetoaddress reports failure as
#          a bare {code,message} with no "error" member, so the harness's
#          own failure check does not catch a silently-failed mine, and a
#          zero-block extend_chain would leave the counter untouched and
#          this gate would pass having tested nothing.
# Gate 3: a restart records nothing (the ring and total both reset), and
#         boot_id changes, so a consumer can tell a reset apart from data
#         loss. ChainstateService exposes no initial-block-download flag, so
#         whether activation replays the reorg path on startup cannot be
#         settled by reading the code alone -- this gate settles it
#         empirically. If it fails, that is a real finding (the recorder
#         needs a replay guard), not a broken test -- report it, don't
#         weaken the gate. Its own positive control checks that the height
#         survived the restart: a node that failed to open its datadir and
#         started from genesis would also report total:0 with an empty
#         ring, passing for entirely the wrong reason (no completed reorg
#         existed in its history to replay in the first place).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./reorg_harness.sh
. "${SCRIPT_DIR}/reorg_harness.sh"     # start_node, stop_node, rpc, rpc_result, force_reorg, extend_chain

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

command -v python3 >/dev/null 2>&1 || fail "python3 is required for exact JSON field comparison"

# json_field <json> <key> -> the scalar value, or empty.
#
# <key> may be a dotted path, e.g. "events.0.disconnected", to reach into the
# SAME event by index -- deliberately not a second independent grep, which
# could match a different event once the ring holds more than one.
#
# Exact comparison via python3, never a substring grep: `grep '"total" *: *1'`
# also matches "total": 10 through 19, and chainstate_service.cpp's own
# comment at the Record() call site warns that an aborted reorg retry path
# can turn one logical reorg into N records -- a false pass from the
# substring gate would land precisely on the regression the source flags as
# dangerous.
json_field() {
    printf '%s' "$1" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
node = doc.get("result", doc)
for key in sys.argv[1].split("."):
    if node is None:
        break
    if isinstance(node, list):
        node = node[int(key)]
    else:
        node = node.get(key)
print("" if node is None else node)
' "$2"
}

assert_field() {  # assert_field <json> <key> <expected> <message>
    local actual
    actual="$(json_field "$1" "$2")"
    [ "${actual}" = "$3" ] || fail "$4 (expected $2=$3, got '${actual}'): $1"
}

assert_total() {  # assert_total <expected> <message>
    assert_field "$(rpc reorg.status)" total "$1" "$2"
}

# ── Gate 1: the method answers at all ───────────────────────────────────────
info "Gate 1: reorg.status is reachable on a fresh node"
start_node
answer="$(rpc reorg.status)"
echo "${answer}" | grep -q '"boot_id"' \
    || fail "reorg.status did not return a result: ${answer}"
echo "${answer}" | grep -q -- '-32601' \
    && fail "reorg.status is not registered: ${answer}"
assert_field "${answer}" total 0 "a fresh node should report total 0"
pass "reorg.status answers and a fresh node reports total 0"

# ── Gate 2: a real reorg produces a matching event ──────────────────────────
info "Gate 2: forcing a depth-controlled reorg (disconnect=2, connect=3)"
force_reorg --disconnect 2 --connect 3
answer="$(rpc reorg.status)"
assert_field "${answer}" total 1 "expected exactly one recorded reorg"
# Depths, seq and timestamp all read off events.0 -- the SAME event -- by
# index, not matched independently.
assert_field "${answer}" events.0.disconnected 2 "recorded disconnect depth is wrong"
assert_field "${answer}" events.0.connected 3 "recorded connect depth is wrong"
assert_field "${answer}" events.0.seq 1 "first recorded event should have seq 1"
timestamp="$(json_field "${answer}" events.0.timestamp)"
case "${timestamp}" in
    ????-??-??T??:??:??Z) : ;;
    *) fail "timestamp is not RFC 3339 UTC: '${timestamp}'" ;;
esac
pass "forced reorg recorded with the exact depths, seq and a well-formed timestamp"

first_boot="$(echo "${answer}" | sed -n 's/.*"boot_id" *: *"\([^"]*\)".*/\1/p')"
[ -n "${first_boot}" ] || fail "could not read boot_id from: ${answer}"

# ── Gate 2b: an ordinary block does NOT count as a reorg ────────────────────
info "Gate 2b: an ordinary connect-only advance must not be counted"
# POSITIVE CONTROL FIRST. Without it this gate is unfalsifiable: if
# extend_chain silently mines zero blocks, the counter trivially stays at 1
# and the gate reports PASS having tested nothing. This is a real failure
# mode, not a hypothetical: generatetoaddress reports failure as a bare
# {code,message} with no top-level "error" member, which the harness's own
# rpc-failed check does not catch.
height_before="$(rpc_result blockchain.getblockcount)"
extend_chain --blocks 3            # connect-only, no disconnect
height_after="$(rpc_result blockchain.getblockcount)"
[ "$((height_after - height_before))" -eq 3 ] \
    || fail "positive control failed: expected +3 blocks, got ${height_before} -> ${height_after}"

assert_total 1 "an ordinary block advance was counted as a reorg"
pass "ordinary block advance mined (verified) and left the reorg count unchanged"

# ── Gate 3: a restart records nothing ───────────────────────────────────────
info "Gate 3: a restart must not manufacture phantom reorgs"
height_before_restart="$(rpc_result blockchain.getblockcount)"
stop_node
start_node

# POSITIVE CONTROL: prove the node reloaded the SAME chain. A node that
# failed to open the datadir and started from genesis would also report
# total:0 with an empty ring -- passing this gate for entirely the wrong
# reason, since there would be no completed reorg in its history to replay
# in the first place.
height_after_restart="$(rpc_result blockchain.getblockcount)"
[ "${height_after_restart}" = "${height_before_restart}" ] \
    || fail "node did not reload the same chain: ${height_before_restart} -> ${height_after_restart}"

answer="$(rpc reorg.status)"
assert_field "${answer}" total 0 "restart manufactured phantom reorgs"
events_len="$(printf '%s' "${answer}" | python3 -c \
    'import json,sys; print(len(json.load(sys.stdin).get("result",{}).get("events",[])))')"
[ "${events_len}" = "0" ] || fail "restart left ${events_len} events in the ring: ${answer}"

second_boot="$(echo "${answer}" | sed -n 's/.*"boot_id" *: *"\([^"]*\)".*/\1/p')"
[ -n "${second_boot}" ] || fail "could not read boot_id after restart from: ${answer}"
[ "${first_boot}" != "${second_boot}" ] \
    || fail "boot_id did not change across a restart, so a consumer cannot tell a reset from data loss"
pass "restart reloaded the same chain (height ${height_after_restart}), recorded nothing, and boot_id changed (${first_boot} -> ${second_boot})"

stop_node
echo "OK: all four gates passed"
