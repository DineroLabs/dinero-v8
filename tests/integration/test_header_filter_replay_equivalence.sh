#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${ROOT_DIR}/build/dinerod"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
BASE_DIR="/tmp/dinero_header_filter_live_$$"
REPLAY_DIR="/tmp/dinero_header_filter_replay_$$"
LOG_BASE="${BASE_DIR}.log"
LOG_REPLAY="${REPLAY_DIR}.log"
STATE_LIVE_FILE="${BASE_DIR}.state.json"
STATE_REPLAY_FILE="${REPLAY_DIR}.state.json"
PID=""
KEEP_ON_FAIL=0
CURRENT_DATADIR=""
CURRENT_RPC_PORT=""
CURRENT_P2P_PORT=""
CURRENT_WALLET_PORT=""

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -120 "${LOG_BASE}" >&2 || true; }
    [[ -f "${LOG_REPLAY}" ]] && { printf -- '--- replay log tail ---\n' >&2; tail -160 "${LOG_REPLAY}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${BASE_DIR}" 2>/dev/null || true
    pkill -f "dinerod.*${REPLAY_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${BASE_DIR}" "${REPLAY_DIR}" "${LOG_BASE}" "${LOG_REPLAY}"
        rm -f "${STATE_LIVE_FILE}" "${STATE_REPLAY_FILE}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
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
    local method="$1"
    local params_json="$2"
    local cookie_path
    cookie_path="$(cookie_file "${CURRENT_DATADIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${CURRENT_RPC_PORT}/"
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
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
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
    local datadir="$1"
    local log_file="$2"
    local rpc_port="$3"
    local p2p_port="$4"
    local wallet_port="$5"
    shift 5

    mkdir -p "${datadir}"
    CURRENT_DATADIR="${datadir}"
    CURRENT_RPC_PORT="${rpc_port}"
    CURRENT_P2P_PORT="${p2p_port}"
    CURRENT_WALLET_PORT="${wallet_port}"

    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --wallet-socket-port="${wallet_port}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        "$@" \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${stop_result}" ]] && rpc_has_error "${stop_result}"; then
        kill "${PID}" 2>/dev/null || true
    fi
    wait_dead "${PID}" || kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

rpc_address() {
    local response="$1"
    jq -r '.result.address // .result // empty' <<<"${response}"
}

mine_blocks() {
    local blocks="$1"
    local address="$2"
    local result
    result="$(rpc_call "generatetoaddress" "[${blocks},\"${address}\"]")"
    rpc_has_error "${result}" && fail "generatetoaddress failed: ${result}"
    return 0
}

send_to_address() {
    local address="$1"
    local amount="$2"
    local result
    result="$(rpc_call "wallet.sendtoaddress" "[\"${address}\",${amount}]")"
    rpc_has_error "${result}" && fail "wallet.sendtoaddress failed: ${result}"
    local txid
    txid="$(jq -r '.result.txid // .result // empty' <<<"${result}")"
    [[ -n "${txid}" && "${txid}" != "null" ]] || fail "wallet.sendtoaddress returned empty txid"
    return 0
}

sync_health() {
    local result
    result="$(rpc_call "blockchain.getsynchealth" '[]')"
    rpc_has_error "${result}" && fail "blockchain.getsynchealth failed: ${result}"
    jq -c '.result' <<<"${result}"
}

wait_for_sync_state() {
    local expected_height="$1"
    local label="$2"
    for _ in $(seq 1 120); do
        local health
        health="$(sync_health)"
        if jq -e \
            --argjson expected_height "${expected_height}" \
            '
            .active_height == $expected_height and
            .chaindb_tip_height == $expected_height and
            .canonical_state_aligned == true and
            (.active_best_hash | type) == "string" and
            (.active_best_hash | length) > 0 and
            .chaindb_tip_hash == .active_best_hash
            ' <<<"${health}" >/dev/null; then
            return 0
        fi
        sleep 1
    done
    fail "${label}: timed out waiting for canonical height ${expected_height}"
}

capture_headers_json() {
    local tip_json tip
    tip_json="$(rpc_call "getblockcount" '[]')"
    rpc_has_error "${tip_json}" && fail "getblockcount failed: ${tip_json}"
    tip="$(jq -r '.result // -1' <<<"${tip_json}")"
    [[ "${tip}" =~ ^[0-9]+$ ]] || fail "invalid getblockcount result: ${tip_json}"

    local rows=()
    local height hash_result hash header_result row
    for height in $(seq 0 "${tip}"); do
        hash_result="$(rpc_call "getblockhash" "[${height}]")"
        rpc_has_error "${hash_result}" && fail "getblockhash(${height}) failed: ${hash_result}"
        hash="$(jq -r '.result // empty' <<<"${hash_result}")"
        [[ -n "${hash}" && "${hash}" != "null" ]] || fail "empty block hash at height ${height}"

        header_result="$(rpc_call "getblockheader" "[\"${hash}\"]")"
        rpc_has_error "${header_result}" && fail "getblockheader(${hash}) failed: ${header_result}"
        row="$(jq -c \
            '.result | {
                height,
                hash,
                previousblockhash,
                merkleroot,
                time,
                bits,
                nonce,
                utreexo_root_raw,
                chainwork
            }' <<<"${header_result}")"
        rows+=("${row}")
    done

    printf '%s\n' "${rows[@]}" | jq -s '.'
}

capture_filters_json() {
    local tip_json tip filters_result
    tip_json="$(rpc_call "getblockcount" '[]')"
    rpc_has_error "${tip_json}" && fail "getblockcount failed: ${tip_json}"
    tip="$(jq -r '.result // -1' <<<"${tip_json}")"
    [[ "${tip}" =~ ^[0-9]+$ ]] || fail "invalid getblockcount result while capturing filters: ${tip_json}"

    filters_result="$(rpc_call "blockchain.getblockfilters" "[0,$((tip + 1))]")"
    rpc_has_error "${filters_result}" && fail "blockchain.getblockfilters failed: ${filters_result}"
    jq -c \
        '.result.filters | map({
            height,
            block_hash,
            filter_hash,
            filter,
            element_count,
            tx_count,
            coinbase_txid
        })' <<<"${filters_result}"
}

capture_state_bundle() {
    local health headers filters
    health="$(sync_health)"
    headers="$(capture_headers_json)"
    filters="$(capture_filters_json)"

    jq -n \
        --argjson health "${health}" \
        --argjson headers "${headers}" \
        --argjson filters "${filters}" \
        '{
            active_height: ($health.active_height // -1),
            active_best_hash: ($health.active_best_hash // ""),
            chaindb_tip_height: ($health.chaindb_tip_height // -1),
            chaindb_tip_hash: ($health.chaindb_tip_hash // ""),
            canonical_state_aligned: ($health.canonical_state_aligned // false),
            latest_utreexo_checkpoint_found: ($health.latest_utreexo_checkpoint_found // false),
            latest_utreexo_checkpoint_height: ($health.latest_utreexo_checkpoint_height // -1),
            latest_utreexo_checkpoint_has_checksum: ($health.latest_utreexo_checkpoint_has_checksum // false),
            utreexo_checksum_version: ($health.utreexo_checksum_version // ""),
            forest_tip_marker_found: ($health.forest_tip_marker_found // false),
            forest_tip_marker_height: ($health.forest_tip_marker_height // -1),
            forest_tip_marker_hash: ($health.forest_tip_marker_hash // ""),
            headers: $headers,
            filters: $filters
        }'
}

# The utreexo CHECKPOINT fields are deliberately excluded from the equivalence
# comparison. They are an accelerator, not consensus state, and live and
# replayed nodes are DESIGNED to differ on them:
#
#   live    — a full forest checkpoint is written only when
#             tip % utreexo.checkpoint_interval == 0 (default 500). At this
#             test's height of 113 none is written, so the height stays 0 and
#             CHECKSUM_VERSION is unset.
#   replay  — BlockReindexer deliberately emits a final checkpoint at the
#             reindex tip even when the every-N gating skipped it, "so the
#             reindexed datadir restarts instantly (no replay window)"
#             (reindexer.cpp, campaign phase 3). Height becomes the tip and
#             CHECKSUM_VERSION becomes "1".
#
# Everything that must match still does — including all 114 headers with their
# chainwork and utreexo roots, all 113 filter hashes, both tip pointers and the
# forest tip marker. Comparing the checkpoint fields asserted an equality the
# design never promised. The divergence is asserted POSITIVELY below instead of
# merely ignored.
readonly REPLAY_DIVERGENT_FIELDS='["latest_utreexo_checkpoint_height", "utreexo_checksum_version"]'

assert_same_bundle() {
    local lhs_file="$1"
    local rhs_file="$2"
    local label="$3"
    if ! jq -s -e --argjson drop "${REPLAY_DIVERGENT_FIELDS}" \
        '(.[0] | delpaths($drop | map([.]))) == (.[1] | delpaths($drop | map([.])))' \
        "${lhs_file}" "${rhs_file}" >/dev/null; then
        printf '%s\n' "${label} mismatch:" >&2
        printf 'lhs=%s\n' "$(jq -c '.' "${lhs_file}")" >&2
        printf 'rhs=%s\n' "$(jq -c '.' "${rhs_file}")" >&2
        fail "${label} state mismatch"
    fi
}

prepare_replay_datadir() {
    rm -rf "${REPLAY_DIR}"
    mkdir -p "${REPLAY_DIR}"
    cp -a "${BASE_DIR}/." "${REPLAY_DIR}/"
    rm -rf "${REPLAY_DIR}/blockchain"
    rm -f \
        "${REPLAY_DIR}/.cookie" \
        "${REPLAY_DIR}/.daemon_id" \
        "${REPLAY_DIR}/dinerod.lock" \
        "${REPLAY_DIR}/dinerod.pid" \
        "${REPLAY_DIR}/mempool.log" \
        "${REPLAY_DIR}/mining.log" \
        "${REPLAY_DIR}/p2p.log" \
        "${REPLAY_DIR}/wallet.log"
}

require_tools

BASE_RPC_PORT=$((40000 + RANDOM % 1000))
BASE_P2P_PORT=$((BASE_RPC_PORT + 1))
BASE_WALLET_PORT=$((BASE_RPC_PORT + 2))
REPLAY_RPC_PORT=$((BASE_RPC_PORT + 10))
REPLAY_P2P_PORT=$((BASE_RPC_PORT + 11))
REPLAY_WALLET_PORT=$((BASE_RPC_PORT + 12))

start_node "${BASE_DIR}" "${LOG_BASE}" "${BASE_RPC_PORT}" "${BASE_P2P_PORT}" "${BASE_WALLET_PORT}"
wait_rpc || fail "base daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

RECIPIENT_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${RECIPIENT_RESULT}" && fail "wallet.getnewaddress failed for first recipient: ${RECIPIENT_RESULT}"
RECIPIENT_ADDR="$(rpc_address "${RECIPIENT_RESULT}")"
[[ -n "${RECIPIENT_ADDR}" ]] || fail "wallet.getnewaddress returned empty first recipient address"

SECOND_RECIPIENT_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${SECOND_RECIPIENT_RESULT}" && fail "wallet.getnewaddress failed for second recipient: ${SECOND_RECIPIENT_RESULT}"
SECOND_RECIPIENT_ADDR="$(rpc_address "${SECOND_RECIPIENT_RESULT}")"
[[ -n "${SECOND_RECIPIENT_ADDR}" ]] || fail "wallet.getnewaddress returned empty second recipient address"

info "Building a nontrivial live chain for header/filter replay equivalence"
mine_blocks 110 "${MINER_ADDR}"
send_to_address "${RECIPIENT_ADDR}" "1.25"
mine_blocks 1 "${MINER_ADDR}"
send_to_address "${SECOND_RECIPIENT_ADDR}" "0.75"
send_to_address "${RECIPIENT_ADDR}" "0.50"
mine_blocks 2 "${MINER_ADDR}"
wait_for_sync_state 113 "live steady state"

capture_state_bundle > "${STATE_LIVE_FILE}"
jq -e \
    '
    .canonical_state_aligned == true and
    (.headers | length) == 114 and
    (.filters | length) >= 1 and
    (.filters[-1].height == .active_height)
    ' \
    "${STATE_LIVE_FILE}" >/dev/null || fail "live state bundle missing expected coverage: $(cat "${STATE_LIVE_FILE}")"
pass "Captured live header/filter bundle"

stop_node

info "Cloning raw block history into replay datadir"
prepare_replay_datadir

start_node "${REPLAY_DIR}" "${LOG_REPLAY}" "${REPLAY_RPC_PORT}" "${REPLAY_P2P_PORT}" "${REPLAY_WALLET_PORT}" --reindex-chainstate
wait_rpc || fail "replay daemon did not reach RPC readiness"
wait_for_sync_state 113 "replay steady state"

capture_state_bundle > "${STATE_REPLAY_FILE}"
jq -e \
    '
    .canonical_state_aligned == true and
    (.headers | length) == 114 and
    (.filters | length) >= 1 and
    (.filters[-1].height == .active_height)
    ' \
    "${STATE_REPLAY_FILE}" >/dev/null || fail "replay state bundle missing expected coverage: $(cat "${STATE_REPLAY_FILE}")"

assert_same_bundle "${STATE_LIVE_FILE}" "${STATE_REPLAY_FILE}" "header/filter replay equivalence"
pass "Replay rebuilt identical headers, chainwork, and served filter hashes"

# Pin the designed divergence rather than merely excluding it. If the reindexer
# ever stops emitting its final tip checkpoint, a reindexed datadir silently
# regains a replay window on next start — a performance regression nothing else
# here would catch.
LIVE_CKPT="$(jq -r '.latest_utreexo_checkpoint_height' "${STATE_LIVE_FILE}")"
REPLAY_CKPT="$(jq -r '.latest_utreexo_checkpoint_height' "${STATE_REPLAY_FILE}")"
REPLAY_ACTIVE="$(jq -r '.active_height' "${STATE_REPLAY_FILE}")"
[[ "${REPLAY_CKPT}" == "${REPLAY_ACTIVE}" ]] \
    || fail "reindex did not emit its final tip checkpoint: ${REPLAY_CKPT} != ${REPLAY_ACTIVE}"
[[ "${LIVE_CKPT}" != "${REPLAY_CKPT}" ]] \
    || info "live and replay checkpoints coincide (tip landed on an interval boundary)"
pass "Reindexed datadir carries a tip checkpoint (live=${LIVE_CKPT}, replay=${REPLAY_CKPT})"

REPLAY_ADDR_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${REPLAY_ADDR_RESULT}" && fail "wallet.getnewaddress failed after replay: ${REPLAY_ADDR_RESULT}"
REPLAY_MINER_ADDR="$(rpc_address "${REPLAY_ADDR_RESULT}")"
[[ -n "${REPLAY_MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty replay mining address"
mine_blocks 1 "${REPLAY_MINER_ADDR}"
wait_for_sync_state 114 "post-replay mining"
pass "Replay node advanced cleanly after equivalence check"

stop_node
