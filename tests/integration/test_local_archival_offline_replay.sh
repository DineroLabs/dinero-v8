#!/usr/bin/env bash
#
# Opt-in local offline archival replay canary.
#
# Replays blockNNN.hex exports through submitblock on a brand-new node started
# with p2p.offline=1 so the live acceptance/connect path is exercised without
# any peer help. Utreexo commitment availability is part of the success signal.
#
# Enable explicitly with:
#   RUN_LOCAL_ARCHIVAL_REPLAY_CANARY=1
#

set -euo pipefail

if [[ "${RUN_LOCAL_ARCHIVAL_REPLAY_CANARY:-0}" != "1" ]]; then
    echo "SKIP: set RUN_LOCAL_ARCHIVAL_REPLAY_CANARY=1 to run the local offline archival replay canary"
    exit 0
fi

ARCHIVE_DIR="${ARCHIVE_DIR:-}"
START_HEIGHT="${START_HEIGHT:-}"
END_HEIGHT="${END_HEIGHT:-}"
EXPECTED_HEIGHT="${EXPECTED_HEIGHT:-}"
EXPECTED_HASH="${EXPECTED_HASH:-}"
EXPECTED_UTREEXO_COMMITMENT="${EXPECTED_UTREEXO_COMMITMENT:-}"
KEEP_TMP_ON_FAIL="${KEEP_TMP_ON_FAIL:-1}"

if [[ -z "${ARCHIVE_DIR}" ]]; then
    echo "ARCHIVE_DIR is required"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DINEROD="${DINEROD:-${PROJECT_ROOT}/build/dinerod}"
REPLAY_SCRIPT="${PROJECT_ROOT}/scripts/offline_replay_archive.py"

[[ -x "${DINEROD}" ]] || { echo "dinerod not found at ${DINEROD}"; exit 1; }
[[ -f "${REPLAY_SCRIPT}" ]] || { echo "offline replay script missing at ${REPLAY_SCRIPT}"; exit 1; }
command -v curl >/dev/null || { echo "curl is required"; exit 1; }
command -v jq >/dev/null || { echo "jq is required"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required"; exit 1; }

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${CYAN}$1${NC}"; }
pass() { echo -e "${GREEN}$1${NC}"; }
fail() { echo -e "${RED}FAILED: $1${NC}" >&2; exit 1; }

DATADIR="$(mktemp -d -t dinero_offline_replay_XXXXXX)"
LOGFILE="${DATADIR}/daemon.log"
REPORT="${DATADIR}/offline-replay-report.json"
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT=$(alloc_port_base)
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
PID=""
EXIT_CODE=0

cleanup() {
    if [[ -n "${PID}" ]]; then
        kill "${PID}" 2>/dev/null || true
        wait "${PID}" 2>/dev/null || true
    fi
    pkill -f "dinerod.*${DATADIR}" 2>/dev/null || true
    if [[ ${EXIT_CODE} -ne 0 ]]; then
        echo -e "\n${YELLOW}=== daemon.log tail ===${NC}"
        [[ -f "${LOGFILE}" ]] && tail -120 "${LOGFILE}" || true
        if [[ "${KEEP_TMP_ON_FAIL}" == "1" ]]; then
            echo -e "${YELLOW}Keeping temp datadir: ${DATADIR}${NC}"
            return
        fi
    fi
    rm -rf "${DATADIR}"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    local cookie
    cookie="$(tr -d '\n' < "${DATADIR}/.cookie" 2>/dev/null || true)"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

wait_rpc() {
    for _ in $(seq 1 60); do
        if rpc_call "getblockcount" '[]' | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

info "[1/4] Starting zero-peer archival replay node"
"${DINEROD}" \
    --datadir="${DATADIR}" \
    --rpcport="${RPC_PORT}" \
    --p2pport="${P2P_PORT}" \
    --wallet-socket-port="${WALLET_PORT}" \
    --server=1 \
    --listen=0 \
    --archival=1 \
    --utreexo=1 \
    --p2p.offline=1 \
    >"${LOGFILE}" 2>&1 &
PID="$!"

wait_rpc || fail "offline node RPC did not come up"
pass "offline node ready"

CONN="$(rpc_call "getconnectioncount" '[]' | jq -r '.result // -1')"
[[ "${CONN}" == "0" ]] || fail "expected zero peers before replay, got ${CONN}"
pass "node starts with zero peers"

info "\n[2/4] Replaying archived blocks through submitblock"
CMD=(
    python3 "${REPLAY_SCRIPT}"
    --archive-dir "${ARCHIVE_DIR}"
    --datadir "${DATADIR}"
    --rpc-port "${RPC_PORT}"
    --require-zero-peers
    --json-report "${REPORT}"
)
[[ -n "${START_HEIGHT}" ]] && CMD+=(--start-height "${START_HEIGHT}")
[[ -n "${END_HEIGHT}" ]] && CMD+=(--end-height "${END_HEIGHT}")
[[ -n "${EXPECTED_HEIGHT}" ]] && CMD+=(--expected-final-height "${EXPECTED_HEIGHT}")
[[ -n "${EXPECTED_HASH}" ]] && CMD+=(--expected-final-hash "${EXPECTED_HASH}")
[[ -n "${EXPECTED_UTREEXO_COMMITMENT}" ]] && CMD+=(--expected-utreexo-commitment "${EXPECTED_UTREEXO_COMMITMENT}")
"${CMD[@]}"
pass "offline replay completed"

info "\n[3/4] Verifying final chain and Utreexo state"
CHAIN_INFO="$(rpc_call "getblockchaininfo" '[]')"
UTREEXO_INFO="$(rpc_call "blockchain.getutreexocommitment" '[]')"
FINAL_CONN="$(rpc_call "getconnectioncount" '[]' | jq -r '.result // -1')"

[[ "${FINAL_CONN}" == "0" ]] || fail "expected zero peers after replay, got ${FINAL_CONN}"
jq -e '.result.blocks >= 0 and (.result.bestblockhash | length) == 64' <<<"${CHAIN_INFO}" >/dev/null \
    || fail "invalid getblockchaininfo response: ${CHAIN_INFO}"
jq -e '.result.commitment != null and (.result.commitment | length) == 64' <<<"${UTREEXO_INFO}" >/dev/null \
    || fail "invalid getutreexocommitment response: ${UTREEXO_INFO}"
pass "final chain/Utreexo RPCs look healthy"

info "\n[4/4] Replay report"
jq -C . "${REPORT}" || fail "missing replay report at ${REPORT}"
pass "LOCAL_ARCHIVAL_OFFLINE_REPLAY=PASS"
