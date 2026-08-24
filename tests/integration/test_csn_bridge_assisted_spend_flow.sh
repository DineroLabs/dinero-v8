#!/usr/bin/env bash
#
# Bridge-assisted CSN spend flow regression.
#
# Proves the stateless/phone spending lifecycle against the current
# architecture:
#   1. Bridge funds a CSN wallet-owned confirmed UTXO
#   2. Bridge re-proves that outpoint after the tip root changes
#   3. CSN verifies the refreshed proof against its current chain context
#   4. CSN spends the coin
#   5. Canonical Utreexo state does not move while the spend is only pending
#   6. Canonical Utreexo state advances only when the confirming block arrives
#

set -euo pipefail

SYNC_TIMEOUT=${TIMEOUT:-180}
FUNDING_BLOCKS=${FUNDING_BLOCKS:-110}
REFRESH_ADVANCE_BLOCKS=${REFRESH_ADVANCE_BLOCKS:-1}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

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
        echo -e "\n${RED}=== Bridge daemon.log (last 80 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -80 "${DATADIR_BRIDGE}/daemon.log"
        echo -e "\n${RED}=== CSN daemon.log (last 80 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -80 "${DATADIR_CSN}/daemon.log"
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

# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT_BRIDGE=$(alloc_port_base)
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

assert_state_advanced() {
    local before_json=$1
    local after_json=$2
    local label=$3
    local advanced
    advanced=$(jq -n \
        --argjson before "${before_json}" \
        --argjson after "${after_json}" \
        '{
            height_advanced: ($after.height > $before.height),
            tip_changed: ($after.tip != $before.tip),
            commitment_changed: ($after.commitment != $before.commitment),
            roots_changed: ($after.roots != $before.roots or
                            $after.num_leaves != $before.num_leaves or
                            $after.num_roots != $before.num_roots)
        }')
    if [[ "$(echo "${advanced}" | jq -r '.height_advanced and .tip_changed and .commitment_changed and .roots_changed')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "before=$(echo "${before_json}" | jq -c '.')"
        echo "after =$(echo "${after_json}" | jq -c '.')"
        echo "eval =$(echo "${advanced}" | jq -c '.')"
        fail "${label} did not advance canonical Utreexo state"
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
        --connect="127.0.0.1:${P2P_PORT_BRIDGE}" \
        >> "${DATADIR_CSN}/daemon.log" 2>&1 &
    PID_CSN=$!
    wait_for_ready "${RPC_PORT_CSN}" "${DATADIR_CSN}" 30 || fail "CSN node failed to start"
}

echo ""
echo "================================================================="
echo "  CSN BRIDGE-ASSISTED SPEND FLOW TEST"
echo "================================================================="
echo ""

command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_spend_flow_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_spend_flow_XXXXXX)

info "[1/9] Starting bridge and CSN nodes"
start_bridge
start_csn
pass "Bridge and CSN nodes ready"

info "\n[2/9] Creating wallets and deriving addresses"
BRIDGE_WALLET=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.createhd" '"bridge_assisted_spend"')
CSN_WALLET=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.createhd" '"csn_spend_wallet"')
rpc_has_error "${BRIDGE_WALLET}" && fail "Bridge wallet creation failed"
rpc_has_error "${CSN_WALLET}" && fail "CSN wallet creation failed"
MINER_ADDRESS=$(echo "${BRIDGE_WALLET}" | jq -r '.result.first_address // empty')
CSN_RECEIVE_ADDRESS=$(echo "${CSN_WALLET}" | jq -r '.result.first_address // empty')
BRIDGE_RECIPIENT=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.getnewaddress" '.result.address // .result // empty')
[[ -n "${MINER_ADDRESS}" && -n "${CSN_RECEIVE_ADDRESS}" && -n "${BRIDGE_RECIPIENT}" ]] || fail "Failed to derive wallet addresses"
pass "Wallets created"

info "\n[3/9] Funding a CSN wallet-owned confirmed UTXO from the bridge"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${FUNDING_BLOCKS}" "${CSN_RECEIVE_ADDRESS}" \
    || fail "Failed to mine funding blocks"
HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to sync funded chain"
UTXO_JSON=$(wait_for_spendable_utxo "${RPC_PORT_CSN}" "${DATADIR_CSN}" 60) \
    || fail "CSN wallet never saw a spendable funded UTXO"
UTXO_TXID=$(echo "${UTXO_JSON}" | jq -r '.txid')
UTXO_VOUT=$(echo "${UTXO_JSON}" | jq -r '.vout')
[[ -n "${UTXO_TXID}" && -n "${UTXO_VOUT}" ]] || fail "Failed to parse funded UTXO"
pass "CSN funded UTXO available: ${UTXO_TXID:0:16}...:${UTXO_VOUT}"

BASE_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
BASE_ROOT=$(echo "${BASE_STATE}" | jq -r '.commitment')

info "\n[4/9] Making the cached root stale and requesting a bridge proof refresh"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${REFRESH_ADVANCE_BLOCKS}" "${MINER_ADDRESS}" \
    || fail "Failed to mine refresh-advance block(s)"
HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to sync refresh-advance block(s)"

PROOF_STATUS=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.proofstatus" "\"${BASE_ROOT}\"")
rpc_has_error "${PROOF_STATUS}" && fail "wallet.proofstatus failed"
[[ "$(echo "${PROOF_STATUS}" | jq -r '.result.stale')" == "true" ]] || fail "Expected stale proof root after tip advance"

PROOF_UPDATE_PARAMS=$(jq -nc \
    --arg root "${BASE_ROOT}" \
    --arg txid "${UTXO_TXID}" \
    --argjson vout "${UTXO_VOUT}" \
    '[{root_from:$root,outpoints:[{txid:$txid,vout:$vout}]}]')
PROOF_UPDATES=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "blockchain.getproofupdates" "${PROOF_UPDATE_PARAMS}")
rpc_has_error "${PROOF_UPDATES}" && fail "blockchain.getproofupdates failed: $(echo "${PROOF_UPDATES}" | tr -d '\n\t')"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.status')" == "updated" ]] || fail "Expected updated proof refresh status"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.root_from')" == "${BASE_ROOT}" ]] || fail "Proof update root_from mismatch"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.proofs | length')" == "1" ]] || fail "Expected exactly one refreshed proof"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '.result.proofs[0].success')" == "true" ]] || fail "Refreshed proof generation failed"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '(.result.stump_num_leaves // .result.num_leaves // 0)')" -ge 1 ]] || fail "Proof update missing stump_num_leaves"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '(.result.stump_roots // .result.roots // []) | length')" -ge 1 ]] || fail "Proof update missing stump_roots"
[[ "$(echo "${PROOF_UPDATES}" | jq -r '(.result.proofs[0].script_pubkey // "") | length')" -gt 0 ]] || fail "Refreshed proof missing script_pubkey"
pass "Bridge refreshed proof for selected spend outpoint"

info "\n[5/9] Verifying the refreshed proof against CSN chain context"
VERIFY_PARAMS=$(jq -nc \
    --arg txid "${UTXO_TXID}" \
    --argjson vout "${UTXO_VOUT}" \
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
rpc_has_error "${VERIFY_RESULT}" && fail "wallet.verifyutxoproof failed: $(echo "${VERIFY_RESULT}" | tr -d '\n\t')"
[[ "$(echo "${VERIFY_RESULT}" | jq -r '.result.valid')" == "true" ]] || fail "Refreshed proof did not verify on CSN"
pass "CSN accepted the bridge-refreshed proof"

PRE_SPEND_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")

info "\n[6/9] Spending the confirmed UTXO from the CSN wallet"
SEND_RESULT=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.sendtoaddress" "[\"${BRIDGE_RECIPIENT}\",1.0]")
rpc_has_error "${SEND_RESULT}" && fail "wallet.sendtoaddress failed: $(echo "${SEND_RESULT}" | tr -d '\n\t')"
SPEND_TXID=$(echo "${SEND_RESULT}" | jq -r '(.result.txid // .result // empty) | strings')
[[ -n "${SPEND_TXID}" ]] || fail "wallet.sendtoaddress returned empty txid"
wait_for_mempool_contains "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "${SPEND_TXID}" 30 \
    || fail "Bridge never observed the CSN spend in mempool"
wait_for_mempool_contains "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${SPEND_TXID}" 30 \
    || fail "CSN never retained its own spend in mempool"
assert_same_state "${PRE_SPEND_STATE}" "$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")" \
    "pending CSN spend"
pass "Pending spend left canonical Utreexo state unchanged"

info "\n[7/9] Confirming the spend on the bridge"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 1 "${MINER_ADDRESS}" \
    || fail "Failed to mine spend confirmation block"
HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to sync spend confirmation"
wait_for_confirmed_tx "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${SPEND_TXID}" 30 \
    || fail "CSN wallet never marked spend confirmed"
POST_CONFIRM_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
assert_state_advanced "${PRE_SPEND_STATE}" "${POST_CONFIRM_STATE}" "confirmed CSN spend"
pass "Confirmed spend advanced canonical Utreexo state"

info "\n[8/9] Ensuring the refreshed proof root is now stale after confirmation"
REFRESHED_ROOT=$(echo "${PROOF_UPDATES}" | jq -r '.result.root_to')
POST_CONFIRM_PROOF_STATUS=$(rpc_call "${RPC_PORT_CSN}" "${DATADIR_CSN}" "wallet.proofstatus" "\"${REFRESHED_ROOT}\"")
rpc_has_error "${POST_CONFIRM_PROOF_STATUS}" && fail "wallet.proofstatus failed after confirmation"
[[ "$(echo "${POST_CONFIRM_PROOF_STATUS}" | jq -r '.result.stale')" == "true" ]] || fail "Expected refreshed proof root to become stale after spend confirmation"
pass "Proof root staleness tracked across the spend lifecycle"

echo ""
echo "================================================================="
echo -e "${GREEN}  CSN BRIDGE-ASSISTED SPEND FLOW PASSED${NC}"
echo "================================================================="
echo ""
echo "Validated:"
echo "  - Bridge funded a CSN wallet-owned confirmed UTXO"
echo "  - Bridge re-proved the spend outpoint after a root change"
echo "  - CSN verified the refreshed proof against current context"
echo "  - Canonical Utreexo state stayed fixed while the spend was pending"
echo "  - Canonical Utreexo state advanced only when the spend confirmed"
echo ""

exit 0
