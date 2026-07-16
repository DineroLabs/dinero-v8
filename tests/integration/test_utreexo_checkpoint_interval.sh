#!/usr/bin/env bash
# Forest checkpoint delta campaign phase 1
# (docs/design/forest-checkpoint-deltas.md).
#
# With --utreexo.checkpoint_interval=5 the full forest checkpoint is written
# only at heights % 5 == 0, while the ForestTipMarker tracks the tip every
# block and the per-block delta sidecar keeps DisconnectTip working at
# non-checkpoint heights. Default behavior (no flag) must stay byte-identical:
# checkpoint at every height.
#
# No restarts here on the flagged node: checkpoint+replay restore is campaign
# phase 2. This test covers the WRITER only.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"

DATA_DIR="$(mktemp -d /tmp/dinero_ckpt_interval.XXXXXX)"
LOG_A="${DATA_DIR}/interval_node.log"
LOG_B="${DATA_DIR}/default_node.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_A}" ]] && { printf -- '--- interval node log tail ---\n' >&2; tail -60 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- default node log tail ---\n' >&2; tail -60 "${LOG_B}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}"
    fi
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

RPC_PORT=$((41000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

NODE_DATADIR=""

cookie_file() {
    for c in "${NODE_DATADIR}/.cookie" "${NODE_DATADIR}/regtest/.cookie"; do
        [[ -f "${c}" ]] && { printf '%s\n' "${c}"; return 0; }
    done
    return 1
}

rpc() {
    local method="$1"
    local params_json="${2:-[]}"
    local cookie_path
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    curl -s --max-time 15 --user "$(tr -d '\n' < "${cookie_path}")" \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${NODE_DATADIR}"
    "${DINEROD}" \
        --regtest \
        --datadir="${NODE_DATADIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        "$@" \
        >"${log_file}" 2>&1 &
    PID=$!
}

wait_rpc() {
    for _ in $(seq 1 60); do
        kill -0 "${PID}" 2>/dev/null || return 1
        local r
        r="$(rpc getblockcount 2>/dev/null || true)"
        if [[ -n "${r}" ]] && jq -e '.result != null' <<<"${r}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc stop '[]' >/dev/null 2>&1 || kill "${PID}" 2>/dev/null || true
    for _ in $(seq 1 30); do
        kill -0 "${PID}" 2>/dev/null || { PID=""; return 0; }
        sleep 1
    done
    kill -9 "${PID}" 2>/dev/null || true
    PID=""
}

get_field() {
    local json="$1" filter="$2"
    jq -r "${filter}" <<<"${json}"
}

new_address() {
    local label="$1"
    local r
    r="$(rpc "wallet.getnewaddress" "[\"taproot\",\"${label}\"]")"
    get_field "${r}" '.result.address // .result // empty'
}

mine_to() {
    local blocks="$1" addr="$2"
    local r
    r="$(rpc generatetoaddress "[${blocks},\"${addr}\"]")"
    jq -e '.error == null or (.error | not)' <<<"${r}" >/dev/null || fail "generatetoaddress failed: ${r}"
}

assert_health() {
    local expected_tip="$1" expected_ckpt="$2" label="$3"
    local health
    health="$(rpc "blockchain.getsynchealth" '[]')"
    jq -e \
        --argjson tip "${expected_tip}" \
        --argjson ckpt "${expected_ckpt}" \
        '
        .result.canonical_state_aligned == true and
        .result.active_height == $tip and
        .result.chaindb_tip_height == $tip and
        .result.forest_tip_marker_found == true and
        .result.forest_tip_marker_height == $tip and
        .result.latest_utreexo_checkpoint_found == true and
        .result.latest_utreexo_checkpoint_height == $ckpt
        ' <<<"${health}" >/dev/null || fail "${label}: ${health}"
}

# ─── Node A: interval = 5 ────────────────────────────────────────────────────
NODE_DATADIR="${DATA_DIR}/interval_node"
info "Starting node with --utreexo.checkpoint_interval=5"
start_node "${LOG_A}" --utreexo.checkpoint_interval=5
wait_rpc || fail "interval node did not reach RPC readiness"
grep -q "utreexo.checkpoint_interval=5" "${LOG_A}" || fail "interval flag not acknowledged in log"

ADDR_A="$(new_address "ckpt-interval-miner")"
[[ -n "${ADDR_A}" ]] || fail "no miner address (interval node)"
ADDR_B="$(new_address "ckpt-interval-remine")"
[[ -n "${ADDR_B}" ]] || fail "no re-mine address (interval node)"

mine_to 12 "${ADDR_A}"
# Tip 12; latest full checkpoint must sit at 10 (12 % 5 != 0), marker at 12.
assert_health 12 10 "interval writer state at tip 12"
pass "Every-5 writer: tip 12 carries latest full checkpoint at height 10"

# Disconnect at a NON-checkpoint height — must succeed off the per-block
# delta sidecar alone.
TIP_HASH="$(get_field "$(rpc getblockhash '[12]')" '.result')"
INV="$(rpc "blockchain.invalidateblock" "[\"${TIP_HASH}\"]")"
jq -e '.error == null' <<<"${INV}" >/dev/null || fail "invalidateblock at non-checkpoint height failed: ${INV}"
assert_health 11 10 "post-disconnect state at tip 11"
pass "Disconnect at non-checkpoint height 12 worked via the delta sidecar"

# Re-mine to a DIFFERENT address: a deterministic re-mine to the same
# coinbase target can reproduce the invalidated block bit-for-bit while
# regtest chain time runs ahead of wall clock (see the
# ShieldedReorgDisconnectRestartEquivalence 2026-07-16 diagnosis).
mine_to 1 "${ADDR_B}"
assert_health 12 10 "post-remine state at tip 12"
NEW_TIP_HASH="$(get_field "$(rpc getblockhash '[12]')" '.result')"
[[ "${NEW_TIP_HASH}" != "${TIP_HASH}" ]] || fail "re-mined block equals the invalidated block"
pass "Chain advanced past the invalidated block"

# Cross the next interval boundary: mining to 15 must land a checkpoint at 15.
mine_to 3 "${ADDR_A}"
assert_health 15 15 "interval-boundary state at tip 15"
pass "Interval boundary 15 wrote a fresh full checkpoint"

stop_node

# ─── Node B: default (no flag) — behavior must be unchanged ─────────────────
NODE_DATADIR="${DATA_DIR}/default_node"
RPC_PORT=$((RPC_PORT + 10))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
info "Starting default node (no interval flag)"
start_node "${LOG_B}"
wait_rpc || fail "default node did not reach RPC readiness"

ADDR_D="$(new_address "ckpt-default-miner")"
[[ -n "${ADDR_D}" ]] || fail "no miner address (default node)"
mine_to 7 "${ADDR_D}"
# Default: full checkpoint at EVERY height — latest == tip.
assert_health 7 7 "default writer state at tip 7"
pass "Default behavior unchanged: checkpoint at every height"

stop_node
pass "Utreexo checkpoint interval writer behaves per design"
exit 0
