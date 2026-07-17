#!/usr/bin/env bash
# Forest checkpoint delta campaign phase 3 — restart torture
# (docs/design/forest-checkpoint-deltas.md, test plan item 4, regtest leg).
#
# An every-5-checkpoint node is cycled through mixed SIGKILL/SIGTERM
# restarts while mining blocks that include real spends (wallet
# self-sends → forest deletions in the replayed deltas). After EVERY
# kill the restart must come back canonically aligned at the committed
# tip — via checkpoint + sidecar replay when the tip is off-interval —
# and keep advancing. This is the campaign invariant under sustained
# crash pressure.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"

DATA_DIR="$(mktemp -d /tmp/dinero_delta_torture.XXXXXX)"
PID=""
KEEP_ON_FAIL=0
CYCLES="${DELTA_TORTURE_CYCLES:-8}"

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    tail -50 "${DATA_DIR}"/node_*.log >&2 2>/dev/null || true
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

RPC_PORT=$((43000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
NODE_DATADIR="${DATA_DIR}/node"

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
    curl -s --max-time 20 --user "$(tr -d '\n' < "${cookie_path}")" \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

start_node() {
    local log_file="$1"
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
        --utreexo.checkpoint_interval=5 \
        >"${log_file}" 2>&1 &
    PID=$!
}

wait_rpc() {
    for _ in $(seq 1 90); do
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

wait_dead() {
    for _ in $(seq 1 60); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 1
    done
    return 1
}

get_field() { jq -r "$2" <<<"$1"; }

mine_to() {
    local r
    r="$(rpc generatetoaddress "[$1,\"$2\"]")"
    jq -e '.error == null or (.error | not)' <<<"${r}" >/dev/null || fail "generatetoaddress failed: ${r}"
}

assert_aligned_at() {
    local expected_tip="$1" label="$2"
    local health
    health="$(rpc "blockchain.getsynchealth" '[]')"
    jq -e --argjson tip "${expected_tip}" '
        .result.canonical_state_aligned == true and
        .result.active_height == $tip and
        .result.chaindb_tip_height == $tip and
        .result.forest_tip_marker_height == $tip
    ' <<<"${health}" >/dev/null || fail "${label}: ${health}"
}

info "Bootstrapping: mature coinbase + every-5 checkpoints"
start_node "${DATA_DIR}/node_boot.log"
wait_rpc || fail "bootstrap daemon did not reach RPC readiness"
MINER_ADDR="$(get_field "$(rpc wallet.getnewaddress '["taproot","torture-miner"]')" '.result.address // .result // empty')"
[[ -n "${MINER_ADDR}" ]] || fail "no miner address"
SPEND_ADDR="$(get_field "$(rpc wallet.getnewaddress '["taproot","torture-spend"]')" '.result.address // .result // empty')"
[[ -n "${SPEND_ADDR}" ]] || fail "no spend address"
mine_to 101 "${MINER_ADDR}"
TIP=101
assert_aligned_at "${TIP}" "bootstrap state"

for ((i = 1; i <= CYCLES; i++)); do
    # Submit a real spend so the next block's delta carries deletions —
    # replay after the kill must reproduce removals, not just adds.
    SEND="$(rpc wallet.sendtoaddress "[\"${SPEND_ADDR}\", 1.0]")"
    if ! jq -e '.error == null and .result != null' <<<"${SEND}" >/dev/null; then
        info "cycle ${i}: sendtoaddress unavailable this cycle (${SEND:0:120}) — mining without spend"
    fi

    BLOCKS=$((1 + RANDOM % 3))
    mine_to "${BLOCKS}" "${MINER_ADDR}"
    TIP=$((TIP + BLOCKS))
    assert_aligned_at "${TIP}" "cycle ${i} pre-kill state"

    if ((i % 2 == 0)); then
        SIG="TERM"
    else
        SIG="KILL"
    fi
    info "cycle ${i}: tip=${TIP}, killing daemon with SIG${SIG}"
    kill "-${SIG}" "${PID}" || fail "cycle ${i}: kill failed"
    wait_dead "${PID}" || fail "cycle ${i}: daemon did not die"
    PID=""

    start_node "${DATA_DIR}/node_cycle${i}.log"
    wait_rpc || fail "cycle ${i}: daemon did not restart"
    assert_aligned_at "${TIP}" "cycle ${i} post-restart state"
    if ((TIP % 5 != 0)); then
        grep -q "\[ForestDeltaReplay\] forest restored to tip height ${TIP}" \
            "${DATA_DIR}/node_cycle${i}.log" \
            || fail "cycle ${i}: off-interval restart did not use delta replay"
    fi
    pass "cycle ${i}: SIG${SIG} restart recovered aligned at tip ${TIP}"
done

info "Final: node must still advance after ${CYCLES} kill cycles"
mine_to 1 "${MINER_ADDR}"
TIP=$((TIP + 1))
assert_aligned_at "${TIP}" "final advance"
pass "Survived ${CYCLES} mixed-signal kill cycles with delta-replay restores"

rpc stop '[]' >/dev/null 2>&1 || true
exit 0
