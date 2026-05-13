#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"
if [[ -x "${ROOT_DIR}/build/tests/integration/shielded_tx_builder" ]]; then
    SHIELDED_TX_BUILDER="${ROOT_DIR}/build/tests/integration/shielded_tx_builder"
elif [[ -x "${ROOT_DIR}/build/shielded_tx_builder" ]]; then
    SHIELDED_TX_BUILDER="${ROOT_DIR}/build/shielded_tx_builder"
else
    SHIELDED_TX_BUILDER="${ROOT_DIR}/shielded_tx_builder"
fi
if [[ -x "${ROOT_DIR}/build/tests/integration/shielded_tip_marker_probe" ]]; then
    SHIELDED_PROBE="${ROOT_DIR}/build/tests/integration/shielded_tip_marker_probe"
elif [[ -x "${ROOT_DIR}/build/shielded_tip_marker_probe" ]]; then
    SHIELDED_PROBE="${ROOT_DIR}/build/shielded_tip_marker_probe"
else
    SHIELDED_PROBE="${ROOT_DIR}/shielded_tip_marker_probe"
fi

TMP_ROOT="$(mktemp -d /tmp/dinero_shielded_reorg_disconnect_restart_equiv.XXXXXX)"
DATA_DIR="${TMP_ROOT}/datadir"
LOG_BASE="${TMP_ROOT}/base.log"
LOG_COMMITTED="${TMP_ROOT}/committed.log"
LOG_CRASH="${TMP_ROOT}/crash.log"
LOG_RESTART="${TMP_ROOT}/restart.log"
LOG_SECOND_RESTART="${TMP_ROOT}/second_restart.log"
LOG_FINAL="${TMP_ROOT}/final.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -120 "${LOG_BASE}" >&2 || true; }
    [[ -f "${LOG_COMMITTED}" ]] && { printf -- '--- committed log tail ---\n' >&2; tail -120 "${LOG_COMMITTED}" >&2 || true; }
    [[ -f "${LOG_CRASH}" ]] && { printf -- '--- crash log tail ---\n' >&2; tail -120 "${LOG_CRASH}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -120 "${LOG_RESTART}" >&2 || true; }
    [[ -f "${LOG_SECOND_RESTART}" ]] && { printf -- '--- second restart log tail ---\n' >&2; tail -120 "${LOG_SECOND_RESTART}" >&2 || true; }
    [[ -f "${LOG_FINAL}" ]] && { printf -- '--- final log tail ---\n' >&2; tail -120 "${LOG_FINAL}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${TMP_ROOT}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v python3 >/dev/null || fail "python3 is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
    [[ -x "${SHIELDED_TX_BUILDER}" ]] || fail "shielded_tx_builder missing at ${SHIELDED_TX_BUILDER}"
    [[ -x "${SHIELDED_PROBE}" ]] || fail "shielded_tip_marker_probe missing at ${SHIELDED_PROBE}"
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
        return 0
    fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
        return 0
    fi
    return 1
}

rpc_call() {
    local datadir="$1"
    local method="$2"
    local params_json="$3"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_has_error() {
    local compact
    compact="$(echo "$1" | tr -d '\n\t ')"
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if [[ -n "${PID}" ]] && ! kill -0 "${PID}" 2>/dev/null; then
            return 1
        fi
        if rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 60); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    env "$@" "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "${DATA_DIR}" "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${stop_result}" ]] && rpc_has_error "${stop_result}"; then
        kill "${PID}" 2>/dev/null || true
    fi
    wait_dead "${PID}" || kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

json_get() {
    local json="$1"
    local filter="$2"
    jq -r "${filter}" <<<"${json}"
}

get_block_header() {
    local hash="$1"
    local result
    result="$(rpc_call "${DATA_DIR}" "getblockheader" "[\"${hash}\"]")"
    rpc_has_error "${result}" && fail "getblockheader(${hash}) failed: ${result}"
    jq -c '.' <<<"${result}"
}

assert_header_flags() {
    local hash="$1"
    local expected_failed_valid="$2"
    local expected_failed_child="$3"
    local label="$4"
    local header
    header="$(get_block_header "${hash}")"
    jq -e \
        --argjson expected_failed_valid "${expected_failed_valid}" \
        --argjson expected_failed_child "${expected_failed_child}" \
        '
        .error == null and
        .result.failed_valid == $expected_failed_valid and
        .result.failed_child == $expected_failed_child and
        (.result.status_flags | type) == "number"
        ' <<<"${header}" >/dev/null || fail "${label} flags mismatch: ${header}"
}

rpc_result() {
    local method="$1"
    local params_json="$2"
    local result
    result="$(rpc_call "${DATA_DIR}" "${method}" "${params_json}")"
    rpc_has_error "${result}" && fail "${method} failed: ${result}"
    printf '%s\n' "${result}"
}

mine_blocks() {
    local blocks="$1"
    local address="$2"
    rpc_result "generatetoaddress" "[${blocks},\"${address}\"]" >/dev/null
}

assert_mempool_contains() {
    local txid="$1"
    local label="$2"
    local mempool
    mempool="$(rpc_result "getrawmempool" '[]')"
    jq -e --arg txid "${txid}" '.result | index($txid) != null' <<<"${mempool}" >/dev/null || \
        fail "${label}: tx ${txid} missing from mempool: ${mempool}"
}

send_raw_transaction_or_fail() {
    local tx_hex="$1"
    local label="$2"
    local response
    response="$(rpc_result "sendrawtransaction" "[\"${tx_hex}\"]")"
    jq -e '(.result | type == "string") or (.result.result | type == "string")' <<<"${response}" >/dev/null || \
        fail "${label}: ${response}"
    json_get "${response}" '.result.result // .result // empty'
}

din_from_una() {
    python3 - "$1" <<'PY'
import sys
value = int(sys.argv[1])
print(f"{value / 100000000:.8f}")
PY
}

sha256_canonical_nullifier_row() {
    python3 - "$1" "$2" <<'PY'
import hashlib
import sys
row = f"{sys.argv[1].upper()}:{int(sys.argv[2])}\n"
print(hashlib.sha256(row.encode("utf-8")).hexdigest())
PY
}

reverse_hex_bytes() {
    python3 - "$1" <<'PY'
import sys

hex_str = sys.argv[1].strip()
if len(hex_str) % 2 != 0:
    raise SystemExit("expected even-length hex string")
print(''.join(reversed([hex_str[i:i+2] for i in range(0, len(hex_str), 2)])))
PY
}

probe_state() {
    local verify_height="$1"
    "${SHIELDED_PROBE}" --datadir "${DATA_DIR}" --verify-tip-height "${verify_height}" | tail -n 1
}

assert_sync_health_matches_tip() {
    local expected_height="$1"
    local expected_hash="$2"
    local expected_tree_size="$3"
    local expected_nullifier_count="$4"
    local label="$5"
    local health
    health="$(rpc_result "blockchain.getsynchealth" '[]')"
    jq -e \
        --arg expected_hash "${expected_hash}" \
        --argjson expected_height "${expected_height}" \
        --argjson expected_tree_size "${expected_tree_size}" \
        --argjson expected_nullifier_count "${expected_nullifier_count}" \
        '
        .result.canonical_state_aligned == true and
        .result.active_height == $expected_height and
        .result.chaindb_tip_height == $expected_height and
        .result.active_best_hash == $expected_hash and
        .result.chaindb_tip_hash == $expected_hash and
        .result.shielded_tip_marker_found == true and
        .result.shielded_tip_marker_height == $expected_height and
        .result.shielded_tip_marker_hash == $expected_hash and
        .result.shielded_tree_size == $expected_tree_size and
        .result.shielded_nullifier_count == $expected_nullifier_count and
        .result.shielded_tip_marker_tree_size == $expected_tree_size and
        .result.shielded_tip_marker_nullifier_count == $expected_nullifier_count and
        .result.shielded_frontier_root == .result.shielded_tip_marker_root
        ' <<<"${health}" >/dev/null || fail "${label}: ${health}"
}

require_tools

RPC_PORT=$((39000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
SHIELD_VALUE_UNA=500000000
TX_FEE_UNA=50000
NOTE1_SEED=16
NOTE2_SEED=64
SHIELDED_SECOND_RESTART_INVALIDITY="${SHIELDED_SECOND_RESTART_INVALIDITY:-0}"

start_node "${LOG_BASE}"
wait_rpc || fail "base daemon did not reach RPC readiness"

MINER_ADDR_RESULT="$(rpc_result "wallet.getnewaddress" '["taproot","phase2-miner"]')"
MINER_ADDR="$(json_get "${MINER_ADDR_RESULT}" '.result.address // .result // empty')"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

CHANGE_ADDR_RESULT="$(rpc_result "wallet.getnewaddress" '["taproot","phase2-change"]')"
CHANGE_ADDR="$(json_get "${CHANGE_ADDR_RESULT}" '.result.address // .result // empty')"
[[ -n "${CHANGE_ADDR}" ]] || fail "wallet.getnewaddress returned empty change address"

info "Mining spendable regtest coinbase"
mine_blocks 101 "${MINER_ADDR}"

UTXO_JSON="$(rpc_result "wallet.listunspent" '[101,9999999]' | jq -c '.result | map(select(.spendable == true and .witness_version == 1))[0] // empty')"
[[ -n "${UTXO_JSON}" ]] || fail "no mature taproot UTXO available"
UTXO_TXID="$(json_get "${UTXO_JSON}" '.txid // empty')"
UTXO_VOUT="$(json_get "${UTXO_JSON}" '.vout // -1')"
UTXO_SCRIPT="$(json_get "${UTXO_JSON}" '.scriptPubKey // empty')"
UTXO_AMOUNT="$(json_get "${UTXO_JSON}" '.amount // empty')"
UTXO_AMOUNT_UNA="$(json_get "${UTXO_JSON}" '.amount_una // 0')"
[[ "${UTXO_AMOUNT_UNA}" =~ ^[0-9]+$ ]] || fail "unexpected non-numeric input amount_una ${UTXO_AMOUNT_UNA}"
(( UTXO_AMOUNT_UNA > SHIELD_VALUE_UNA )) || fail "input amount_una ${UTXO_AMOUNT_UNA} does not cover shield value ${SHIELD_VALUE_UNA}"
(( SHIELD_VALUE_UNA > TX_FEE_UNA )) || fail "shield value ${SHIELD_VALUE_UNA} does not cover tx fee ${TX_FEE_UNA}"

CHANGE_VALUE_UNA="$((UTXO_AMOUNT_UNA - SHIELD_VALUE_UNA - TX_FEE_UNA))"
CHANGE_VALUE_DIN="$(din_from_una "${CHANGE_VALUE_UNA}")"

RAW_TX_RESULT="$(rpc_result "wallet.createrawtransaction" "[[{\"txid\":\"${UTXO_TXID}\",\"vout\":${UTXO_VOUT}}],[{\"${CHANGE_ADDR}\":${CHANGE_VALUE_DIN}}]]")"
RAW_TX_HEX="$(json_get "${RAW_TX_RESULT}" '.result.hex // empty')"
[[ -n "${RAW_TX_HEX}" ]] || fail "wallet.createrawtransaction returned empty hex"

SHIELD_TX_JSON="$("${SHIELDED_TX_BUILDER}" attach-shield-output \
    --raw-tx "${RAW_TX_HEX}" \
    --shield-value-una "${SHIELD_VALUE_UNA}" \
    --explicit-fee-una "${TX_FEE_UNA}" \
    --note-seed "${NOTE1_SEED}")"
SHIELD_TX_HEX="$(json_get "${SHIELD_TX_JSON}" '.hex // empty')"
[[ -n "${SHIELD_TX_HEX}" ]] || fail "shielded_tx_builder returned empty shield tx hex"

SIGN_PARAMS="[\"${SHIELD_TX_HEX}\",[{\"txid\":\"${UTXO_TXID}\",\"vout\":${UTXO_VOUT},\"scriptPubKey\":\"${UTXO_SCRIPT}\",\"amount\":${UTXO_AMOUNT}}]]"
SIGN_RESULT="$(rpc_result "wallet.signrawtransaction" "${SIGN_PARAMS}")"
[[ "$(json_get "${SIGN_RESULT}" '.result.complete // false')" == "true" ]] || fail "wallet.signrawtransaction incomplete: ${SIGN_RESULT}"
SIGNED_SHIELD_TX_HEX="$(json_get "${SIGN_RESULT}" '.result.hex // empty')"
[[ -n "${SIGNED_SHIELD_TX_HEX}" ]] || fail "wallet.signrawtransaction returned empty shield hex"

SHIELD_TXID="$(send_raw_transaction_or_fail "${SIGNED_SHIELD_TX_HEX}" "shield funding sendrawtransaction failed")"
[[ -n "${SHIELD_TXID}" ]] || fail "sendrawtransaction returned empty shield txid"
assert_mempool_contains "${SHIELD_TXID}" "shield funding tx not in mempool"
mine_blocks 1 "${MINER_ADDR}"

BASE_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
BASE_HASH="$(json_get "$(rpc_result "getblockhash" "[${BASE_HEIGHT}]")" '.result')"
assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "pre-transfer shielded steady state"

stop_node

BASE_PROBE_JSON="$(probe_state "${BASE_HEIGHT}")"
BASE_FRONTIER_SHA="$(json_get "${BASE_PROBE_JSON}" '.current_frontier_sha256 // empty')"
BASE_NULLIFIER_SHA="$(json_get "${BASE_PROBE_JSON}" '.current_nullifier_dump_sha256 // empty')"
[[ "$(json_get "${BASE_PROBE_JSON}" '.current_tree_size // 0')" == "1" ]] || fail "baseline tree size mismatch: ${BASE_PROBE_JSON}"
[[ "$(json_get "${BASE_PROBE_JSON}" '.current_nullifier_count // 0')" == "0" ]] || fail "baseline nullifier count mismatch: ${BASE_PROBE_JSON}"

TRANSFER_TX_JSON="$("${SHIELDED_TX_BUILDER}" build-transfer \
    --input-note-seed "${NOTE1_SEED}" \
    --input-value-una "${SHIELD_VALUE_UNA}" \
    --input-leaf-index 0 \
    --output-note-seed "${NOTE2_SEED}" \
    --output-value-una "$((SHIELD_VALUE_UNA - TX_FEE_UNA))" \
    --explicit-fee-una "${TX_FEE_UNA}")"
TRANSFER_TX_HEX="$(json_get "${TRANSFER_TX_JSON}" '.hex // empty')"
[[ -n "${TRANSFER_TX_HEX}" ]] || fail "shielded_tx_builder returned empty transfer tx hex"
EXPECTED_TRANSFER_TREE_ROOT="$(json_get "${TRANSFER_TX_JSON}" '.expected_tree_root // empty')"
[[ -n "${EXPECTED_TRANSFER_TREE_ROOT}" ]] || fail "shielded_tx_builder returned empty expected tree root"
EXPECTED_TRANSFER_TREE_ROOT_DISPLAY="$(reverse_hex_bytes "${EXPECTED_TRANSFER_TREE_ROOT}")"
TRANSFER_NULLIFIER_HEX="$(json_get "${TRANSFER_TX_JSON}" '.input_nullifier // empty')"
[[ -n "${TRANSFER_NULLIFIER_HEX}" ]] || fail "shielded_tx_builder returned empty input nullifier"
EXPECTED_TRANSFER_NULLIFIER_SHA="$(sha256_canonical_nullifier_row "${TRANSFER_NULLIFIER_HEX}" "$((BASE_HEIGHT + 1))")"

start_node "${LOG_COMMITTED}"
wait_rpc || fail "committed daemon did not reach RPC readiness"
assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "pre-transfer restart state"

TRANSFER_TXID="$(send_raw_transaction_or_fail "${TRANSFER_TX_HEX}" "pre-reorg transfer sendrawtransaction failed")"
[[ -n "${TRANSFER_TXID}" ]] || fail "sendrawtransaction returned empty transfer txid"
assert_mempool_contains "${TRANSFER_TXID}" "pre-reorg transfer tx not in mempool"
mine_blocks 1 "${MINER_ADDR}"

COMMITTED_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
[[ "${COMMITTED_HEIGHT}" == "$((BASE_HEIGHT + 1))" ]] || fail "committed height did not advance by one"
COMMITTED_HASH="$(json_get "$(rpc_result "getblockhash" "[${COMMITTED_HEIGHT}]")" '.result')"
assert_sync_health_matches_tip "${COMMITTED_HEIGHT}" "${COMMITTED_HASH}" 2 1 "committed transfer state"
pass "Committed shielded transfer block established the rollback target"

stop_node

start_node "${LOG_CRASH}" DINERO_CRASH_AT="after_disconnect_tip_before_shielded_flush"
wait_rpc || fail "crash daemon did not reach RPC readiness"
assert_sync_health_matches_tip "${COMMITTED_HEIGHT}" "${COMMITTED_HASH}" 2 1 "pre-invalidate committed state"

info "Triggering shielded rollback crash at after_disconnect_tip_before_shielded_flush"
set +e
CRASH_TRIGGER_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.invalidateblock" "[\"${COMMITTED_HASH}\"]" 2>/dev/null)"
set -e
if [[ -n "${CRASH_TRIGGER_RESULT}" ]] && rpc_has_error "${CRASH_TRIGGER_RESULT}"; then
    fail "invalidateblock failed before crash trigger: ${CRASH_TRIGGER_RESULT}"
fi
wait_dead "${PID}" || fail "daemon did not crash at after_disconnect_tip_before_shielded_flush"
PID=""
grep -q "DINERO_CRASH" "${LOG_CRASH}" || fail "crash log did not show named crash hook"
pass "Crash hook triggered on live shielded rollback/disconnect"

start_node "${LOG_RESTART}"
wait_rpc || fail "restarted daemon did not reach RPC readiness"
POST_RESTART_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
POST_RESTART_HASH="$(json_get "$(rpc_result "getblockhash" "[${POST_RESTART_HEIGHT}]")" '.result')"
if [[ "${POST_RESTART_HEIGHT}" == "${BASE_HEIGHT}" && "${POST_RESTART_HASH}" == "${BASE_HASH}" ]]; then
    assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "post-rollback crash restart state"
    pass "Restart preserved the rolled-back pre-transfer canonical state"
elif [[ "${POST_RESTART_HEIGHT}" == "${COMMITTED_HEIGHT}" && "${POST_RESTART_HASH}" == "${COMMITTED_HASH}" ]]; then
    assert_sync_health_matches_tip "${COMMITTED_HEIGHT}" "${COMMITTED_HASH}" 2 1 "post-rollback crash retry state"
    pass "Restart came back on the committed tip, ready to retry the invalidation cleanly"
else
    fail "unexpected post-crash restart tip height/hash: height=${POST_RESTART_HEIGHT} hash=${POST_RESTART_HASH}"
fi

if [[ "${POST_RESTART_HEIGHT}" == "${COMMITTED_HEIGHT}" ]]; then
    INVALIDATE_RETRY_RESULT="$(rpc_result "blockchain.invalidateblock" "[\"${COMMITTED_HASH}\"]")"
    jq -e '.error == null' <<<"${INVALIDATE_RETRY_RESULT}" >/dev/null || fail "retry invalidateblock failed: ${INVALIDATE_RETRY_RESULT}"
    ROLLBACK_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
    ROLLBACK_HASH="$(json_get "$(rpc_result "getblockhash" "[${ROLLBACK_HEIGHT}]")" '.result')"
    [[ "${ROLLBACK_HEIGHT}" == "${BASE_HEIGHT}" && "${ROLLBACK_HASH}" == "${BASE_HASH}" ]] || \
        fail "retry invalidateblock did not restore pre-transfer tip"
    assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "post-retry rollback state"
    pass "Retried invalidation converged back to the pre-transfer canonical state"
fi

assert_header_flags "${COMMITTED_HASH}" true false "post-retry invalidated committed block"
pass "Committed shielded block is flagged invalid after rollback retry"

if [[ "${SHIELDED_SECOND_RESTART_INVALIDITY}" == "1" ]]; then
    stop_node

    start_node "${LOG_SECOND_RESTART}"
    wait_rpc || fail "second restart daemon did not reach RPC readiness"
    assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "post-second-restart rollback state"
    assert_header_flags "${COMMITTED_HASH}" true false "post-second-restart invalidated committed block"
    pass "Second clean restart kept the invalidated shielded block dead"
fi

FINAL_MEMPOOL_JSON="$(rpc_result "getrawmempool" '[]')"
if jq -e --arg txid "${TRANSFER_TXID}" '.result | index($txid) != null' <<<"${FINAL_MEMPOOL_JSON}" >/dev/null; then
    TRANSFER_TXID_FINAL="${TRANSFER_TXID}"
    pass "Rollback retry restored the disconnected shielded spend to mempool"
else
    TRANSFER_TXID_FINAL="$(send_raw_transaction_or_fail "${TRANSFER_TX_HEX}" "post-restart transfer sendrawtransaction failed")"
    [[ -n "${TRANSFER_TXID_FINAL}" ]] || fail "final sendrawtransaction returned empty transfer txid"
fi
assert_mempool_contains "${TRANSFER_TXID_FINAL}" "post-restart transfer tx not in mempool"
mine_blocks 1 "${MINER_ADDR}"

FINAL_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
FINAL_HASH="$(json_get "$(rpc_result "getblockhash" "[${FINAL_HEIGHT}]")" '.result')"
[[ "${FINAL_HEIGHT}" == "$((BASE_HEIGHT + 1))" ]] || fail "final height did not advance by one"
assert_sync_health_matches_tip "${FINAL_HEIGHT}" "${FINAL_HASH}" 2 1 "post-rollback-recovery shielded advance"

stop_node

FINAL_PROBE_JSON="$(probe_state "${FINAL_HEIGHT}")"
[[ "$(json_get "${FINAL_PROBE_JSON}" '.current_tree_size // 0')" == "2" ]] || fail "final tree size mismatch: ${FINAL_PROBE_JSON}"
[[ "$(json_get "${FINAL_PROBE_JSON}" '.current_nullifier_count // 0')" == "1" ]] || fail "final nullifier count mismatch: ${FINAL_PROBE_JSON}"
[[ "$(json_get "${FINAL_PROBE_JSON}" '.current_root // empty')" == "${EXPECTED_TRANSFER_TREE_ROOT_DISPLAY}" ]] || \
    fail "final frontier root mismatch: ${FINAL_PROBE_JSON}"
[[ "$(json_get "${FINAL_PROBE_JSON}" '.current_nullifier_dump_sha256 // empty')" == "${EXPECTED_TRANSFER_NULLIFIER_SHA}" ]] || \
    fail "final nullifier dump mismatch: ${FINAL_PROBE_JSON}"
pass "Recovered daemon re-mined the shielded spend after rollback crash and converged canonically"
