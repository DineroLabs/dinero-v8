#!/bin/bash
#
# Historical spend-block relay regression for bridge -> CSN sync.
#
# Reproduces the failing seam:
#   1. Bridge node syncs statefully and mines mature coinbase outputs.
#   2. Bridge creates a real spend block (first non-coinbase path).
#   3. Additional blocks make that spend block historical.
#   4. Fresh CSN connects and must sync past the historical spend block via utxoblk.
#

set -euo pipefail

SYNC_TIMEOUT=${TIMEOUT:-180}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Honour $DINEROD first (and require it to be executable); the chain
# below never consulted it, so it CLOBBERED the caller's choice and an
# arbitrary build directory could not be used.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
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
EXIT_CODE=0

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    [[ -n "$DATADIR_BRIDGE" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    [[ -n "$DATADIR_CSN" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    sleep 1
    if [[ $EXIT_CODE -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 60 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -60 "${DATADIR_BRIDGE}/daemon.log"
        echo -e "\n${RED}=== CSN daemon.log (last 60 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -60 "${DATADIR_CSN}/daemon.log"
        if [[ "$KEEP_TMP_ON_FAIL" == "1" ]]; then
            echo -e "\n${YELLOW}Keeping temp dirs for debugging:${NC}"
            [[ -n "$DATADIR_BRIDGE" ]] && echo "  Bridge: ${DATADIR_BRIDGE}"
            [[ -n "$DATADIR_CSN" ]] && echo "  CSN:    ${DATADIR_CSN}"
            return
        fi
    fi
    [[ -n "$DATADIR_BRIDGE" && -d "$DATADIR_BRIDGE" ]] && rm -rf "$DATADIR_BRIDGE"
    [[ -n "$DATADIR_CSN" && -d "$DATADIR_CSN" ]] && rm -rf "$DATADIR_CSN"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }

rpc_call() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local params="$*"
    local cookie
    cookie=$(cat "${datadir}/.cookie" 2>/dev/null || true)
    [[ -z "$cookie" ]] && return 1
    local json_params="[]"
    if [[ -n "$params" ]]; then
        if [[ "$params" == \[*\] ]]; then
            json_params="$params"
        else
            json_params="[$params]"
        fi
    fi
    curl -s -u "$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$json_params,\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

rpc_has_error() {
    local compact
    compact=$(echo "$1" | tr -d '\n\t ')
    [[ "$compact" == *"\"error\":null"* ]] && return 1
    [[ "$compact" == *"\"error\":"* ]] && return 0
    return 1
}

rpc_scalar() {
    local port=$1
    local datadir=$2
    local method=$3
    local jq_expr=$4
    shift 4
    local response
    response=$(rpc_call "$port" "$datadir" "$method" "$@")
    if rpc_has_error "$response"; then
        echo ""
        return 1
    fi
    echo "$response" | jq -r "$jq_expr // empty"
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt $timeout ]] && return 1
        if [[ -f "${datadir}/.cookie" ]]; then
            local height
            height=$(rpc_scalar "$port" "$datadir" "getblockcount" '.result' 2>/dev/null || true)
            [[ -n "$height" ]] && return 0
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
        [[ $(($(date +%s) - start)) -gt $timeout ]] && return 1
        local height hash
        height=$(rpc_scalar "$port" "$datadir" "getblockcount" '.result' 2>/dev/null || true)
        hash=$(rpc_scalar "$port" "$datadir" "getbestblockhash" '.result' 2>/dev/null || true)
        [[ "$height" == "$target_height" && "$hash" == "$target_hash" ]] && return 0
        sleep 0.5
    done
}

mine_blocks_to_address() {
    local port=$1
    local datadir=$2
    local total=$3
    local address=$4
    local result
    result=$(rpc_call "$port" "$datadir" "generatetoaddress" "$total, \"$address\"")
    rpc_has_error "$result" && return 1
    return 0
}

echo ""
echo "================================================================="
echo "  Bridge <-> CSN Historical Spend Relay Test"
echo "================================================================="
echo ""

RPC_PORT_BRIDGE=$((28000 + RANDOM % 1000))
P2P_PORT_BRIDGE=$((RPC_PORT_BRIDGE + 1))
RPC_PORT_CSN=$((RPC_PORT_BRIDGE + 2))
P2P_PORT_CSN=$((RPC_PORT_BRIDGE + 3))

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_hist_spend_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_hist_spend_XXXXXX)

info "[STEP 1] Starting bridge node..."
"$DINEROD" --regtest \
    --datadir="$DATADIR_BRIDGE" \
    --rpcport="$RPC_PORT_BRIDGE" \
    --port="$P2P_PORT_BRIDGE" \
    --listen=1 \
    --connect="127.0.0.1:$P2P_PORT_CSN" \
    --utreexo=1 \
    --utreexo-bridge=1 \
    >> "${DATADIR_BRIDGE}/daemon.log" 2>&1 &

wait_for_ready "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" 30 || fail "Bridge node failed to start"
pass "Bridge node ready"

info "\n[STEP 2] Mining mature coinbase outputs..."
WALLET_RESULT=$(rpc_call "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "wallet.createhd" '"bridge_hist_spend"')
MINER_ADDRESS=$(echo "$WALLET_RESULT" | jq -r '.result.first_address // empty')
[[ -n "$MINER_ADDRESS" ]] || fail "Failed to create bridge wallet"

mine_blocks_to_address "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" 110 "$MINER_ADDRESS" || fail "Failed to mine maturity blocks"
pass "Bridge mined 110 blocks"

info "\n[STEP 3] Creating a real spend block..."
RECIPIENT_RESULT=$(rpc_call "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "wallet.getnewaddress")
RECIPIENT_ADDRESS=$(echo "$RECIPIENT_RESULT" | jq -r '.result.address // .result // empty')
[[ -n "$RECIPIENT_ADDRESS" ]] || fail "Failed to derive recipient address"

SEND_RESULT=$(rpc_call "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "wallet.sendtoaddress" "[\"$RECIPIENT_ADDRESS\",1.0]")
rpc_has_error "$SEND_RESULT" && fail "wallet.sendtoaddress failed: $(echo "$SEND_RESULT" | tr -d '\n\t')"
SPEND_TXID=$(echo "$SEND_RESULT" | jq -r '(.result.txid // .result // empty) | strings')
[[ -n "$SPEND_TXID" ]] || fail "wallet.sendtoaddress returned empty txid"

mine_blocks_to_address "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" 1 "$MINER_ADDRESS" || fail "Failed to mine spend block"
SPEND_BLOCK_HEIGHT=$(rpc_scalar "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "getblockcount" '.result')
[[ -n "$SPEND_BLOCK_HEIGHT" ]] || fail "Failed to read spend block height"
pass "Spend tx ${SPEND_TXID:0:16}... confirmed at height ${SPEND_BLOCK_HEIGHT}"

info "\n[STEP 4] Advancing chain so spend block becomes historical..."
mine_blocks_to_address "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" 10 "$MINER_ADDRESS" || fail "Failed to mine trailing blocks"
HEIGHT_BRIDGE=$(rpc_scalar "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "getbestblockhash" '.result')
[[ -n "$HEIGHT_BRIDGE" && -n "$TIP_BRIDGE" ]] || fail "Failed to read bridge tip"
pass "Bridge tip advanced to height ${HEIGHT_BRIDGE}"

info "\n[STEP 5] Starting fresh CSN node..."
"$DINEROD" --regtest \
    --datadir="$DATADIR_CSN" \
    --rpcport="$RPC_PORT_CSN" \
    --port="$P2P_PORT_CSN" \
    --listen=1 \
    --utreexo=1 \
    --utreexo-stateless=1 \
    --connect="127.0.0.1:$P2P_PORT_BRIDGE" \
    >> "${DATADIR_CSN}/daemon.log" 2>&1 &

wait_for_ready "$RPC_PORT_CSN" "$DATADIR_CSN" 30 || fail "CSN node failed to start"
pass "CSN node ready"

info "\n[STEP 6] Waiting for CSN to sync through historical spend block..."
if ! wait_for_sync "$RPC_PORT_CSN" "$DATADIR_CSN" "$HEIGHT_BRIDGE" "$TIP_BRIDGE" "$SYNC_TIMEOUT"; then
    CSN_HEIGHT=$(rpc_scalar "$RPC_PORT_CSN" "$DATADIR_CSN" "getblockcount" '.result' || true)
    fail "CSN sync timeout (height=${CSN_HEIGHT:-unknown}, spend_height=${SPEND_BLOCK_HEIGHT})"
fi
pass "CSN synced to bridge tip ${HEIGHT_BRIDGE}"

info "\n[STEP 7] Verifying proof relay markers..."
CSN_PROOF_HITS=$(cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null | grep -cE "validated with (transition|batch) proof" || true)
BRIDGE_SERVE_HITS=$(cat "${DATADIR_BRIDGE}/daemon.log" "${DATADIR_BRIDGE}/p2p.log" 2>/dev/null | grep -c "Sent utreexo block" || true)
if cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null | grep -q "missing-utreexo-data"; then
    fail "CSN replay hit missing-utreexo-data during historical spend sync"
fi
[[ ${BRIDGE_SERVE_HITS:-0} -gt 0 ]] || fail "Bridge never served utxoblk during historical spend sync"
pass "Bridge served utxoblk ${BRIDGE_SERVE_HITS} time(s)"
if [[ ${CSN_PROOF_HITS:-0} -gt 0 ]]; then
    pass "CSN validated ${CSN_PROOF_HITS} block(s) with proofs"
fi

echo ""
echo "================================================================="
echo -e "${GREEN}  HISTORICAL SPEND RELAY TEST PASSED${NC}"
echo "================================================================="
echo ""
echo "Validated:"
echo "  - Bridge mined a real spend block at height ${SPEND_BLOCK_HEIGHT}"
echo "  - Spend block became historical before CSN joined"
echo "  - Fresh CSN synced through the historical spend block"
echo "  - Bridge served utxoblk for historical sync"
echo ""

exit 0
