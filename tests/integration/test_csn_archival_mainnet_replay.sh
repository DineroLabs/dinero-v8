#!/usr/bin/env bash
#
# Opt-in external archival mainnet replay soak.
#
# Starts a brand-new mainnet stateless CSN against live archival bridges and
# requires it to replay the full available historical range from genesis to the
# current bridge tip via utxoblk. The final tip hash and stump state must match
# the bridges exactly, with no proof failures or missing proof data.
#
# This is intentionally not a default local/CI gate. Enable explicitly with:
#   RUN_ARCHIVAL_MAINNET_SOAK=1
#

set -euo pipefail

if [[ "${RUN_ARCHIVAL_MAINNET_SOAK:-0}" != "1" ]]; then
    echo "SKIP: set RUN_ARCHIVAL_MAINNET_SOAK=1 to run the external archival mainnet replay"
    exit 0
fi

SYNC_TIMEOUT=${TIMEOUT:-5400}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}
BRIDGE_RPC_USER=${BRIDGE_RPC_USER:-}
BRIDGE_RPC_PASSWORD=${BRIDGE_RPC_PASSWORD:-}
PRIMARY_BRIDGE_HOST=${PRIMARY_BRIDGE_HOST:-173.249.195.59}
SECONDARY_BRIDGE_HOST=${SECONDARY_BRIDGE_HOST:-172.93.160.131}
BRIDGE_RPC_PORT=${BRIDGE_RPC_PORT:-20998}
BRIDGE_P2P_PORT=${BRIDGE_P2P_PORT:-20999}

if [[ -z "${BRIDGE_RPC_USER}" || -z "${BRIDGE_RPC_PASSWORD}" ]]; then
    echo "BRIDGE_RPC_USER and BRIDGE_RPC_PASSWORD are required for archival replay"
    exit 1
fi

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

DATADIR_CSN=""
PID_CSN=""
EXIT_CODE=0

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    if [[ -n "${PID_CSN}" ]]; then
        kill "${PID_CSN}" 2>/dev/null || true
        wait "${PID_CSN}" 2>/dev/null || true
    fi
    [[ -n "${DATADIR_CSN}" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    sleep 1
    if [[ ${EXIT_CODE} -ne 0 ]]; then
        echo -e "\n${RED}=== CSN daemon.log (last 160 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -160 "${DATADIR_CSN}/daemon.log"
        if [[ "${KEEP_TMP_ON_FAIL}" == "1" ]]; then
            echo -e "\n${YELLOW}Keeping temp dir for debugging:${NC}"
            [[ -n "${DATADIR_CSN}" ]] && echo "  CSN: ${DATADIR_CSN}"
            return
        fi
    fi
    [[ -n "${DATADIR_CSN}" && -d "${DATADIR_CSN}" ]] && rm -rf "${DATADIR_CSN}"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

LOCAL_RPC_PORT=$((37000 + RANDOM % 1000))
LOCAL_P2P_PORT=$((LOCAL_RPC_PORT + 1))
LOCAL_WALLET_PORT=$((LOCAL_RPC_PORT + 2))

remote_rpc_call() {
    local host=$1
    local method=$2
    shift 2
    local params="${1:-[]}"
    curl -s --connect-timeout 5 --max-time 30 \
        -u "${BRIDGE_RPC_USER}:${BRIDGE_RPC_PASSWORD}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}" \
        "http://${host}:${BRIDGE_RPC_PORT}/" 2>/dev/null
}

local_rpc_call() {
    local method=$1
    shift
    local params="${1:-[]}"
    local cookie
    cookie=$(cat "${DATADIR_CSN}/.cookie" 2>/dev/null || true)
    [[ -z "${cookie}" ]] && return 1
    curl -s --connect-timeout 2 --max-time 30 \
        -u "${cookie}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}" \
        "http://127.0.0.1:${LOCAL_RPC_PORT}/" 2>/dev/null
}

rpc_has_error() {
    local compact
    compact=$(echo "$1" | tr -d '\n\t ')
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

require_rpc_success() {
    local response=$1
    local label=$2
    rpc_has_error "${response}" && fail "${label}: $(echo "${response}" | tr '\n' ' ')"
}

wait_for_ready() {
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt 60 ]] && return 1
        if [[ -f "${DATADIR_CSN}/.cookie" ]]; then
            local response
            response=$(local_rpc_call "getblockcount" "[]" || true)
            if [[ -n "${response}" ]] && ! rpc_has_error "${response}"; then
                return 0
            fi
        fi
        sleep 1
    done
}

capture_remote_state() {
    local host=$1
    local height hash commitment roots

    height=$(remote_rpc_call "${host}" "getblockcount" "[]")
    require_rpc_success "${height}" "remote getblockcount ${host}"

    hash=$(remote_rpc_call "${host}" "getbestblockhash" "[]")
    require_rpc_success "${hash}" "remote getbestblockhash ${host}"

    commitment=$(remote_rpc_call "${host}" "blockchain.getutreexocommitment" "[]")
    require_rpc_success "${commitment}" "remote getutreexocommitment ${host}"

    roots=$(remote_rpc_call "${host}" "blockchain.getutreexoroots" "[]")
    require_rpc_success "${roots}" "remote getutreexoroots ${host}"

    jq -n \
        --arg host "${host}" \
        --argjson height "$(echo "${height}" | jq -r '.result')" \
        --arg hash "$(echo "${hash}" | jq -r '.result')" \
        --arg commitment "$(echo "${commitment}" | jq -r '.result.commitment // empty')" \
        --argjson num_leaves "$(echo "${commitment}" | jq -r '.result.num_leaves // 0')" \
        --argjson num_roots "$(echo "${commitment}" | jq -r '.result.num_roots // 0')" \
        --argjson roots "$(echo "${roots}" | jq -c '.result.roots // []')" \
        '{
            host: $host,
            height: $height,
            hash: $hash,
            commitment: $commitment,
            num_leaves: $num_leaves,
            num_roots: $num_roots,
            roots: $roots
        }'
}

capture_local_state() {
    local height hash commitment roots

    height=$(local_rpc_call "getblockcount" "[]")
    require_rpc_success "${height}" "local getblockcount"

    hash=$(local_rpc_call "getbestblockhash" "[]")
    require_rpc_success "${hash}" "local getbestblockhash"

    commitment=$(local_rpc_call "blockchain.getutreexocommitment" "[]")
    require_rpc_success "${commitment}" "local getutreexocommitment"

    roots=$(local_rpc_call "blockchain.getutreexoroots" "[]")
    require_rpc_success "${roots}" "local getutreexoroots"

    jq -n \
        --argjson height "$(echo "${height}" | jq -r '.result')" \
        --arg hash "$(echo "${hash}" | jq -r '.result')" \
        --arg commitment "$(echo "${commitment}" | jq -r '.result.commitment // empty')" \
        --argjson num_leaves "$(echo "${commitment}" | jq -r '.result.num_leaves // 0')" \
        --argjson num_roots "$(echo "${commitment}" | jq -r '.result.num_roots // 0')" \
        --argjson roots "$(echo "${roots}" | jq -c '.result.roots // []')" \
        '{
            height: $height,
            hash: $hash,
            commitment: $commitment,
            num_leaves: $num_leaves,
            num_roots: $num_roots,
            roots: $roots
        }'
}

assert_same_state() {
    local lhs_json=$1
    local rhs_json=$2
    local lhs_label=$3
    local rhs_label=$4
    local equal

    equal=$(jq -n \
        --argjson lhs "${lhs_json}" \
        --argjson rhs "${rhs_json}" \
        '($lhs.height == $rhs.height)
         and ($lhs.hash == $rhs.hash)
         and ($lhs.commitment == $rhs.commitment)
         and ($lhs.num_leaves == $rhs.num_leaves)
         and ($lhs.num_roots == $rhs.num_roots)
         and ($lhs.roots == $rhs.roots)')

    [[ "${equal}" == "true" ]] || {
        echo "${lhs_label} = $(echo "${lhs_json}" | jq -c '.')"
        echo "${rhs_label} = $(echo "${rhs_json}" | jq -c '.')"
        fail "state mismatch between ${lhs_label} and ${rhs_label}"
    }
}

echo ""
echo "================================================================="
echo "  CSN Archived Mainnet Replay Soak"
echo "================================================================="
echo "  primary bridge:   ${PRIMARY_BRIDGE_HOST}:${BRIDGE_P2P_PORT}"
echo "  secondary bridge: ${SECONDARY_BRIDGE_HOST}:${BRIDGE_P2P_PORT}"
echo "  sync timeout:     ${SYNC_TIMEOUT} s"
echo "================================================================="
echo ""

info "[1/5] Querying archival bridges for reference tip and stump state"
PRIMARY_STATE=$(capture_remote_state "${PRIMARY_BRIDGE_HOST}")
SECONDARY_STATE=$(capture_remote_state "${SECONDARY_BRIDGE_HOST}")
assert_same_state "${PRIMARY_STATE}" "${SECONDARY_STATE}" "primary bridge" "secondary bridge"

TARGET_HEIGHT=$(echo "${PRIMARY_STATE}" | jq -r '.height')
TARGET_HASH=$(echo "${PRIMARY_STATE}" | jq -r '.hash')
TARGET_COMMITMENT=$(echo "${PRIMARY_STATE}" | jq -r '.commitment')
pass "Both archival bridges agree at height ${TARGET_HEIGHT} hash ${TARGET_HASH:0:16}..."

DATADIR_CSN=$(mktemp -d -t dinero_csn_archival_mainnet_XXXXXX)

info "\n[2/5] Starting fresh mainnet stateless CSN"
"${DINEROD}" \
    --datadir="${DATADIR_CSN}" \
    --rpcport="${LOCAL_RPC_PORT}" \
    --port="${LOCAL_P2P_PORT}" \
    --wallet-socket-port="${LOCAL_WALLET_PORT}" \
    --listen=1 \
    --utreexo=1 \
    --utreexo-stateless=1 \
    --addnode="${PRIMARY_BRIDGE_HOST}:${BRIDGE_P2P_PORT}" \
    --addnode="${SECONDARY_BRIDGE_HOST}:${BRIDGE_P2P_PORT}" \
    >> "${DATADIR_CSN}/daemon.log" 2>&1 &
PID_CSN=$!

wait_for_ready || fail "fresh mainnet CSN failed to start"
pass "Fresh CSN node ready"

info "\n[3/5] Waiting for full genesis-to-tip proof replay"
START_TIME=$(date +%s)
LAST_PROGRESS_TIME=${START_TIME}
LAST_HEIGHT=-1

while true; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - START_TIME))
    [[ ${ELAPSED} -gt ${SYNC_TIMEOUT} ]] && fail "timed out waiting for archival mainnet replay"

    LOCAL_HEIGHT_RESPONSE=$(local_rpc_call "getblockcount" "[]" || true)
    if [[ -z "${LOCAL_HEIGHT_RESPONSE}" ]] || rpc_has_error "${LOCAL_HEIGHT_RESPONSE}"; then
        sleep 2
        continue
    fi
    LOCAL_HEIGHT=$(echo "${LOCAL_HEIGHT_RESPONSE}" | jq -r '.result')

    if [[ "${LOCAL_HEIGHT}" != "${LAST_HEIGHT}" ]]; then
        LAST_HEIGHT=${LOCAL_HEIGHT}
        LAST_PROGRESS_TIME=${NOW}
    fi

    if (( (NOW - LAST_PROGRESS_TIME) > 300 )); then
        fail "no sync progress for 300s during archival replay (stuck at height ${LOCAL_HEIGHT})"
    fi

    if (( ELAPSED % 30 == 0 )); then
        echo "  progress: height=${LOCAL_HEIGHT}/${TARGET_HEIGHT} elapsed=${ELAPSED}s"
    fi

    LOCAL_HASH_RESPONSE=$(local_rpc_call "getbestblockhash" "[]" || true)
    if [[ -n "${LOCAL_HASH_RESPONSE}" ]] && ! rpc_has_error "${LOCAL_HASH_RESPONSE}"; then
        LOCAL_HASH=$(echo "${LOCAL_HASH_RESPONSE}" | jq -r '.result')
        if [[ "${LOCAL_HEIGHT}" == "${TARGET_HEIGHT}" && "${LOCAL_HASH}" == "${TARGET_HASH}" ]]; then
            break
        fi
    fi

    sleep 2
done
pass "Fresh CSN reached live archival bridge tip"

info "\n[4/5] Verifying exact final stump state"
LOCAL_STATE=$(capture_local_state)
assert_same_state "${PRIMARY_STATE}" "${LOCAL_STATE}" "bridge reference" "fresh CSN"
pass "Fresh CSN stump matches archival bridges exactly"

info "\n[5/5] Checking proof-plane logs"
grep -q "Routing utxoblk" "${DATADIR_CSN}/daemon.log" || fail "CSN never routed utxoblk during archival replay"
! grep -q "FAIL step" "${DATADIR_CSN}/daemon.log" || fail "proof validation step failure detected during archival replay"
! grep -q "Invalid utreexo proof" "${DATADIR_CSN}/daemon.log" || fail "invalid proof detected during archival replay"
! grep -q "missing-utreexo-data" "${DATADIR_CSN}/daemon.log" || fail "bridge reported missing proof data during archival replay"
! grep -q "NOTFOUND for" "${DATADIR_CSN}/daemon.log" || fail "bridge returned NOTFOUND during archival replay"

UTXOBLK_COUNT=$(grep -c "Routing utxoblk" "${DATADIR_CSN}/daemon.log" || true)
PROOF_OK_COUNT=$(grep -c "\\[StatelessNode\\] Proof OK" "${DATADIR_CSN}/daemon.log" || true)
[[ ${UTXOBLK_COUNT} -ge $((TARGET_HEIGHT - 10)) ]] || fail "too few utxoblk deliveries (${UTXOBLK_COUNT} for height ${TARGET_HEIGHT})"
[[ ${PROOF_OK_COUNT} -ge $((TARGET_HEIGHT - 10)) ]] || fail "too few proof validations (${PROOF_OK_COUNT} for height ${TARGET_HEIGHT})"
pass "Observed ${UTXOBLK_COUNT} utxoblk deliveries and ${PROOF_OK_COUNT} proof validations"

echo ""
echo "================================================================="
echo -e "${GREEN}  ARCHIVAL MAINNET REPLAY PASSED${NC}"
echo "================================================================="
echo ""
echo "Validated:"
echo "  - Fresh mainnet CSN replayed the full available archived chain via live utxoblk proof relay"
echo "  - Final tip hash matched both archival bridges: ${TARGET_HASH}"
echo "  - Final stump commitment matched both archival bridges: ${TARGET_COMMITMENT}"
echo "  - No NOTFOUND, missing-utreexo-data, or proof failures occurred"
echo ""

exit 0
