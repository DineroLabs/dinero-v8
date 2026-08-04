#!/usr/bin/env bash
#
# Confirmed-spend reorg reconciliation regression.
#
# Proves that when a CSN-originated spend is confirmed and then invalidated by a
# bridge-side reorg:
#   1. canonical Utreexo state rewinds to the new winning branch
#   2. wallet/mempool overlay reconciles without corrupting canonical state
#   3. stale proof roots are rejected and the resurrected UTXO can be re-proved
#

set -euo pipefail

SYNC_TIMEOUT=${TIMEOUT:-240}
FUNDING_BLOCKS=${FUNDING_BLOCKS:-120}
POST_SPEND_CONFIRM_BLOCKS=${POST_SPEND_CONFIRM_BLOCKS:-2}
ALT_BRANCH_BLOCKS=${ALT_BRANCH_BLOCKS:-3}
CHECKPOINT_INTERVAL=${CHECKPOINT_INTERVAL:-500}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

[[ "${CHECKPOINT_INTERVAL}" =~ ^[1-9][0-9]*$ ]] \
    || { echo "CHECKPOINT_INTERVAL must be a positive integer"; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ -n "${DINEROD:-}" && -x "${DINEROD}" ]]; then
    DINEROD="${DINEROD}"
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    echo "dinerod not found"
    exit 1
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

DATADIR_BRIDGE=""
DATADIR_CSN=""
PID_BRIDGE=""
PID_CSN=""
EXIT_CODE=0

info() { echo -e "${CYAN}$1${NC}"; }
pass() { echo -e "${GREEN}  $1${NC}"; }
fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    [[ -n "${PID_CSN}" ]] && kill "${PID_CSN}" 2>/dev/null || true
    [[ -n "${PID_BRIDGE}" ]] && kill "${PID_BRIDGE}" 2>/dev/null || true
    [[ -n "${DATADIR_CSN}" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    [[ -n "${DATADIR_BRIDGE}" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    sleep 1
    if [[ ${EXIT_CODE} -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 120 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -120 "${DATADIR_BRIDGE}/daemon.log"
        echo -e "\n${RED}=== CSN daemon.log (last 120 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -120 "${DATADIR_CSN}/daemon.log"
        if [[ "${KEEP_TMP_ON_FAIL}" == "1" ]]; then
            echo -e "\n${YELLOW}Keeping temp dirs for debugging:${NC}"
            [[ -n "${DATADIR_BRIDGE}" ]] && echo "  Bridge: ${DATADIR_BRIDGE}"
            [[ -n "${DATADIR_CSN}" ]] && echo "  CSN:    ${DATADIR_CSN}"
            return
        fi
    fi
    [[ -n "${DATADIR_BRIDGE}" && -d "${DATADIR_BRIDGE}" ]] && rm -rf "${DATADIR_BRIDGE}"
    [[ -n "${DATADIR_CSN}" && -d "${DATADIR_CSN}" ]] && rm -rf "${DATADIR_CSN}"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

RPC_PORT_BRIDGE=$((35000 + RANDOM % 1000))
P2P_PORT_BRIDGE=$((RPC_PORT_BRIDGE + 1))
RPC_PORT_CSN=$((RPC_PORT_BRIDGE + 2))
P2P_PORT_CSN=$((RPC_PORT_BRIDGE + 3))
WALLET_PORT_BRIDGE=$((RPC_PORT_BRIDGE + 4))
WALLET_PORT_CSN=$((RPC_PORT_BRIDGE + 5))

rpc_call() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local params="$*"
    local cookie
    cookie=$(cat "${datadir}/.cookie" 2>/dev/null || true)
    [[ -z "${cookie}" ]] && return 1
    local json_params="[]"
    if [[ -n "${params}" ]]; then
        if [[ "${params}" == \[*\] ]]; then
            json_params="${params}"
        else
            json_params="[${params}]"
        fi
    fi
    curl -s --connect-timeout 2 --max-time 30 \
        -u "${cookie}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

rpc_has_error() {
    local compact
    compact=$(echo "$1" | tr -d '\n\t ')
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

rpc_result() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local response
    response=$(rpc_call "${port}" "${datadir}" "${method}" "$@")
    if rpc_has_error "${response}"; then
        echo "${response}" | tr -d '\n\t'
        return 1
    fi
    echo "${response}" | jq '.result'
}

rpc_scalar() {
    local port=$1
    local datadir=$2
    local method=$3
    local jq_expr=$4
    shift 4
    local response
    response=$(rpc_call "${port}" "${datadir}" "${method}" "$@")
    rpc_has_error "${response}" && return 1
    echo "${response}" | jq -r "${jq_expr} // empty"
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        if [[ -f "${datadir}/.cookie" ]]; then
            local height
            height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result' 2>/dev/null || true)
            [[ -n "${height}" ]] && return 0
        fi
        sleep 1
    done
}

wait_for_sync() {
    local port=$1
    local datadir=$2
    local target_height=$3
    local target_hash=$4
    local timeout=$5
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local height hash
        height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result' 2>/dev/null || true)
        hash=$(rpc_scalar "${port}" "${datadir}" "getbestblockhash" '.result' 2>/dev/null || true)
        [[ "${height}" == "${target_height}" && "${hash}" == "${target_hash}" ]] && return 0
        sleep 1
    done
}

wait_for_spendable_utxo() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local utxo
        utxo=$(rpc_result "${port}" "${datadir}" "wallet.listunspent" "[1,9999999]" 2>/dev/null | jq 'map(select(.spendable == true))[0] // empty')
        if [[ -n "${utxo}" ]]; then
            echo "${utxo}"
            return 0
        fi
        sleep 1
    done
}

wait_for_mempool_contains() {
    local port=$1
    local datadir=$2
    local txid=$3
    local timeout=$4
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local mempool
        mempool=$(rpc_result "${port}" "${datadir}" "getrawmempool" "[]" 2>/dev/null || true)
        if [[ -n "${mempool}" ]] && echo "${mempool}" | jq -e --arg txid "${txid}" 'index($txid) != null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
}

wait_for_mempool_size() {
    local port=$1
    local datadir=$2
    local expected=$3
    local timeout=$4
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local count
        count=$(rpc_result "${port}" "${datadir}" "getrawmempool" "[]" 2>/dev/null | jq -r 'length' || true)
        [[ "${count}" == "${expected}" ]] && return 0
        sleep 1
    done
}

wait_for_confirmed_tx() {
    local port=$1
    local datadir=$2
    local txid=$3
    local timeout=$4
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local tx_info conf
        tx_info=$(rpc_call "${port}" "${datadir}" "wallet.gettransaction" "\"${txid}\"" 2>/dev/null || true)
        if ! rpc_has_error "${tx_info}"; then
            conf=$(echo "${tx_info}" | jq -r '.result.confirmations // 0')
            if [[ -n "${conf}" && "${conf}" -gt 0 ]]; then
                return 0
            fi
        fi
        sleep 1
    done
}

wait_for_height_at_most() {
    local port=$1
    local datadir=$2
    local max_height=$3
    local timeout=$4
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local height
        height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result' 2>/dev/null || true)
        if [[ -n "${height}" && "${height}" -le "${max_height}" ]]; then
            return 0
        fi
        sleep 1
    done
}

wait_for_outpoint_presence() {
    local port=$1
    local datadir=$2
    local txid=$3
    local vout=$4
    local expected_count=$5
    local timeout=$6
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local count
        count=$(rpc_result "${port}" "${datadir}" "wallet.listunspent" "[1,9999999]" 2>/dev/null | jq -r --arg txid "${txid}" --argjson vout "${vout}" '[.[] | select(.txid == $txid and .vout == $vout)] | length' || true)
        [[ "${count}" == "${expected_count}" ]] && return 0
        sleep 1
    done
}

mine_blocks_to_address() {
    local port=$1
    local datadir=$2
    local total=$3
    local address=$4
    local result
    result=$(rpc_call "${port}" "${datadir}" "generatetoaddress" "${total}, \"${address}\"")
    rpc_has_error "${result}" && return 1
    return 0
}

capture_state_json() {
    local port=$1
    local datadir=$2
    local height tip commitment roots

    height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result')
    tip=$(rpc_scalar "${port}" "${datadir}" "getbestblockhash" '.result')
    commitment=$(rpc_call "${port}" "${datadir}" "blockchain.getutreexocommitment" "[]")
    roots=$(rpc_call "${port}" "${datadir}" "blockchain.getutreexoroots" "[]")

    jq -n \
        --argjson height "${height}" \
        --arg tip "${tip}" \
        --arg commitment "$(echo "${commitment}" | jq -r '.result.commitment // empty')" \
        --argjson num_leaves "$(echo "${commitment}" | jq -r '.result.num_leaves // 0')" \
        --argjson num_roots "$(echo "${commitment}" | jq -r '.result.num_roots // 0')" \
        --argjson roots "$(echo "${roots}" | jq -c '.result.roots // []')" \
        '{
            height: $height,
            tip: $tip,
            commitment: $commitment,
            num_leaves: $num_leaves,
            num_roots: $num_roots,
            roots: $roots
        }'
}

assert_same_state() {
    local lhs_json=$1
    local rhs_json=$2
    local label=$3
    local mismatch
    mismatch=$(jq -n \
        --argjson lhs "${lhs_json}" \
        --argjson rhs "${rhs_json}" \
        '{
            height: ($lhs.height == $rhs.height),
            tip: ($lhs.tip == $rhs.tip),
            commitment: ($lhs.commitment == $rhs.commitment),
            num_leaves: ($lhs.num_leaves == $rhs.num_leaves),
            num_roots: ($lhs.num_roots == $rhs.num_roots),
            roots: ($lhs.roots == $rhs.roots)
        }')
    if [[ "$(echo "${mismatch}" | jq -r 'all(.[]; . == true)')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "lhs=$(echo "${lhs_json}" | jq -c '.')"
        echo "rhs=$(echo "${rhs_json}" | jq -c '.')"
        echo "eq =$(echo "${mismatch}" | jq -c '.')"
        fail "${label} state mismatch"
    fi
}

assert_state_changed() {
    local before_json=$1
    local after_json=$2
    local label=$3
    local changed
    changed=$(jq -n \
        --argjson before "${before_json}" \
        --argjson after "${after_json}" \
        '{
            tip_changed: ($after.tip != $before.tip),
            commitment_changed: ($after.commitment != $before.commitment),
            roots_changed: ($after.roots != $before.roots or
                            $after.num_leaves != $before.num_leaves or
                            $after.num_roots != $before.num_roots)
        }')
    if [[ "$(echo "${changed}" | jq -r '.tip_changed and .commitment_changed and .roots_changed')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "before=$(echo "${before_json}" | jq -c '.')"
        echo "after =$(echo "${after_json}" | jq -c '.')"
        echo "eval =$(echo "${changed}" | jq -c '.')"
        fail "${label} did not change canonical state as expected"
    fi
}

start_bridge() {
    "$DINEROD" --regtest \
        --datadir="${DATADIR_BRIDGE}" \
        --rpcport="${RPC_PORT_BRIDGE}" \
        --port="${P2P_PORT_BRIDGE}" \
        --wallet-socket-port="${WALLET_PORT_BRIDGE}" \
        --listen=1 \
        --connect="127.0.0.1:${P2P_PORT_CSN}" \
        --utreexo=1 \
        --utreexo-bridge=1 \
        >> "${DATADIR_BRIDGE}/daemon.log" 2>&1 &
    PID_BRIDGE=$!
    wait_for_ready "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 30 || fail "Bridge node failed to start"
}

start_csn() {
    "$DINEROD" --regtest \
        --datadir="${DATADIR_CSN}" \
        --rpcport="${RPC_PORT_CSN}" \
        --port="${P2P_PORT_CSN}" \
        --wallet-socket-port="${WALLET_PORT_CSN}" \
        --listen=1 \
        --utreexo=1 \
        --utreexo-stateless=1 \
        --utreexo.checkpoint_interval="${CHECKPOINT_INTERVAL}" \
        --connect="127.0.0.1:${P2P_PORT_BRIDGE}" \
        >> "${DATADIR_CSN}/daemon.log" 2>&1 &
    PID_CSN=$!
    wait_for_ready "${RPC_PORT_CSN}" "${DATADIR_CSN}" 30 || fail "CSN node failed to start"
}

echo ""
echo "================================================================="
echo "  CSN SPEND REORG RECONCILIATION TEST"
echo "================================================================="
echo ""

command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_spend_reorg_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_spend_reorg_XXXXXX)

info "[1/10] Starting bridge and CSN nodes"
start_bridge
start_csn
pass "Bridge and CSN nodes ready"

info "\n[2/10] Creating wallets and funding a mature CSN UTXO"
BRIDGE_WALLET=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.createhd" '"bridge_spend_reorg"')
CSN_WALLET=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.createhd" '"csn_spend_reorg"')
rpc_has_error "${BRIDGE_WALLET}" && fail "Bridge wallet creation failed"
rpc_has_error "${CSN_WALLET}" && fail "CSN wallet creation failed"
MINER_ADDRESS=$(echo "${BRIDGE_WALLET}" | jq -r '.result.first_address // empty')
CSN_RECEIVE_ADDRESS=$(echo "${CSN_WALLET}" | jq -r '.result.first_address // empty')
BRIDGE_RECIPIENT=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.getnewaddress" '.result.address // .result // empty')
[[ -n "${MINER_ADDRESS}" && -n "${CSN_RECEIVE_ADDRESS}" && -n "${BRIDGE_RECIPIENT}" ]] || fail "Failed to derive wallet addresses"

mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${FUNDING_BLOCKS}" "${CSN_RECEIVE_ADDRESS}" \
    || fail "Failed to mine funding blocks"
HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to sync funded chain"
CANDIDATE_UTXO=$(wait_for_spendable_utxo "${RPC_PORT_CSN}" "${DATADIR_CSN}" 60) \
    || fail "CSN wallet never saw a spendable funded UTXO"
[[ -n "${CANDIDATE_UTXO}" ]] || fail "Failed to capture a spendable funded UTXO"
PRE_SEND_UTXOS=$(rpc_result "${RPC_PORT_CSN}" "${DATADIR_CSN}" \
    "wallet.listunspent" "[1,9999999]")
BASE_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
BASE_HEIGHT=$(echo "${BASE_STATE}" | jq -r '.height')
pass "Spendable CSN funding captured before transaction construction"

info "\n[3/10] Broadcasting a CSN-originated spend"
SEND_RESULT=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.sendtoaddress" "[\"${BRIDGE_RECIPIENT}\",1.0]")
rpc_has_error "${SEND_RESULT}" && fail "wallet.sendtoaddress failed: $(echo "${SEND_RESULT}" | tr -d '\n\t')"
SPEND_TXID=$(echo "${SEND_RESULT}" | jq -r '(.result.txid // .result // empty) | strings')
[[ -n "${SPEND_TXID}" ]] || fail "wallet.sendtoaddress returned empty txid"
SPEND_DECODED=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" \
    "wallet.getrawtransaction" "\"${SPEND_TXID}\", true")
rpc_has_error "${SPEND_DECODED}" \
    && fail "wallet.getrawtransaction failed for CSN spend"
[[ "$(echo "${SPEND_DECODED}" | jq -r '.result.vin | length')" == "1" ]] \
    || fail "test requires one actual spent input"
FUNDED_TXID=$(echo "${SPEND_DECODED}" | jq -r '.result.vin[0].txid // empty')
FUNDED_VOUT=$(echo "${SPEND_DECODED}" | jq -r '.result.vin[0].vout // empty')
[[ -n "${FUNDED_TXID}" && -n "${FUNDED_VOUT}" ]] \
    || fail "failed to decode the actual spent outpoint"
[[ "$(echo "${PRE_SEND_UTXOS}" | jq -r --arg txid "${FUNDED_TXID}" \
    --argjson vout "${FUNDED_VOUT}" \
    '[.[] | select(.txid == $txid and .vout == $vout and .spendable == true)] | length')" == "1" ]] \
    || fail "actual spend input was not in the pre-send spendable set"
wait_for_mempool_contains "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${SPEND_TXID}" 30 \
    || fail "Bridge never observed the CSN spend in mempool"
wait_for_mempool_contains "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${SPEND_TXID}" 30 \
    || fail "CSN never retained its own spend in mempool"
assert_same_state "${BASE_STATE}" "$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")" \
    "pending spend before confirmation"
pass "Pending spend preserved canonical Utreexo state"

info "\n[4/10] Confirming the spend and mining a descendant block"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${POST_SPEND_CONFIRM_BLOCKS}" "${MINER_ADDRESS}" \
    || fail "Failed to mine confirmation branch"
HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to sync confirmed spend branch"
wait_for_confirmed_tx "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${SPEND_TXID}" 30 \
    || fail "CSN wallet never marked spend confirmed"
wait_for_outpoint_presence "${RPC_PORT_CSN}" "${DATADIR_CSN}" \
    "${FUNDED_TXID}" "${FUNDED_VOUT}" 0 30 \
    || fail "precondition not met: confirmed spend never removed the funded outpoint"
SPEND_CONFIRM_HEIGHT=$((BASE_HEIGHT + 1))
SPEND_BLOCK_HASH=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockhash" '.result' "[${SPEND_CONFIRM_HEIGHT}]")
[[ -n "${SPEND_BLOCK_HASH}" ]] || fail "Failed to resolve spend confirmation block hash"
CONFIRMED_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
CONFIRMED_ROOT=$(echo "${CONFIRMED_STATE}" | jq -r '.commitment')
pass "Spend confirmed on branch rooted at ${SPEND_BLOCK_HASH:0:16}..."

info "\n[5/10] Invalidating the spend block and clearing bridge mempool"
INVALIDATE_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "invalidateblock" "\"${SPEND_BLOCK_HASH}\"")
rpc_has_error "${INVALIDATE_RESULT}" && fail "invalidateblock failed: $(echo "${INVALIDATE_RESULT}" | tr -d '\n\t')"
wait_for_height_at_most "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${BASE_HEIGHT}" 60 \
    || fail "Bridge did not rewind below spend confirmation height"
BRIDGE_CLEAR=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "mempool.clear" "[]")
rpc_has_error "${BRIDGE_CLEAR}" && fail "bridge mempool.clear failed: $(echo "${BRIDGE_CLEAR}" | tr -d '\n\t')"
wait_for_mempool_size "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 0 30 \
    || fail "bridge mempool.clear did not empty the mempool"
pass "Bridge rewound and prevented disconnected spend from being re-mined automatically"

info "\n[6/10] Mining a longer competing branch without the spend"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${ALT_BRANCH_BLOCKS}" "${MINER_ADDRESS}" \
    || fail "Failed to mine alternative branch"
HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to follow alternative branch"
REORG_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
assert_state_changed "${CONFIRMED_STATE}" "${REORG_STATE}" "post-reorg CSN state"
pass "CSN canonical Utreexo state changed with the winning reorg branch"

info "\n[7/10] Proving the old confirmed root is now stale"
PROOF_STATUS=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.proofstatus" "\"${CONFIRMED_ROOT}\"")
rpc_has_error "${PROOF_STATUS}" && fail "wallet.proofstatus failed after reorg"
[[ "$(echo "${PROOF_STATUS}" | jq -r '.result.stale')" == "true" ]] || fail "Expected confirmed-root proof status to become stale after reorg"
pass "Old confirmed root marked stale"

info "\n[8/10] Checking wallet/mempool overlay reconciliation on the CSN"
CSN_MEMPOOL=$(rpc_result "${RPC_PORT_CSN}" "${DATADIR_CSN}" "getrawmempool" "[]" 2>/dev/null || true)
CSN_HAS_TX="false"
if [[ -n "${CSN_MEMPOOL}" ]] && echo "${CSN_MEMPOOL}" | jq -e --arg txid "${SPEND_TXID}" 'index($txid) != null' >/dev/null 2>&1; then
    CSN_HAS_TX="true"
fi

if [[ "${CSN_HAS_TX}" == "true" ]]; then
    pass "Disconnected spend returned to the CSN mempool"
    wait_for_outpoint_presence "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${FUNDED_TXID}" "${FUNDED_VOUT}" 0 20 \
        || fail "Original outpoint reappeared even though the disconnected spend is still pending in CSN mempool"
    CSN_CLEAR=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "mempool.clear" "[]")
    rpc_has_error "${CSN_CLEAR}" && fail "CSN mempool.clear failed: $(echo "${CSN_CLEAR}" | tr -d '\n\t')"
    wait_for_mempool_size "${RPC_PORT_CSN}" "${DATADIR_CSN}" 0 30 || fail "CSN mempool.clear did not empty the mempool"
fi

wait_for_outpoint_presence "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${FUNDED_TXID}" "${FUNDED_VOUT}" 1 30 \
    || fail "Actual spent outpoint never returned to the spendable set after reorg reconciliation"
pass "Wallet overlay reconciled and the resurrected coin is spendable again"

info "\n[9/10] Re-proving the resurrected UTXO against the new winning branch"
PROOF_UPDATE_PARAMS=$(jq -nc \
    --arg root "${CONFIRMED_ROOT}" \
    --arg txid "${FUNDED_TXID}" \
    --argjson vout "${FUNDED_VOUT}" \
    '[{root_from:$root,outpoints:[{txid:$txid,vout:$vout}]}]')
PROOF_UPDATES=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "blockchain.getproofupdates" "${PROOF_UPDATE_PARAMS}")
rpc_has_error "${PROOF_UPDATES}" && fail "blockchain.getproofupdates failed after reorg: $(echo "${PROOF_UPDATES}" | tr -d '\n\t')"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.status')" == "updated" ]] || fail "Expected updated proof refresh status after reorg"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.proofs | length')" == "1" ]] || fail "Expected exactly one updated proof after reorg"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.proofs[0].success')" == "true" ]] || fail "Re-proving resurrected UTXO failed after reorg"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '(.result.stump_num_leaves // .result.num_leaves // 0)')" -ge 1 ]] || fail "Proof update missing stump_num_leaves after reorg"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '(.result.stump_roots // .result.roots // []) | length')" -ge 1 ]] || fail "Proof update missing stump_roots after reorg"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '(.result.proofs[0].script_pubkey // "") | length')" -gt 0 ]] || fail "Re-proved UTXO missing script_pubkey after reorg"

VERIFY_PARAMS=$(jq -nc \
    --arg txid "${FUNDED_TXID}" \
    --argjson vout "${FUNDED_VOUT}" \
    --arg root_to "$(echo "${PROOF_UPDATES}" | jq -r '.result.root_to')" \
    --arg tip_hash "$(echo "${PROOF_UPDATES}" | jq -r '.result.block_hash')" \
    --argjson proof "$(echo "${PROOF_UPDATES}" | jq '.result.proofs[0]')" \
    '[{
        txid:$txid,
        vout:$vout,
        proof:$proof,
        expected_utreexo_root:$root_to,
        expected_tip_hash:$tip_hash,
        enforce_bound_context:true
    }]')
VERIFY_RESULT=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.verifyutxoproof" "${VERIFY_PARAMS}")
rpc_has_error "${VERIFY_RESULT}" && fail "wallet.verifyutxoproof failed after reorg: $(echo "${VERIFY_RESULT}" | tr -d '\n\t')"
[[ "$(echo "${VERIFY_RESULT}" | jq -r '.result.valid')" == "true" ]] || fail "Resurrected UTXO proof did not verify on CSN after reorg"
pass "Stale proof root rejected; resurrected UTXO re-proved on the new branch"

info "\n[10/10] Verifying CSN full-checkpoint write reduction"
SYNC_HEALTH=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "blockchain.getsynchealth" "[]")
rpc_has_error "${SYNC_HEALTH}" && fail "blockchain.getsynchealth failed"
CSN_CHECKPOINT_WRITES=$(echo "${SYNC_HEALTH}" | jq -r '.result.sync_stats.csn_checkpoint_writes // -1')
[[ "${CSN_CHECKPOINT_WRITES}" -ge 0 ]] || fail "Missing csn_checkpoint_writes metric"
MAX_EXPECTED_WRITES=$((HEIGHT_BRIDGE / CHECKPOINT_INTERVAL + 1))
[[ "${CSN_CHECKPOINT_WRITES}" -le "${MAX_EXPECTED_WRITES}" ]] \
    || fail "CSN wrote ${CSN_CHECKPOINT_WRITES} full checkpoints for ${HEIGHT_BRIDGE} blocks at interval ${CHECKPOINT_INTERVAL}"
[[ "${CSN_CHECKPOINT_WRITES}" -lt "${HEIGHT_BRIDGE}" ]] \
    || fail "CSN still writes a full forest checkpoint every block"
pass "CSN full checkpoints reduced to ${CSN_CHECKPOINT_WRITES} write(s) for ${HEIGHT_BRIDGE} blocks"

echo ""
echo "================================================================="
echo -e "${GREEN}  CSN SPEND REORG RECONCILIATION PASSED${NC}"
echo "================================================================="
echo ""
echo "Validated:"
echo "  - Confirmed spend was invalidated by a longer competing branch"
echo "  - Canonical Utreexo state rewound cleanly on the CSN"
echo "  - Wallet/mempool overlay reconciled without corrupting canonical state"
echo "  - The resurrected UTXO could be re-proved against the new branch"
echo "  - Full CSN forest checkpoints were gated to every ${CHECKPOINT_INTERVAL} blocks"
echo ""

exit 0
