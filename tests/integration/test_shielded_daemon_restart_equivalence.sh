#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ -n "${DINEROD:-}" ]]; then
    # CTest supplies this (ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"), so the
    # test follows the build directory wherever it is. Honour it and require it
    # to be real — never silently fall through to a guessed path.
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 1; }
elif [[ -x "${ROOT_DIR}/build/dinerod" ]]; then
    # Manual/local convenience only.
    DINEROD="${ROOT_DIR}/build/dinerod"
elif [[ -x "${ROOT_DIR}/dinerod" ]]; then
    DINEROD="${ROOT_DIR}/dinerod"
else
    # Fail HERE, naming the paths tried. Launching a non-existent binary and
    # then waiting on its RPC turns a missing file into a 30s timeout reported
    # as "RPC never came up", which reads like a consensus failure.
    echo "dinerod not found (tried: \$DINEROD unset, ${ROOT_DIR}/build/dinerod, ${ROOT_DIR}/dinerod)" >&2
    echo "set DINEROD=/path/to/dinerod to override" >&2
    exit 1
fi
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

DATA_DIR="/tmp/dinero_shielded_daemon_restart_equiv_$$"
LOG_BASE="${DATA_DIR}.base.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_RESTART="${DATA_DIR}.restart.log"
LOG_FINAL="${DATA_DIR}.final.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -120 "${LOG_BASE}" >&2 || true; }
    [[ -f "${LOG_CRASH}" ]] && { printf -- '--- crash log tail ---\n' >&2; tail -120 "${LOG_CRASH}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -120 "${LOG_RESTART}" >&2 || true; }
    [[ -f "${LOG_FINAL}" ]] && { printf -- '--- final log tail ---\n' >&2; tail -120 "${LOG_FINAL}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_BASE}" "${LOG_CRASH}" "${LOG_RESTART}" "${LOG_FINAL}"
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

sha256_hex_text() {
    python3 - "$1" <<'PY'
import hashlib
import sys
print(hashlib.sha256(sys.argv[1].encode("utf-8")).hexdigest())
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

RPC_PORT=$((38000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
SHIELD_VALUE_UNA=500000000
TX_FEE_UNA=50000
NOTE1_SEED=16
NOTE2_SEED=64
SHIELDED_CRASH_HOOK="${SHIELDED_CRASH_HOOK:-after_undo_before_tip}"
SHIELDED_EXPECT_COMMITTED="${SHIELDED_EXPECT_COMMITTED:-0}"

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
assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "pre-crash shielded steady state"

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

start_node "${LOG_CRASH}" DINERO_CRASH_AT="${SHIELDED_CRASH_HOOK}"
wait_rpc || fail "crash daemon did not reach RPC readiness"
assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "pre-trigger restart state"

TRANSFER_TXID="$(send_raw_transaction_or_fail "${TRANSFER_TX_HEX}" "pre-crash transfer sendrawtransaction failed")"
[[ -n "${TRANSFER_TXID}" ]] || fail "sendrawtransaction returned empty transfer txid"
assert_mempool_contains "${TRANSFER_TXID}" "pre-crash transfer tx not in mempool"

info "Triggering shielded crash at ${SHIELDED_CRASH_HOOK}"
set +e
CRASH_TRIGGER_RESULT="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[1,\"${MINER_ADDR}\"]" 2>/dev/null)"
set -e
if [[ -n "${CRASH_TRIGGER_RESULT}" ]] && rpc_has_error "${CRASH_TRIGGER_RESULT}"; then
    fail "generatetoaddress failed before crash trigger: ${CRASH_TRIGGER_RESULT}"
fi
wait_dead "${PID}" || fail "daemon did not crash at ${SHIELDED_CRASH_HOOK}"
PID=""
grep -q "DINERO_CRASH" "${LOG_CRASH}" || fail "crash log did not show named crash hook"
pass "Crash hook triggered on a live shielded block connect"

start_node "${LOG_RESTART}"
wait_rpc || fail "restarted daemon did not reach RPC readiness"
if [[ "${SHIELDED_EXPECT_COMMITTED}" == "1" ]]; then
    COMMITTED_HEIGHT=$((BASE_HEIGHT + 1))
    COMMITTED_HASH="$(json_get "$(rpc_result "getblockhash" "[${COMMITTED_HEIGHT}]")" '.result')"
    assert_sync_health_matches_tip "${COMMITTED_HEIGHT}" "${COMMITTED_HASH}" 2 1 "post-crash committed restart state"
    pass "Restart preserved the tip-persisted canonical shielded state"
else
    assert_sync_health_matches_tip "${BASE_HEIGHT}" "${BASE_HASH}" 1 0 "post-crash restart state"
    pass "Restart held the pre-crash canonical shielded state"
fi

stop_node

if [[ "${SHIELDED_EXPECT_COMMITTED}" == "1" ]]; then
    POST_CRASH_PROBE_JSON="$(probe_state "$((BASE_HEIGHT + 1))")"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.current_tree_size // 0')" == "2" ]] || \
        fail "committed restart tree size mismatch: ${POST_CRASH_PROBE_JSON}"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.current_nullifier_count // 0')" == "1" ]] || \
        fail "committed restart nullifier count mismatch: ${POST_CRASH_PROBE_JSON}"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.current_root // empty')" == "${EXPECTED_TRANSFER_TREE_ROOT_DISPLAY}" ]] || \
        fail "committed restart frontier root mismatch: ${POST_CRASH_PROBE_JSON}"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.current_nullifier_dump_sha256 // empty')" == "${EXPECTED_TRANSFER_NULLIFIER_SHA}" ]] || \
        fail "committed restart nullifier dump mismatch: ${POST_CRASH_PROBE_JSON}"
    pass "Probe-level shielded state matched the committed shielded block"
else
    POST_CRASH_PROBE_JSON="$(probe_state "${BASE_HEIGHT}")"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.current_frontier_sha256 // empty')" == "${BASE_FRONTIER_SHA}" ]] || \
        fail "frontier hash changed across crash/restart: ${POST_CRASH_PROBE_JSON}"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.current_nullifier_dump_sha256 // empty')" == "${BASE_NULLIFIER_SHA}" ]] || \
        fail "nullifier dump hash changed across crash/restart: ${POST_CRASH_PROBE_JSON}"
    [[ "$(json_get "${POST_CRASH_PROBE_JSON}" '.chaindb_tip_hash // empty')" == "${BASE_HASH}" ]] || \
        fail "probe tip hash drifted across crash/restart: ${POST_CRASH_PROBE_JSON}"
    pass "Probe-level shielded state matched the pre-crash baseline"
fi

start_node "${LOG_FINAL}"
wait_rpc || fail "final daemon did not reach RPC readiness"
if [[ "${SHIELDED_EXPECT_COMMITTED}" == "1" ]]; then
    mine_blocks 1 "${MINER_ADDR}"

    FINAL_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
    FINAL_HASH="$(json_get "$(rpc_result "getblockhash" "[${FINAL_HEIGHT}]")" '.result')"
    [[ "${FINAL_HEIGHT}" == "$((BASE_HEIGHT + 2))" ]] || fail "final height did not advance by one from committed tip"
    assert_sync_health_matches_tip "${FINAL_HEIGHT}" "${FINAL_HASH}" 2 1 "post-committed-restart advance"

    stop_node

    FINAL_PROBE_JSON="$(probe_state "${FINAL_HEIGHT}")"
    [[ "$(json_get "${FINAL_PROBE_JSON}" '.current_tree_size // 0')" == "2" ]] || fail "final committed tree size mismatch: ${FINAL_PROBE_JSON}"
    [[ "$(json_get "${FINAL_PROBE_JSON}" '.current_nullifier_count // 0')" == "1" ]] || fail "final committed nullifier count mismatch: ${FINAL_PROBE_JSON}"
    [[ "$(json_get "${FINAL_PROBE_JSON}" '.current_root // empty')" == "${EXPECTED_TRANSFER_TREE_ROOT_DISPLAY}" ]] || \
        fail "final committed frontier root mismatch: ${FINAL_PROBE_JSON}"
    pass "Recovered daemon kept the committed shielded spend and advanced one more block"
else
    TRANSFER_TXID_FINAL="$(send_raw_transaction_or_fail "${TRANSFER_TX_HEX}" "post-restart transfer sendrawtransaction failed")"
    [[ -n "${TRANSFER_TXID_FINAL}" ]] || fail "final sendrawtransaction returned empty transfer txid"
    assert_mempool_contains "${TRANSFER_TXID_FINAL}" "post-restart transfer tx not in mempool"
    mine_blocks 1 "${MINER_ADDR}"

    FINAL_HEIGHT="$(json_get "$(rpc_result "getblockcount" '[]')" '.result')"
    FINAL_HASH="$(json_get "$(rpc_result "getblockhash" "[${FINAL_HEIGHT}]")" '.result')"
    [[ "${FINAL_HEIGHT}" == "$((BASE_HEIGHT + 1))" ]] || fail "final height did not advance by one"
    assert_sync_health_matches_tip "${FINAL_HEIGHT}" "${FINAL_HASH}" 2 1 "post-recovery shielded advance"

    stop_node

    FINAL_PROBE_JSON="$(probe_state "${FINAL_HEIGHT}")"
    [[ "$(json_get "${FINAL_PROBE_JSON}" '.current_tree_size // 0')" == "2" ]] || fail "final tree size mismatch: ${FINAL_PROBE_JSON}"
    [[ "$(json_get "${FINAL_PROBE_JSON}" '.current_nullifier_count // 0')" == "1" ]] || fail "final nullifier count mismatch: ${FINAL_PROBE_JSON}"
    pass "Recovered daemon accepted the retried shielded spend and advanced canonical state"
fi
