#!/usr/bin/env bash
#
# Runtime enforcement of the fail-closed snapshot container-format policy.
#
# The unit tests (SnapshotFormatPolicy) prove the PREDICATE is correct. They
# cannot prove LoadSnapshot() actually consults it. This drives the real
# blockchain.loadtxoutset RPC against a running daemon and proves three things:
#
#   1. a V2 container is rejected with the DEPRECATION error;
#   2. a V3 container at/after shielded activation is rejected with the
#      SHIELDED error;
#   3. neither rejection mutates any state -- before/after and across a restart.
#
# WHY THE EXACT ERROR MATTERS
# ---------------------------
# A rejected snapshot returns an error whatever the reason. If this test merely
# asserted "loadtxoutset failed", it would pass on a checksum mismatch, a
# truncated read, or an unknown base block -- none of which prove the policy
# ran. Those alternative failures are therefore asserted ABSENT, not just
# "something failed".
#
# The fixtures use the daemon's own genesis hash so the base-block check could
# not be what fires. (The policy gate sits at chainstate_service.cpp:9330, ahead
# of the base-block check at :9385, so it should fire first regardless -- but
# the fixture removes any doubt about which check spoke.)
#
# FIXTURE SHAPE
# -------------
# 68-byte header + 32 bytes of padding = 100 bytes:
#
#   off  0  uint32  magic        0x4F545855 "UTXO"
#   off  4  uint32  version      2 or 3
#   off  8  32B     block_hash   genesis, raw (display order reversed)
#   off 40  uint32  block_height 0
#   off 44  uint64  utxo_count   0
#   off 52  uint64  timestamp    0
#   off 60  uint64  reserved     0
#   off 68  32B     padding
#
# Deliberately header-only: if a rejection happened AFTER body parsing began,
# a 100-byte file would produce a truncation error instead of the policy error,
# and this test would fail. So the fixture size itself proves the gate runs
# before the body is read.
#
# Note regtest sets shielded_activation_height = 0, so height 0 satisfies
# `>= activation` and V3 is rejected there. That is the policy working, not a
# quirk of the fixture.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

DATA_DIR="/tmp/dinero_snapfmt_$$"
LOG_FILE="${DATA_DIR}.log"
FIX_DIR="${DATA_DIR}.fixtures"
RPC_PORT=""
P2P_PORT=""
NODE_PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }

fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -n "${NODE_PID}" ]]; then
        kill "${NODE_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do kill -0 "${NODE_PID}" 2>/dev/null || break; sleep 0.2; done
        kill -9 "${NODE_PID}" 2>/dev/null || true
    fi
    if [[ -f "${LOG_FILE}" ]]; then
        printf -- '--- last 80 log lines ---\n' >&2
        tail -80 "${LOG_FILE}" >&2 || true
        printf -- '--- snapshot lines ---\n' >&2
        grep -nE "LoadSnapshot|snapshot|Snapshot" "${LOG_FILE}" | tail -40 >&2 || true
    fi
    printf -- '--- preserved ---\n  datadir: %s\n  log: %s\n  fixtures: %s\n' \
        "${DATA_DIR}" "${LOG_FILE}" "${FIX_DIR}" >&2
    exit 1
}

cleanup() {
    if [[ -n "${NODE_PID}" ]]; then
        kill "${NODE_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do kill -0 "${NODE_PID}" 2>/dev/null || break; sleep 0.2; done
        kill -9 "${NODE_PID}" 2>/dev/null || true
    fi
    [[ "${KEEP_ON_FAIL}" -eq 0 ]] && rm -rf "${DATA_DIR}" "${LOG_FILE}" "${FIX_DIR}" 2>/dev/null || true
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl required"
    command -v jq >/dev/null || fail "jq required"
    command -v python3 >/dev/null || fail "python3 required"
    command -v lsof >/dev/null || fail "lsof required"
    command -v shasum >/dev/null || command -v sha256sum >/dev/null || fail "sha256 tool required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

sha256_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum | awk '{print $1}';
    else shasum -a 256 | awk '{print $1}'; fi
}

pick_ports() {
    local c
    for _ in $(seq 1 40); do
        c=$((36000 + RANDOM % 12000))
        if ! lsof -nP -iTCP:"${c}" -sTCP:LISTEN >/dev/null 2>&1 \
           && ! lsof -nP -iTCP:"$((c + 100))" -sTCP:LISTEN >/dev/null 2>&1; then
            RPC_PORT="${c}"; P2P_PORT="$((c + 100))"; return 0
        fi
    done
    fail "no free port pair"
}

cookie_file() {
    [[ -f "${DATA_DIR}/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/.cookie"; return 0; }
    [[ -f "${DATA_DIR}/regtest/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/regtest/.cookie"; return 0; }
    return 1
}

rpc_raw() {
    local cp c
    cp="$(cookie_file 2>/dev/null || true)"; [[ -n "${cp}" ]] || return 1
    c="$(tr -d '\n' < "${cp}")"; [[ -n "${c}" ]] || return 1
    curl -s --user "${c}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":${2:-[]}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

# Strict envelope (post-#458): a handler failure is a non-null TOP-LEVEL error.
rpc_result() {
    local r; r="$(rpc_raw "$1" "${2:-[]}")" || fail "RPC $1 transport failure"
    jq -e '.error == null and has("result")' <<<"${r}" >/dev/null 2>&1 \
        || fail "RPC $1 failed: ${r}"
    jq -c '.result' <<<"${r}"
}

start_node() {
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" --regtest --datadir="${DATA_DIR}" --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" --p2p.offline=1 --listen=0 >>"${LOG_FILE}" 2>&1 &
    NODE_PID="$!"
}

stop_node() {
    [[ -n "${NODE_PID}" ]] || return 0
    kill "${NODE_PID}" 2>/dev/null || true
    for _ in $(seq 1 100); do
        kill -0 "${NODE_PID}" 2>/dev/null || { NODE_PID=""; return 0; }
        sleep 0.2
    done
    kill -9 "${NODE_PID}" 2>/dev/null || true; NODE_PID=""
}

wait_rpc() {
    for _ in $(seq 1 180); do
        rpc_raw getblockcount '[]' 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1 && return 0
        sleep 0.5
    done
    fail "daemon RPC never came up"
}

# Full observable state. Whole RPC results are digested, so a field that starts
# differing cannot slip past by not being named here.
capture_state() {
    {
        printf 'tip=%s\n'        "$(rpc_result getbestblockhash)"
        printf 'height=%s\n'     "$(rpc_result getblockcount)"
        printf 'txoutset=%s\n'   "$(rpc_result blockchain.gettxoutsetinfo)"
        printf 'commitment=%s\n' "$(rpc_result blockchain.getutreexocommitment)"
        printf 'roots=%s\n'      "$(rpc_result blockchain.getutreexoroots)"
        printf 'stats=%s\n'      "$(rpc_result blockchain.getutreexostats)"
        # AssumeUTXO lifecycle + persisted metadata, best-effort across builds:
        # a missing method must not silently drop the field from the digest.
        printf 'assumeutxo=%s\n' "$(rpc_raw blockchain.getchainstates '[]' 2>/dev/null | jq -c '.result // "unavailable"')"
        printf 'syncheal=%s\n'   "$(rpc_raw blockchain.getsynchealth '[]' 2>/dev/null | jq -c '.result // "unavailable"')"
    } | sha256_stdin
}

make_fixture() {
    local version="$1" out="$2" genesis_display="$3"
    python3 - "$version" "$out" "$genesis_display" <<'PYEOF'
import struct, sys, binascii
version = int(sys.argv[1]); out = sys.argv[2]
genesis_display = sys.argv[3]
# uint256 is stored raw; the RPC reports display order, so reverse it.
raw = binascii.unhexlify(genesis_display)[::-1]
assert len(raw) == 32, len(raw)
hdr  = struct.pack('<I', 0x4F545855)   # magic "UTXO"
hdr += struct.pack('<I', version)      # version
hdr += raw                             # block_hash (genesis)
hdr += struct.pack('<I', 0)            # block_height
hdr += struct.pack('<Q', 0)            # utxo_count
hdr += struct.pack('<Q', 0)            # timestamp
hdr += struct.pack('<Q', 0)            # reserved
assert len(hdr) == 68, len(hdr)
open(out, 'wb').write(hdr + b'\x00' * 32)   # 100 bytes total
PYEOF
    [[ "$(wc -c < "${out}" | tr -d ' ')" == "100" ]] || fail "fixture ${out} is not 100 bytes"
}

# Assert the rejection came from the POLICY, not from some other failure that
# would also produce an error.
expect_rejection() {
    local fixture="$1" expect_substr="$2" label="$3"
    local resp err
    resp="$(rpc_raw blockchain.loadtxoutset "[\"${fixture}\"]")" \
        || fail "${label}: loadtxoutset transport failure"

    # It must be rejected at all.
    if jq -e '.error == null' <<<"${resp}" >/dev/null 2>&1; then
        local inner
        inner="$(jq -r '.result.error // .result.message // empty' <<<"${resp}")"
        [[ -n "${inner}" ]] || fail "${label}: snapshot was ACCEPTED; the policy did not fire"
        err="${inner}"
    else
        err="$(jq -r '.error.message // .error // empty' <<<"${resp}")"
    fi
    [[ -n "${err}" ]] || fail "${label}: rejected but produced no message: ${resp}"

    # It must be rejected for the RIGHT reason.
    grep -qi -- "${expect_substr}" <<<"${err}" \
        || fail "${label}: wrong rejection reason.
  expected to contain: ${expect_substr}
  actual:              ${err}"

    # And explicitly NOT for any of these, which would mean the policy never ran.
    # These patterns must match the OTHER failure paths' actual message text,
    # not a loose phrase. The policy's own V2 message legitimately contains
    # "base block's utreexo_root", so a bare "base block" pattern produced a
    # false positive against a correct rejection.
    local wrong
    for wrong in "checksum" "truncat" "unexpected end" "Snapshot base block" "not available for snapshot"; do
        if grep -qi -- "${wrong}" <<<"${err}"; then
            fail "${label}: rejection mentions '${wrong}' -- that is a different
  failure path, so this does not prove the format policy ran.
  actual: ${err}"
        fi
    done
    pass "${label}: rejected by policy (${err:0:72}...)"
}

# ---------------------------------------------------------------------------

require_tools
pick_ports
mkdir -p "${FIX_DIR}"
info "RPC ${RPC_PORT} / P2P ${P2P_PORT}, offline regtest"

start_node
wait_rpc
GENESIS="$(rpc_result blockchain.getblockhash '[0]' | tr -d '"')"
[[ ${#GENESIS} -eq 64 ]] || fail "unexpected genesis hash: ${GENESIS}"
info "genesis: ${GENESIS}"

make_fixture 2 "${FIX_DIR}/v2.dat" "${GENESIS}"
make_fixture 3 "${FIX_DIR}/v3.dat" "${GENESIS}"
info "fixtures built: 100 bytes each (68-byte header + 32-byte padding)"

BEFORE="$(capture_state)"
info "state before: ${BEFORE:0:16}..."

expect_rejection "${FIX_DIR}/v2.dat" "v2 is no longer supported" "V2 deprecated"
expect_rejection "${FIX_DIR}/v3.dat" "no shielded section"       "V3 post-activation"

AFTER="$(capture_state)"
[[ "${AFTER}" == "${BEFORE}" ]] || fail "state changed after rejected snapshot imports.
  before ${BEFORE}
  after  ${AFTER}
  A rejection must not mutate the UTXO set, forest, lifecycle or metadata."
pass "no state mutation after both rejections"

# A rejection must also leave nothing persisted that only surfaces on reload.
info "restarting to check for persisted damage"
stop_node
start_node
wait_rpc
RESTART="$(capture_state)"
[[ "${RESTART}" == "${BEFORE}" ]] || fail "state changed across restart after rejections.
  before  ${BEFORE}
  restart ${RESTART}
  A rejection persisted something that only appears on reload."
pass "state identical across restart"

pass "LoadSnapshot enforces the container-format policy at runtime"
info "V4 acceptance is covered by AssumeUtxoReplayE2E, which exports a current"
info "V4 snapshot and imports it through this same path."
exit 0
