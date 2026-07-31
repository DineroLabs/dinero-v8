#!/usr/bin/env bash
# Regression test for #462 — a valid-but-disconnected fork block must keep its
# validation status across a restart.
#
# The startup block-index rebuild walks the CANONICAL HEIGHT INDEX:
#
#     for (uint32_t h = 0; h <= height; ++h)
#         chain_db_->getBlockHashByHeight(h)
#
# which maps a height to the ACTIVE chain's block at that height. Blocks on a
# losing fork are therefore never re-added to the in-memory index. getblockheader
# emits status only when FindBlockIndex() hits:
#
#     if (auto* block_index = chainstate->FindBlockIndex(hash)) {
#         result["status_flags"] = ...;
#         result["failed_valid"] = ...;
#         result["failed_child"] = ...;
#     }
#
# so after a restart those three fields vanish for fork blocks — not null, absent
# entirely. The header itself survives (getblock works, chainwork is intact); it
# is only the validation state that is lost.
#
# Why that matters: after restart the node cannot tell that a fork block was ever
# validated, nor whether it was marked invalid. If the fork later wins a deeper
# reorg, #453 established that the node does not re-validate — so a durable
# record is required. It also makes "valid block that lost the fork race"
# indistinguishable from "block marked invalid", the exact distinction #453
# established as two separate facts.
#
# Building the competing branch requires the #458 extranonce fix; before that,
# mining after invalidateblock reproduced the invalidated block and never
# activated.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_fork_status_$$"
LOG_A="${DATA_DIR}.a.log"
LOG_B="${DATA_DIR}.b.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

BLOCK_VALID_SCRIPTS=16
SEED_BLOCKS=16
FORK_HEIGHT=12
BRANCH_B_BLOCKS=9

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for L in "${LOG_A}" "${LOG_B}"; do
        [[ -f "$L" ]] && { printf -- '--- %s tail ---\n' "$L" >&2; tail -40 "$L" >&2 || true; }
    done
    exit 1
}

cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    [[ "${KEEP_ON_FAIL}" != "1" ]] && rm -rf "${DATA_DIR}" "${LOG_A}" "${LOG_B}"
    return 0
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

RPC_PORT=$((50000 + RANDOM % 500))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

rpc_call() {
    local cookie
    cookie="$(tr -d '\n' < "${DATA_DIR}/.cookie" 2>/dev/null || tr -d '\n' < "${DATA_DIR}/regtest/.cookie" 2>/dev/null)"
    curl -s --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

# Structural. Also catches the nested-error shape from older daemons.
rpc_failed() {
    local r="$1"
    [[ -z "${r}" ]] && return 0
    jq -e '.error != null' >/dev/null 2>&1 <<<"${r}" && return 0
    jq -e '(.result | type) == "object" and (.result.error != null)' >/dev/null 2>&1 <<<"${r}" && return 0
    return 1
}

start_node() {
    "${DINEROD}" --regtest --datadir="${DATA_DIR}" --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 >"$1" 2>&1 &
    PID=$!
    for _ in $(seq 1 90); do
        rpc_call getblockcount '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc_call stop '[]' >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do
        kill -0 "${PID}" 2>/dev/null || { wait "${PID}" 2>/dev/null || true; PID=""; return 0; }
        sleep 1
    done
    fail "daemon did not exit cleanly"
}

block_count() { jq -r '.result' <<<"$(rpc_call getblockcount '[]')"; }

hash_at_height() {
    local r v
    r="$(rpc_call getblockhash "[$1]")"
    rpc_failed "${r}" && return 1
    v="$(jq -r 'if (.result | type) == "string" then .result else empty end' <<<"${r}")"
    [[ "${v}" =~ ^[0-9a-fA-F]{64}$ ]] || return 1
    printf '%s\n' "${v}"
}

# Emits the raw header object so callers can test for FIELD PRESENCE, which is
# the actual failure mode — the fields go missing entirely, they do not go null.
header_json() {
    local r
    r="$(rpc_call getblockheader "[\"$1\"]")"
    rpc_failed "${r}" && fail "getblockheader failed for $1: ${r}"
    jq -c '.result' <<<"${r}"
}

mkdir -p "${DATA_DIR}"
info "Starting daemon"
start_node "${LOG_A}" || fail "daemon did not reach RPC readiness"

ADDR="$(rpc_call wallet.getnewaddress '[]')"
rpc_failed "${ADDR}" && fail "wallet.getnewaddress failed: ${ADDR}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty mining address"

info "Mining branch A (${SEED_BLOCKS} blocks)"
MINE="$(rpc_call generatetoaddress "[${SEED_BLOCKS},\"${MINER_ADDR}\"]")"
rpc_failed "${MINE}" && fail "seed mine failed: ${MINE}"

TARGET_HASH="$(hash_at_height "${FORK_HEIGHT}")" || fail "getblockhash at ${FORK_HEIGHT} failed"
info "target@${FORK_HEIGHT} = ${TARGET_HASH}"

CONNECTED_JSON="$(header_json "${TARGET_HASH}")"
CONNECTED_FLAGS="$(jq -r '.status_flags // empty' <<<"${CONNECTED_JSON}")"
[[ -n "${CONNECTED_FLAGS}" ]] || fail "precondition: connected block has no status_flags: ${CONNECTED_JSON}"
(( (CONNECTED_FLAGS & BLOCK_VALID_SCRIPTS) != 0 )) \
    || fail "precondition: connected block lacks BLOCK_VALID_SCRIPTS (${CONNECTED_FLAGS})"
info "status_flags while connected: ${CONNECTED_FLAGS}"

# ---- displace it with a higher-work competing branch ------------------------
info "Invalidating height ${FORK_HEIGHT} to roll back and fork"
INV="$(rpc_call invalidateblock "[\"${TARGET_HASH}\"]")"
rpc_failed "${INV}" && fail "invalidateblock failed: ${INV}"

info "Mining competing branch B (${BRANCH_B_BLOCKS} blocks)"
MINE_B="$(rpc_call generatetoaddress "[${BRANCH_B_BLOCKS},\"${MINER_ADDR}\"]")"
rpc_failed "${MINE_B}" && fail "branch B mine failed (needs the #458 extranonce fix): ${MINE_B}"

info "Reconsidering the target so it is valid-but-outweighed"
REC="$(rpc_call reconsiderblock "[\"${TARGET_HASH}\"]")"
rpc_failed "${REC}" && fail "reconsiderblock failed: ${REC}"

ACTIVE_AT_FORK="$(hash_at_height "${FORK_HEIGHT}")" || fail "getblockhash after reorg failed"
[[ "${ACTIVE_AT_FORK}" != "${TARGET_HASH}" ]] \
    || fail "competing branch did not displace the target; premise not met"
info "active height after reorg: $(block_count), hash@${FORK_HEIGHT} is now ${ACTIVE_AT_FORK}"
pass "competing branch displaced the target block"

DISCONNECTED_JSON="$(header_json "${TARGET_HASH}")"
DISCONNECTED_FLAGS="$(jq -r '.status_flags // empty' <<<"${DISCONNECTED_JSON}")"
[[ -n "${DISCONNECTED_FLAGS}" ]] \
    || fail "disconnected block has no status_flags before restart: ${DISCONNECTED_JSON}"
(( (DISCONNECTED_FLAGS & BLOCK_VALID_SCRIPTS) != 0 )) \
    || fail "disconnected block lost BLOCK_VALID_SCRIPTS before restart (${DISCONNECTED_FLAGS})"
info "status_flags while disconnected: ${DISCONNECTED_FLAGS}"
pass "disconnected block retains BLOCK_VALID_SCRIPTS before restart"

# ---- the regression --------------------------------------------------------
info "Restarting the daemon"
stop_node
start_node "${LOG_B}" || fail "daemon did not reach RPC readiness after restart"

RESTART_JSON="$(header_json "${TARGET_HASH}")"
info "header after restart: $(echo "${RESTART_JSON}" | cut -c1-160)"

# The block itself must survive.
[[ "$(jq -r '.hash // empty' <<<"${RESTART_JSON}")" == "${TARGET_HASH}" ]] \
    || fail "fork block header not retrievable after restart: ${RESTART_JSON}"

# Field PRESENCE is the assertion. They go missing entirely, not null.
jq -e 'has("status_flags")' >/dev/null 2>&1 <<<"${RESTART_JSON}" \
    || fail "fork block lost status_flags across restart — the field is ABSENT, not null. The startup rebuild walks the canonical height index (getBlockHashByHeight), so blocks on a losing fork are never re-added to the in-memory index and FindBlockIndex misses them. Header after restart: ${RESTART_JSON}"
jq -e 'has("failed_valid") and has("failed_child")' >/dev/null 2>&1 <<<"${RESTART_JSON}" \
    || fail "fork block lost failed_valid/failed_child across restart: ${RESTART_JSON}"
pass "fork block still reports status fields after restart"

RESTART_FLAGS="$(jq -r '.status_flags' <<<"${RESTART_JSON}")"
info "status_flags after restart: ${RESTART_FLAGS}"

(( (RESTART_FLAGS & BLOCK_VALID_SCRIPTS) != 0 )) \
    || fail "fork block lost BLOCK_VALID_SCRIPTS across restart: ${DISCONNECTED_FLAGS} -> ${RESTART_FLAGS}"
pass "fork block retains BLOCK_VALID_SCRIPTS across restart"

[[ "${DISCONNECTED_FLAGS}" == "${RESTART_FLAGS}" ]] \
    || fail "fork block status_flags changed across restart: ${DISCONNECTED_FLAGS} -> ${RESTART_FLAGS}"
pass "fork block status_flags identical across restart (${RESTART_FLAGS})"

# A valid block that merely lost the fork race must not read as invalid.
jq -e '.failed_valid == false and .failed_child == false' >/dev/null 2>&1 <<<"${RESTART_JSON}" \
    || fail "valid-but-outweighed fork block reports failure flags after restart: ${RESTART_JSON}"
pass "fork block is not marked invalid — 'lost the fork race' stays distinct from 'invalid'"

# The active chain must be unaffected by restoring fork blocks.
ACTIVE_AFTER="$(hash_at_height "${FORK_HEIGHT}")" || fail "getblockhash after restart failed"
[[ "${ACTIVE_AFTER}" == "${ACTIVE_AT_FORK}" ]] \
    || fail "active chain changed across restart: ${ACTIVE_AT_FORK} -> ${ACTIVE_AFTER}. Restoring fork blocks to the index must not alter fork choice."
pass "active chain unchanged across restart"

stop_node
pass "#462 fork-block status durability test completed"
