#!/usr/bin/env bash
#
# IBD Torture Harness (regtest)
#
# Deterministic adversarial IBD test that stresses:
# - competing forks during sync
# - staged block availability / delayed peer exposure
# - intermittent peer disconnect churn
# - restart mid-sync (checkpoint/restore style)
# - eventual convergence by tip hash + chainwork
#
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
if [[ ! -x "${DINEROD}" ]]; then
    echo -e "${RED}FAIL:${NC} dinerod binary not found/executable: ${DINEROD}" >&2
    exit 1
fi

for cmd in curl jq python3; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo -e "${RED}FAIL:${NC} requires ${cmd}" >&2
        exit 1
    fi
done

usage() {
    cat <<'USAGE'
Usage: tests/test_ibd_torture.sh [--profile strict|release|smoke]

Profiles:
  strict   Full deterministic stress profile (default)
  release  Heavy release-gate profile (faster than strict, still adversarial)
  smoke    Quick local verification profile

Any profile value can be overridden via env vars, e.g.:
  SHARED_BASE_BLOCKS=500 tests/test_ibd_torture.sh --profile release
USAGE
}

PROFILE="${PROFILE:-strict}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)
            if [[ $# -lt 2 ]]; then
                echo -e "${RED}FAIL:${NC} --profile requires a value" >&2
                exit 2
            fi
            PROFILE="$2"
            shift 2
            ;;
        --profile=*)
            PROFILE="${1#*=}"
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}FAIL:${NC} unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

case "${PROFILE}" in
    strict)
        D_SHARED_BASE_BLOCKS=600
        D_SOURCE_PRE_BLOCKS=1800
        D_FORK_PRE_BLOCKS=2100
        D_SOURCE_WITHHELD_BLOCKS=120
        D_FORK_WITHHELD_BLOCKS=120
        D_SOURCE_POST_BLOCKS=500
        D_SOURCE_WIN_STEP=50
        D_SOURCE_WIN_MARGIN=5
        D_IBD_PROGRESS_TARGET=400
        D_RPC_READY_TIMEOUT=60
        D_PROGRESS_TIMEOUT=480
        D_CONVERGE_TIMEOUT=900
        D_NO_PROGRESS_TIMEOUT=45
        D_PROGRESS_EXTENSION=30
        D_MAX_PROGRESS_TIMEOUT=1800
        D_CHURN_LOOPS=8
        D_CHURN_INTERVAL=3
        D_CHURN_RESTART_PAUSE=1
        ;;
    release)
        D_SHARED_BASE_BLOCKS=240
        D_SOURCE_PRE_BLOCKS=700
        D_FORK_PRE_BLOCKS=850
        D_SOURCE_WITHHELD_BLOCKS=60
        D_FORK_WITHHELD_BLOCKS=60
        D_SOURCE_POST_BLOCKS=250
        D_SOURCE_WIN_STEP=50
        D_SOURCE_WIN_MARGIN=5
        D_IBD_PROGRESS_TARGET=180
        D_RPC_READY_TIMEOUT=60
        D_PROGRESS_TIMEOUT=900
        D_CONVERGE_TIMEOUT=1200
        D_NO_PROGRESS_TIMEOUT=45
        D_PROGRESS_EXTENSION=30
        D_MAX_PROGRESS_TIMEOUT=1800
        D_CHURN_LOOPS=6
        D_CHURN_INTERVAL=2
        D_CHURN_RESTART_PAUSE=1
        ;;
    smoke)
        D_SHARED_BASE_BLOCKS=60
        D_SOURCE_PRE_BLOCKS=100
        D_FORK_PRE_BLOCKS=120
        D_SOURCE_WITHHELD_BLOCKS=8
        D_FORK_WITHHELD_BLOCKS=8
        D_SOURCE_POST_BLOCKS=40
        D_SOURCE_WIN_STEP=20
        D_SOURCE_WIN_MARGIN=3
        D_IBD_PROGRESS_TARGET=30
        D_RPC_READY_TIMEOUT=45
        D_PROGRESS_TIMEOUT=300
        D_CONVERGE_TIMEOUT=300
        D_NO_PROGRESS_TIMEOUT=30
        D_PROGRESS_EXTENSION=20
        D_MAX_PROGRESS_TIMEOUT=900
        D_CHURN_LOOPS=1
        D_CHURN_INTERVAL=1
        D_CHURN_RESTART_PAUSE=1
        ;;
    *)
        echo -e "${RED}FAIL:${NC} unsupported profile: ${PROFILE}" >&2
        usage
        exit 2
        ;;
esac

# Tunables
SHARED_BASE_BLOCKS="${SHARED_BASE_BLOCKS:-${D_SHARED_BASE_BLOCKS}}"
SOURCE_PRE_BLOCKS="${SOURCE_PRE_BLOCKS:-${D_SOURCE_PRE_BLOCKS}}"
FORK_PRE_BLOCKS="${FORK_PRE_BLOCKS:-${D_FORK_PRE_BLOCKS}}"
SOURCE_WITHHELD_BLOCKS="${SOURCE_WITHHELD_BLOCKS:-${D_SOURCE_WITHHELD_BLOCKS}}"
FORK_WITHHELD_BLOCKS="${FORK_WITHHELD_BLOCKS:-${D_FORK_WITHHELD_BLOCKS}}"
SOURCE_POST_BLOCKS="${SOURCE_POST_BLOCKS:-${D_SOURCE_POST_BLOCKS}}"
SOURCE_WIN_STEP="${SOURCE_WIN_STEP:-${D_SOURCE_WIN_STEP}}"
SOURCE_WIN_MARGIN="${SOURCE_WIN_MARGIN:-${D_SOURCE_WIN_MARGIN}}"

IBD_PROGRESS_TARGET="${IBD_PROGRESS_TARGET:-${D_IBD_PROGRESS_TARGET}}"
RPC_READY_TIMEOUT="${RPC_READY_TIMEOUT:-${D_RPC_READY_TIMEOUT}}"
PROGRESS_TIMEOUT="${PROGRESS_TIMEOUT:-${D_PROGRESS_TIMEOUT}}"
CONVERGE_TIMEOUT="${CONVERGE_TIMEOUT:-${D_CONVERGE_TIMEOUT}}"
NO_PROGRESS_TIMEOUT="${NO_PROGRESS_TIMEOUT:-${D_NO_PROGRESS_TIMEOUT}}"
PROGRESS_EXTENSION="${PROGRESS_EXTENSION:-${D_PROGRESS_EXTENSION}}"
MAX_PROGRESS_TIMEOUT="${MAX_PROGRESS_TIMEOUT:-${D_MAX_PROGRESS_TIMEOUT}}"

CHURN_LOOPS="${CHURN_LOOPS:-${D_CHURN_LOOPS}}"
CHURN_INTERVAL="${CHURN_INTERVAL:-${D_CHURN_INTERVAL}}"
CHURN_RESTART_PAUSE="${CHURN_RESTART_PAUSE:-${D_CHURN_RESTART_PAUSE}}"
MINE_BATCH_SIZE="${MINE_BATCH_SIZE:-500}"
PHASE1_START_TIMEOUT="${PHASE1_START_TIMEOUT:-180}"
PHASE1_NO_PROGRESS_TIMEOUT="${PHASE1_NO_PROGRESS_TIMEOUT:-120}"
AUTO_PORTS="${AUTO_PORTS:-1}"
PORT_PICK_MIN="${PORT_PICK_MIN:-19000}"
PORT_PICK_MAX="${PORT_PICK_MAX:-25000}"

RSS_DELTA_LIMIT_KB="${RSS_DELTA_LIMIT_KB:-1048576}" # 1 GiB
KEEP_DATADIR="${KEEP_DATADIR:-0}"

BASE_RPC_PORT="${BASE_RPC_PORT:-19800}"
BASE_P2P_PORT="${BASE_P2P_PORT:-20800}"
WORKDIR="${WORKDIR:-/tmp/din_ibd_torture_${PROFILE}_$$}"

port_in_use() {
    local port="$1"
    lsof -nP -iTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1
}

port_window_free() {
    local rpc_base="$1" p2p_base="$2"
    local i
    for i in 0 1 2 3 4; do
        if port_in_use "$((rpc_base + i))" || port_in_use "$((p2p_base + i))"; then
            return 1
        fi
    done
    return 0
}

pick_free_base_ports() {
    if port_window_free "${BASE_RPC_PORT}" "${BASE_P2P_PORT}"; then
        return 0
    fi

    if [[ "${AUTO_PORTS}" != "1" ]]; then
        echo "FAIL: configured port window is busy (rpc=${BASE_RPC_PORT}..$((BASE_RPC_PORT + 4)), p2p=${BASE_P2P_PORT}..$((BASE_P2P_PORT + 4)))" >&2
        echo "Set AUTO_PORTS=1 or choose different BASE_RPC_PORT/BASE_P2P_PORT." >&2
        exit 1
    fi

    local attempts=0 span cand_rpc cand_p2p
    span=$((PORT_PICK_MAX - PORT_PICK_MIN))
    if [[ "${span}" -lt 2000 ]]; then
        echo "FAIL: invalid port pick range (${PORT_PICK_MIN}-${PORT_PICK_MAX})" >&2
        exit 1
    fi

    while [[ "${attempts}" -lt 300 ]]; do
        cand_rpc=$((PORT_PICK_MIN + RANDOM % (span - 1000)))
        cand_p2p=$((cand_rpc + 1000))
        if port_window_free "${cand_rpc}" "${cand_p2p}"; then
            BASE_RPC_PORT="${cand_rpc}"
            BASE_P2P_PORT="${cand_p2p}"
            return 0
        fi
        attempts=$((attempts + 1))
    done

    echo "FAIL: unable to find free RPC/P2P port window after ${attempts} attempts" >&2
    exit 1
}

pick_free_base_ports

mkdir -p "${WORKDIR}"

# Node roles:
# S = source canonical node, F = competing fork node, A/B/C = fresh IBD nodes.
NODE_NAMES=("S" "F" "A" "B" "C")
NODE_WALLETS=("ibd_source" "ibd_fork" "ibd_a" "ibd_b" "ibd_c")

RPC_PORTS=()
P2P_PORTS=()
DATADIRS=()
PIDS=("" "" "" "" "")
MINER_ADDRS=("" "" "" "" "")
RSS_BASELINES=(0 0 0 0 0)

for i in 0 1 2 3 4; do
    RPC_PORTS[$i]="$((BASE_RPC_PORT + i))"
    P2P_PORTS[$i]="$((BASE_P2P_PORT + i))"
    DATADIRS[$i]="${WORKDIR}/node_${NODE_NAMES[$i]}"
done

IDX_S=0
IDX_F=1
IDX_A=2
IDX_B=3
IDX_C=4

log_header() {
    echo -e "\n${CYAN}================================================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}================================================================${NC}"
}

info() { echo -e "${BLUE}INFO:${NC} $*"; }
warn() { echo -e "${YELLOW}WARN:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }
fail() { echo -e "${RED}FAIL:${NC} $*" >&2; exit 1; }

name_of() {
    local idx="$1"
    echo "${NODE_NAMES[$idx]}"
}

assert_distinct_datadirs() {
    python3 - "${DATADIRS[@]}" <<'PY'
import os
import sys

paths = [os.path.realpath(p) for p in sys.argv[1:]]
if len(paths) != len(set(paths)):
    print("datadir realpath collision detected:", file=sys.stderr)
    for p in paths:
        print(f"  {p}", file=sys.stderr)
    sys.exit(1)
PY
}

node_auth() {
    local idx="$1"
    local cookie
    cookie="$(cat "${DATADIRS[$idx]}/.cookie" 2>/dev/null || true)"
    [[ -n "${cookie}" ]] || return 1
    if [[ "${cookie}" == *:* ]]; then
        echo "${cookie}"
    else
        echo "__cookie__:${cookie}"
    fi
}

wait_file_nonempty() {
    local file="$1"
    local timeout_s="${2:-120}"
    local waited=0
    while [[ "${waited}" -lt "${timeout_s}" ]]; do
        if [[ -s "${file}" ]]; then
            return 0
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    return 1
}

rpc_ping_idx() {
    local idx="$1"
    local auth
    auth="$(node_auth "${idx}")" || return 1
    curl -sS --max-time 1 -X POST "http://127.0.0.1:${RPC_PORTS[$idx]}" \
        -u "${auth}" \
        -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"ping","method":"getblockchaininfo","params":[]}' \
        >/dev/null 2>&1
}

dump_startup_failure_idx() {
    local idx="$1"
    local logf="${DATADIRS[$idx]}/daemon.log"
    warn "Startup diagnostics for node $(name_of "${idx}")"
    if [[ -f "${logf}" ]]; then
        tail -n 200 "${logf}" || true
        if rg -a -n "Address already in use|bind|listen|port" "${logf}" >/dev/null 2>&1; then
            warn "Potential port bind/listen failure detected in $(name_of "${idx}") log"
            rg -a -n "Address already in use|bind|listen|port" "${logf}" | tail -n 20 || true
        fi
    else
        warn "No daemon log found for node $(name_of "${idx}")"
    fi
}

rpc_raw_idx() {
    local idx="$1" method="$2" params="${3:-[]}"
    local auth
    auth="$(node_auth "${idx}")" || return 1

    curl -sS -X POST "http://127.0.0.1:${RPC_PORTS[$idx]}" \
        -u "${auth}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}"
}

rpc_result_idx() {
    local idx="$1" method="$2" params="${3:-[]}"
    local pid="${PIDS[$idx]:-}"
    if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
        echo "RPC error [$(name_of "${idx}"):${method}]: node process not running" >&2
        return 1
    fi

    if ! rpc_ping_idx "${idx}"; then
        if ! wait_for_rpc_idx "${idx}"; then
            echo "RPC error [$(name_of "${idx}"):${method}]: RPC not ready" >&2
            return 1
        fi
    fi

    local resp
    resp="$(rpc_raw_idx "${idx}" "${method}" "${params}")" || return 1
    if echo "${resp}" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "${resp}" | jq -r '.error.message // (.error|tostring)')"
        echo "RPC error [$(name_of "${idx}"):${method}]: ${emsg}" >&2
        return 1
    fi
    echo "${resp}" | jq '.result'
}

rpc_scalar_idx() {
    local idx="$1" method="$2" params="$3" expr="$4"
    rpc_result_idx "${idx}" "${method}" "${params}" | jq -r "${expr}"
}

rpc_try_scalar_idx() {
    local idx="$1" method="$2" params="$3" expr="$4" def="$5"
    rpc_scalar_idx "${idx}" "${method}" "${params}" "${expr}" 2>/dev/null || echo "${def}"
}

wait_for_rpc_idx() {
    local idx="$1"
    local cookie_file="${DATADIRS[$idx]}/.cookie"
    if ! wait_file_nonempty "${cookie_file}" "${RPC_READY_TIMEOUT}"; then
        return 1
    fi

    local checks=0
    local max_checks=$((RPC_READY_TIMEOUT * 4))
    while [[ "${checks}" -lt "${max_checks}" ]]; do
        local pid="${PIDS[$idx]:-}"
        if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
            return 1
        fi
        if rpc_ping_idx "${idx}"; then
            return 0
        fi
        sleep 0.25
        checks=$((checks + 1))
    done
    return 1
}

start_node_idx() {
    local idx="$1" fresh="$2"

    if [[ "${fresh}" == "1" ]]; then
        rm -rf "${DATADIRS[$idx]}"
    fi
    mkdir -p "${DATADIRS[$idx]}"
    rm -f "${DATADIRS[$idx]}/peers.dat"

    pkill -f "dinerod.*${DATADIRS[$idx]}" 2>/dev/null || true
    sleep 0.2

    if port_in_use "${RPC_PORTS[$idx]}" || port_in_use "${P2P_PORTS[$idx]}"; then
        fail "Port collision before start for node $(name_of "${idx}") (rpc=${RPC_PORTS[$idx]} p2p=${P2P_PORTS[$idx]})"
    fi

    "${DINEROD}" \
        --regtest \
        --datadir="${DATADIRS[$idx]}" \
        --rpcport="${RPC_PORTS[$idx]}" \
        --port="${P2P_PORTS[$idx]}" \
        --debug \
        > "${DATADIRS[$idx]}/daemon.log" 2>&1 &

    PIDS[$idx]="$!"

    local spin
    for spin in {1..40}; do
        if ! kill -0 "${PIDS[$idx]}" 2>/dev/null; then
            dump_startup_failure_idx "${idx}"
            fail "Node $(name_of "${idx}") exited before RPC ready"
        fi
        sleep 0.05
    done

    if ! wait_for_rpc_idx "${idx}"; then
        dump_startup_failure_idx "${idx}"
        fail "Node $(name_of "${idx}") failed to become RPC-ready"
    fi
}

stop_node_idx() {
    local idx="$1"
    local pid="${PIDS[$idx]:-}"

    if [[ -n "${pid}" ]]; then
        kill "${pid}" 2>/dev/null || true
        local t
        for t in {1..30}; do
            if ! kill -0 "${pid}" 2>/dev/null; then
                break
            fi
            sleep 0.2
        done
        if kill -0 "${pid}" 2>/dev/null; then
            kill -9 "${pid}" 2>/dev/null || true
        fi
        wait "${pid}" 2>/dev/null || true
    fi
    pkill -f "dinerod.*${DATADIRS[$idx]}" 2>/dev/null || true
    PIDS[$idx]=""
}

stop_all_nodes() {
    local i
    for i in 0 1 2 3 4; do
        stop_node_idx "${i}"
    done
}

cleanup() {
    stop_all_nodes
    if [[ "${KEEP_DATADIR}" == "1" ]]; then
        warn "KEEP_DATADIR=1 set, preserving ${WORKDIR}"
    else
        rm -rf "${WORKDIR}"
    fi
}
trap cleanup EXIT

get_new_address_idx() {
    local idx="$1"
    rpc_result_idx "${idx}" "wallet.getnewaddress" "[]" | jq -r '.address // (if type=="string" then . else empty end)'
}

ensure_wallet_idx() {
    local idx="$1"
    local wname="${NODE_WALLETS[$idx]}"
    local addr

    rpc_result_idx "${idx}" "wallet.load" "[\"${wname}\"]" >/dev/null 2>&1 || true
    rpc_result_idx "${idx}" "wallet.createhd" "[\"${wname}\"]" >/dev/null 2>&1 || true

    addr="$(get_new_address_idx "${idx}" 2>/dev/null || true)"
    if [[ -z "${addr}" ]]; then
        fail "Unable to get wallet address for node $(name_of "${idx}")"
    fi
    MINER_ADDRS[$idx]="${addr}"
}

mine_blocks_idx() {
    local idx="$1" count="$2"
    if [[ -z "${MINER_ADDRS[$idx]}" ]]; then
        ensure_wallet_idx "${idx}"
    fi

    local remaining batch
    remaining="${count}"
    while [[ "${remaining}" -gt 0 ]]; do
        batch="${remaining}"
        if [[ "${batch}" -gt "${MINE_BATCH_SIZE}" ]]; then
            batch="${MINE_BATCH_SIZE}"
        fi
        rpc_result_idx "${idx}" "generatetoaddress" "[${batch},\"${MINER_ADDRS[$idx]}\"]" >/dev/null
        remaining=$((remaining - batch))
    done
}

get_block_hex_at_height_idx() {
    local idx="$1" height="$2"
    local block_hash
    block_hash="$(rpc_scalar_idx "${idx}" "getblockhash" "[${height}]" '.')"
    rpc_result_idx "${idx}" "getblock" "[\"${block_hash}\",0]" | jq -r 'if type=="object" then (.hex // "") else . end'
}

submit_block_hex_idx() {
    local idx="$1" hex="$2"
    local resp
    resp="$(rpc_raw_idx "${idx}" "blockchain.submitblock" "[\"${hex}\"]")" || return 1

    if echo "${resp}" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "${resp}" | jq -r '.error.message // (.error|tostring)')"
        echo "submitblock RPC error on $(name_of "${idx}"): ${emsg}" >&2
        return 1
    fi

    local accepted result_str
    accepted="$(echo "${resp}" | jq -r 'if .result == null then "1" elif ((.result|type) == "object" and (.result|length) == 0) then "1" elif ((.result|type) == "string" and .result == "") then "1" else "0" end')"
    if [[ "${accepted}" != "1" ]]; then
        result_str="$(echo "${resp}" | jq -c '.result')"
        echo "submitblock reject on $(name_of "${idx}"): ${result_str}" >&2
        return 1
    fi
    return 0
}

replay_missing_blocks_to_idx() {
    local src_idx="$1" dst_idx="$2"
    local src_h dst_h
    src_h="$(rpc_scalar_idx "${src_idx}" "getblockcount" "[]" '.')"
    dst_h="$(rpc_scalar_idx "${dst_idx}" "getblockcount" "[]" '.')"

    if [[ "${dst_h}" -ge "${src_h}" ]]; then
        return 0
    fi

    local missing
    missing=$((src_h - dst_h))
    info "Replaying ${missing} missing blocks from $(name_of "${src_idx}") to $(name_of "${dst_idx}")"

    local h submitted
    submitted=0
    for ((h=dst_h + 1; h<=src_h; h++)); do
        local block_hash
        local block_hex
        block_hash="$(rpc_scalar_idx "${src_idx}" "getblockhash" "[${h}]" '.')"
        block_hex="$(rpc_result_idx "${src_idx}" "getblock" "[\"${block_hash}\",0]" | jq -r 'if type=="object" then (.hex // "") else . end')"
        [[ -n "${block_hex}" && "${block_hex}" != "null" ]] || fail "Failed to fetch block hex at height ${h}"
        submit_block_hex_idx "${dst_idx}" "${block_hex}" || fail "Failed replay submit at height ${h}"
        if ! wait_height_at_least_idx "${dst_idx}" "${h}" 20; then
            fail "Replay block at height ${h} did not connect on $(name_of "${dst_idx}")"
        fi
        submitted=$((submitted + 1))
        if (( submitted % 100 == 0 )); then
            info "Replay progress to $(name_of "${dst_idx}"): ${submitted}/${missing}"
        fi
    done
}

recover_lagging_nodes_from_source() {
    local src_h
    src_h="$(rpc_try_scalar_idx "${IDX_S}" "getblockcount" "[]" '.' "-1")"
    [[ "${src_h}" =~ ^[0-9]+$ ]] || fail "Source node unavailable during lagging-node recovery"

    local idx
    for idx in "${IDX_A}" "${IDX_B}" "${IDX_C}"; do
        local h
        h="$(rpc_try_scalar_idx "${idx}" "getblockcount" "[]" '.' "-1")"
        [[ "${h}" =~ ^[0-9]+$ ]] || fail "Node $(name_of "${idx}") unavailable during lagging-node recovery"
        if [[ "${h}" -lt "${src_h}" ]]; then
            replay_missing_blocks_to_idx "${IDX_S}" "${idx}"
            connect_bidirectional "${idx}" "${IDX_S}"
        fi
    done
}

connect_oneway() {
    local src="$1" dst="$2"
    rpc_result_idx "${src}" "addnode" "[\"127.0.0.1:${P2P_PORTS[$dst]}\",\"onetry\"]" >/dev/null 2>&1 || true
}

connect_bidirectional() {
    local a="$1" b="$2"
    connect_oneway "${a}" "${b}"
    connect_oneway "${b}" "${a}"
}

connection_count_idx() {
    local idx="$1"
    rpc_try_scalar_idx "${idx}" "getconnectioncount" "[]" '.' "0"
}

assert_no_peers_idx() {
    local idx="$1"
    local cnt
    cnt="$(connection_count_idx "${idx}")"
    [[ "${cnt}" =~ ^[0-9]+$ ]] || cnt=0
    if [[ "${cnt}" -ne 0 ]]; then
        fail "Node $(name_of "${idx}") is expected isolated but has ${cnt} peers"
    fi
}

phase1_sync_vector_idx() {
    local idx="$1"
    local h hash peers headers work bi

    h="$(rpc_try_scalar_idx "${idx}" "getblockcount" "[]" '.' "NA")"
    hash="$(rpc_try_scalar_idx "${idx}" "getbestblockhash" "[]" '.' "NA")"
    peers="$(connection_count_idx "${idx}")"

    bi="$(rpc_result_idx "${idx}" "getblockchaininfo" "[]" 2>/dev/null || echo "{}")"
    headers="$(echo "${bi}" | jq -r '.headers // .best_header_height // "NA"' 2>/dev/null || echo "NA")"
    work="$(echo "${bi}" | jq -r '.chainwork // "NA"' 2>/dev/null || echo "NA")"

    if [[ "${headers}" == "NA" && "${h}" != "NA" ]]; then
        headers="${h}"
    fi

    echo "${h}|${headers}|${work}|${hash}|${peers}"
}

dump_phase1_diagnostics_idx() {
    local idx="$1"
    local name
    name="$(name_of "${idx}")"

    warn "Phase 1 diagnostics for node ${name}"
    echo "  state: $(phase1_sync_vector_idx "${idx}")"

    local bi
    bi="$(rpc_result_idx "${idx}" "getblockchaininfo" "[]" 2>/dev/null || echo "{}")"
    echo "  getblockchaininfo=$(echo "${bi}" | jq -c '{blocks:(.blocks // .height // null), headers:(.headers // .best_header_height // null), chainwork:(.chainwork // null), bestblockhash:(.bestblockhash // null), initialblockdownload:(.initialblockdownload // null), verificationprogress:(.verificationprogress // null)}' 2>/dev/null || echo '{}')"

    local peer_info
    peer_info="$(rpc_result_idx "${idx}" "getpeerinfo" "[]" 2>/dev/null || echo "[]")"
    echo "  getpeerinfo=$(echo "${peer_info}" | jq -c 'if type=="array" then map({addr:(.addr // null), inbound:(.inbound // null), connection_type:(.connection_type // null), startingheight:(.startingheight // null), synced_headers:(.synced_headers // null), synced_blocks:(.synced_blocks // null), lastrecv:(.lastrecv // null), lastsend:(.lastsend // null)}) else . end' 2>/dev/null || echo '[]')"

    local tips
    tips="$(rpc_result_idx "${idx}" "getchaintips" "[]" 2>/dev/null || echo "[]")"
    echo "  getchaintips=$(echo "${tips}" | jq -c '.' 2>/dev/null || echo '[]')"

    local logf="${DATADIRS[$idx]}/daemon.log"
    if [[ -f "${logf}" ]]; then
        echo "  log_tail(relevant):"
        rg -a -n "BlockDownloadScheduler|HeaderSync|ActivateBestChain|utreexo|missing blocks|requesting block|GetData|headers sync|OnHeaders" "${logf}" 2>/dev/null | tail -n 60 || true
    fi
}

wait_phase1_start_idx() {
    local idx="$1" timeout="$2"
    local waited=0

    while [[ "${waited}" -lt "${timeout}" ]]; do
        local v h headers _work _hash peers
        v="$(phase1_sync_vector_idx "${idx}")"
        IFS='|' read -r h headers _work _hash peers <<< "${v}"

        local peers_ok=0 headers_ok=0
        if [[ "${peers}" =~ ^[0-9]+$ ]] && [[ "${peers}" -ge 1 ]]; then
            peers_ok=1
        fi
        if [[ "${headers}" == "NA" ]]; then
            headers_ok=1
        elif [[ "${headers}" =~ ^[0-9]+$ ]] && [[ "${headers}" -gt 0 ]]; then
            headers_ok=1
        fi

        if [[ "${peers_ok}" == "1" && "${headers_ok}" == "1" ]]; then
            return 0
        fi

        sleep 1
        waited=$((waited + 1))
    done

    warn "Phase 1 start condition failed for node $(name_of "${idx}") after ${timeout}s"
    dump_phase1_diagnostics_idx "${idx}"
    return 1
}

wait_phase1_progress_idx() {
    local idx="$1" target_height="$2" max_total="$3" no_progress_limit="$4"
    local waited=0
    local no_progress=0
    local prev_progress=""

    if ! wait_phase1_start_idx "${idx}" "${PHASE1_START_TIMEOUT}"; then
        return 1
    fi

    while [[ "${waited}" -lt "${max_total}" ]]; do
        local v h headers work _hash peers
        v="$(phase1_sync_vector_idx "${idx}")"
        IFS='|' read -r h headers work _hash peers <<< "${v}"

        if [[ "${h}" =~ ^[0-9]+$ ]] && [[ "${h}" -ge "${target_height}" ]]; then
            return 0
        fi

        local progress_tuple="${h}|${headers}|${work}"
        if [[ -n "${prev_progress}" && "${progress_tuple}" != "${prev_progress}" ]]; then
            no_progress=0
        else
            if [[ "${peers}" =~ ^[0-9]+$ ]] && [[ "${peers}" -ge 1 ]]; then
                no_progress=$((no_progress + 1))
            fi
        fi
        prev_progress="${progress_tuple}"

        if [[ "${no_progress}" -ge "${no_progress_limit}" ]]; then
            warn "Phase 1 liveness stalled for node $(name_of "${idx}") (no progress ${no_progress}s)"
            dump_phase1_diagnostics_idx "${idx}"
            return 1
        fi

        sleep 1
        waited=$((waited + 1))
    done

    warn "Phase 1 progress timeout for node $(name_of "${idx}") after ${max_total}s"
    dump_phase1_diagnostics_idx "${idx}"
    return 1
}

state_triplet_idx() {
    local idx="$1"
    local h hash work
    h="$(rpc_try_scalar_idx "${idx}" "getblockcount" "[]" '.' "NA")"
    hash="$(rpc_try_scalar_idx "${idx}" "getbestblockhash" "[]" '.' "NA")"
    work="$(rpc_result_idx "${idx}" "getblockchaininfo" "[]" 2>/dev/null | jq -r '.chainwork // "NA"' 2>/dev/null || echo "NA")"
    echo "${h}|${hash}|${work}"
}

print_state_idx() {
    local idx="$1"
    local t h hash work peers
    t="$(state_triplet_idx "${idx}")"
    IFS='|' read -r h hash work <<< "${t}"
    peers="$(connection_count_idx "${idx}")"
    echo "  $(name_of "${idx}"): height=${h} hash=${hash} work=${work} peers=${peers}"
}

print_network_state() {
    local i
    for i in 0 1 2 3 4; do
        print_state_idx "${i}"
    done
}

rss_kb_idx() {
    local idx="$1"
    local pid="${PIDS[$idx]:-}"
    if [[ -z "${pid}" ]]; then
        echo 0
        return
    fi
    ps -o rss= -p "${pid}" 2>/dev/null | tr -d ' ' || echo 0
}

assert_rss_not_runaway_idx() {
    local idx="$1" baseline="$2"
    local current limit
    current="$(rss_kb_idx "${idx}")"
    [[ "${current}" =~ ^[0-9]+$ ]] || current=0
    limit=$((baseline + RSS_DELTA_LIMIT_KB))
    if [[ "${current}" -gt "${limit}" ]]; then
        fail "Node $(name_of "${idx}") RSS grew too much: current=${current}KB baseline=${baseline}KB limit=${limit}KB"
    fi
}

assert_nodes_alive() {
    local i
    for i in 0 1 2 3 4; do
        local pid="${PIDS[$i]:-}"
        if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
            fail "Node $(name_of "${i}") is not running"
        fi
    done
}

wait_height_at_least_idx() {
    local idx="$1" target="$2" timeout="$3"
    local waited=0
    while [[ "${waited}" -lt "${timeout}" ]]; do
        local h
        h="$(rpc_try_scalar_idx "${idx}" "getblockcount" "[]" '.' "-1")"
        if [[ "${h}" =~ ^[0-9]+$ ]] && [[ "${h}" -ge "${target}" ]]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

snapshot_nodes() {
    local s=""
    local i
    for i in 0 1 2 3 4; do
        s+="${i}:$(state_triplet_idx "${i}")|"
    done
    echo "${s}"
}

all_nodes_converged() {
    local base_h base_hash base_work
    local base_triplet
    base_triplet="$(state_triplet_idx "${IDX_S}")"
    IFS='|' read -r base_h base_hash base_work <<< "${base_triplet}"

    if [[ "${base_h}" == "NA" || "${base_hash}" == "NA" || "${base_work}" == "NA" ]]; then
        return 1
    fi

    local i
    for i in "${IDX_A}" "${IDX_B}" "${IDX_C}"; do
        local t h hash work
        t="$(state_triplet_idx "${i}")"
        IFS='|' read -r h hash work <<< "${t}"
        if [[ "${h}" != "${base_h}" || "${hash}" != "${base_hash}" || "${work}" != "${base_work}" ]]; then
            return 1
        fi
    done
    return 0
}

wait_nodes_converged_with_progress() {
    local base_timeout="$1"
    local waited=0
    local no_progress=0
    local deadline="${base_timeout}"
    local previous_snapshot=""

    while [[ "${waited}" -lt "${MAX_PROGRESS_TIMEOUT}" && "${waited}" -lt "${deadline}" ]]; do
        if all_nodes_converged; then
            return 0
        fi

        local snapshot
        snapshot="$(snapshot_nodes)"
        if [[ "${snapshot}" != "${previous_snapshot}" ]]; then
            previous_snapshot="${snapshot}"
            no_progress=0

            local extended_deadline=$((waited + PROGRESS_EXTENSION))
            if [[ "${extended_deadline}" -gt "${deadline}" ]]; then
                deadline="${extended_deadline}"
                if [[ "${deadline}" -gt "${MAX_PROGRESS_TIMEOUT}" ]]; then
                    deadline="${MAX_PROGRESS_TIMEOUT}"
                fi
            fi
        else
            no_progress=$((no_progress + 1))
            if [[ "${no_progress}" -ge "${NO_PROGRESS_TIMEOUT}" ]]; then
                warn "Convergence stalled for ${no_progress}s without state change"
                print_network_state
                return 1
            fi
        fi

        sleep 1
        waited=$((waited + 1))
    done

    warn "Convergence timed out after ${waited}s (deadline=${deadline}s)"
    print_network_state
    return 1
}

hex_gt() {
    local left="$1" right="$2"
    python3 - "$left" "$right" <<'PY'
import sys
left = int(sys.argv[1], 16)
right = int(sys.argv[2], 16)
print("1" if left > right else "0")
PY
}

chainwork_idx() {
    local idx="$1"
    rpc_result_idx "${idx}" "getblockchaininfo" "[]" | jq -r '.chainwork // "0x0"'
}

chainwork_try_idx() {
    local idx="$1"
    rpc_result_idx "${idx}" "getblockchaininfo" "[]" 2>/dev/null | jq -r '.chainwork // "NA"' 2>/dev/null || echo "NA"
}

assert_hash_work_consistent() {
    local hash_s work_s
    hash_s="$(rpc_scalar_idx "${IDX_S}" "getbestblockhash" "[]" '.')"
    work_s="$(chainwork_idx "${IDX_S}")"

    local i
    for i in 1 2 3 4; do
        local hash_i work_i
        hash_i="$(rpc_scalar_idx "${i}" "getbestblockhash" "[]" '.')"
        work_i="$(chainwork_idx "${i}")"
        if [[ "${hash_i}" == "${hash_s}" && "${work_i}" != "${work_s}" ]]; then
            fail "Hash/work mismatch: $(name_of "${i}") matches source hash but differs chainwork"
        fi
    done
}

scan_logs_for_fatal() {
    local i
    for i in 0 1 2 3 4; do
        local logf="${DATADIRS[$i]}/daemon.log"
        [[ -f "${logf}" ]] || continue

        local bad
        bad="$(
            grep -aEi "ASSERTION FAILED|\\bFATAL\\b|Segmentation fault|AddressSanitizer|terminate called|panic:|utreexo root mismatch|missing-utreexo-delta-undo-data" "${logf}" \
                | grep -avE "\\[MINING ASSERT\\]" \
                || true
        )"
        if [[ -n "${bad}" ]]; then
            echo "Potential fatal signature found in ${logf}:" >&2
            echo "${bad}" | head -n 20 >&2
            fail "Fatal/assert signature detected in node logs"
        fi
    done
}

copy_source_to_fork_snapshot() {
    rm -rf "${DATADIRS[$IDX_F]}"
    mkdir -p "${DATADIRS[$IDX_F]}"
    cp -a "${DATADIRS[$IDX_S]}/." "${DATADIRS[$IDX_F]}/"
    rm -f "${DATADIRS[$IDX_F]}/.cookie" \
          "${DATADIRS[$IDX_F]}/.lock" \
          "${DATADIRS[$IDX_F]}/peers.dat" \
          "${DATADIRS[$IDX_F]}/debug.log"
}

start_churn_b() {
    local iter
    for ((iter=1; iter<=CHURN_LOOPS; iter++)); do
        sleep "${CHURN_INTERVAL}"
        stop_node_idx "${IDX_B}"
        sleep "${CHURN_RESTART_PAUSE}"
        start_node_idx "${IDX_B}" 0
        if ! wait_for_rpc_idx "${IDX_B}"; then
            dump_startup_failure_idx "${IDX_B}"
            fail "Node B RPC not ready after churn restart"
        fi
        connect_bidirectional "${IDX_B}" "${IDX_S}"
    done
}

ensure_distinct_source_fork_miner_identity() {
    local addr_s addr_f
    addr_s="${MINER_ADDRS[$IDX_S]}"
    addr_f="${MINER_ADDRS[$IDX_F]}"

    if [[ "${addr_s}" == "${addr_f}" ]]; then
        local tries
        for tries in {1..8}; do
            addr_f="$(get_new_address_idx "${IDX_F}")"
            MINER_ADDRS[$IDX_F]="${addr_f}"
            if [[ "${addr_s}" != "${addr_f}" ]]; then
                break
            fi
        done
    fi

    if [[ "${addr_s}" == "${addr_f}" ]]; then
        fail "Source/fork miner identities are identical; cannot guarantee deterministic fork divergence"
    fi
}

assert_phase0_divergence_probe() {
    local base_s base_f probe1_s probe1_f probe2_s probe2_f
    base_s="$(rpc_scalar_idx "${IDX_S}" "getbestblockhash" "[]" '.')"
    base_f="$(rpc_scalar_idx "${IDX_F}" "getbestblockhash" "[]" '.')"

    if [[ "${base_s}" != "${base_f}" ]]; then
        fail "Phase 0 probe expected shared-base equality before isolated mining"
    fi

    mine_blocks_idx "${IDX_S}" 1
    probe1_s="$(rpc_scalar_idx "${IDX_S}" "getbestblockhash" "[]" '.')"
    probe1_f="$(rpc_scalar_idx "${IDX_F}" "getbestblockhash" "[]" '.')"
    if [[ "${probe1_s}" == "${probe1_f}" ]]; then
        fail "Phase 0 probe failed: S-only mining did not diverge tips"
    fi

    mine_blocks_idx "${IDX_F}" 1
    probe2_s="$(rpc_scalar_idx "${IDX_S}" "getbestblockhash" "[]" '.')"
    probe2_f="$(rpc_scalar_idx "${IDX_F}" "getbestblockhash" "[]" '.')"
    if [[ "${probe2_s}" == "${probe2_f}" ]]; then
        fail "Phase 0 probe failed: F-only mining re-converged tips unexpectedly"
    fi
}

phase0_prepare_forks() {
    log_header "Phase 0 - Prepare Divergent Forks"

    start_node_idx "${IDX_S}" 1
    ensure_wallet_idx "${IDX_S}"
    info "Source mining shared base: ${SHARED_BASE_BLOCKS} blocks"
    mine_blocks_idx "${IDX_S}" "${SHARED_BASE_BLOCKS}"

    stop_node_idx "${IDX_S}"
    copy_source_to_fork_snapshot

    start_node_idx "${IDX_S}" 0
    start_node_idx "${IDX_F}" 0
    assert_no_peers_idx "${IDX_S}"
    assert_no_peers_idx "${IDX_F}"

    ensure_wallet_idx "${IDX_S}"
    ensure_wallet_idx "${IDX_F}"
    ensure_distinct_source_fork_miner_identity

    info "Phase 0 divergence probe (isolated S/F)"
    assert_phase0_divergence_probe

    info "Source mining canonical extension: ${SOURCE_PRE_BLOCKS} blocks"
    mine_blocks_idx "${IDX_S}" "${SOURCE_PRE_BLOCKS}"

    info "Fork mining competing extension: ${FORK_PRE_BLOCKS} blocks"
    mine_blocks_idx "${IDX_F}" "${FORK_PRE_BLOCKS}"

    local h_s h_f hash_s hash_f work_s work_f
    h_s="$(rpc_scalar_idx "${IDX_S}" "getblockcount" "[]" '.')"
    h_f="$(rpc_scalar_idx "${IDX_F}" "getblockcount" "[]" '.')"
    hash_s="$(rpc_scalar_idx "${IDX_S}" "getbestblockhash" "[]" '.')"
    hash_f="$(rpc_scalar_idx "${IDX_F}" "getbestblockhash" "[]" '.')"
    work_s="$(chainwork_idx "${IDX_S}")"
    work_f="$(chainwork_idx "${IDX_F}")"

    if [[ "${hash_s}" == "${hash_f}" ]]; then
        fail "Source and fork tips are identical after fork preparation"
    fi

    if [[ "$(hex_gt "${work_f}" "${work_s}")" != "1" ]]; then
        fail "Competing fork must initially lead in chainwork (S=${work_s}, F=${work_f})"
    fi

    info "Prepared competing tips:"
    print_state_idx "${IDX_S}"
    print_state_idx "${IDX_F}"
    pass "Fork preparation complete"
}

phase1_ibd_with_adversity() {
    log_header "Phase 1 - IBD Under Adversity"

    start_node_idx "${IDX_A}" 1
    start_node_idx "${IDX_B}" 1
    start_node_idx "${IDX_C}" 1

    RSS_BASELINES[$IDX_A]="$(rss_kb_idx "${IDX_A}")"
    RSS_BASELINES[$IDX_B]="$(rss_kb_idx "${IDX_B}")"
    RSS_BASELINES[$IDX_C]="$(rss_kb_idx "${IDX_C}")"

    # Bootstrap all fresh IBD nodes from source chain first.
    # Fork chain remains withheld until Phase 3.
    connect_bidirectional "${IDX_A}" "${IDX_S}"
    connect_bidirectional "${IDX_B}" "${IDX_S}"
    connect_bidirectional "${IDX_C}" "${IDX_S}"

    info "Mining withheld fork/source batches (S=${SOURCE_WITHHELD_BLOCKS}, F=${FORK_WITHHELD_BLOCKS})"
    mine_blocks_idx "${IDX_S}" "${SOURCE_WITHHELD_BLOCKS}"
    mine_blocks_idx "${IDX_F}" "${FORK_WITHHELD_BLOCKS}"

    info "Running deterministic disconnect churn on node B (loops=${CHURN_LOOPS})"
    start_churn_b

    local target_a target_b b_delta
    target_a=$((SHARED_BASE_BLOCKS + IBD_PROGRESS_TARGET))
    b_delta=$((IBD_PROGRESS_TARGET / 2))
    if [[ "${b_delta}" -lt 1 ]]; then
        b_delta=1
    fi
    target_b=$((SHARED_BASE_BLOCKS + b_delta))

    if ! wait_phase1_progress_idx "${IDX_A}" "${target_a}" "${PROGRESS_TIMEOUT}" "${PHASE1_NO_PROGRESS_TIMEOUT}"; then
        print_network_state
        fail "Node A failed to make expected IBD progress"
    fi
    if ! wait_phase1_progress_idx "${IDX_B}" "${target_b}" "${PROGRESS_TIMEOUT}" "${PHASE1_NO_PROGRESS_TIMEOUT}"; then
        print_network_state
        fail "Node B failed to make expected IBD progress"
    fi
    pass "IBD adversity phase complete"
}

phase2_restart_mid_sync() {
    log_header "Phase 2 - Mid-IBD Restart Recovery"

    local before after
    before="$(rpc_try_scalar_idx "${IDX_C}" "getblockcount" "[]" '.' "-1")"
    info "Restarting node C mid-sync (pre-restart height=${before})"

    # Release one active source slot so C can become an active downloader deterministically.
    stop_node_idx "${IDX_A}"
    stop_node_idx "${IDX_C}"
    sleep 1
    start_node_idx "${IDX_C}" 0
    connect_bidirectional "${IDX_C}" "${IDX_S}"

    local c_target
    c_target=$((IBD_PROGRESS_TARGET / 2))
    if [[ "${c_target}" -lt 1 ]]; then
        c_target=1
    fi
    if ! wait_height_at_least_idx "${IDX_C}" "${c_target}" "${PROGRESS_TIMEOUT}"; then
        print_network_state
        fail "Node C did not resume progress after mid-sync restart"
    fi

    start_node_idx "${IDX_A}" 0
    connect_bidirectional "${IDX_A}" "${IDX_S}"

    after="$(rpc_try_scalar_idx "${IDX_C}" "getblockcount" "[]" '.' "-1")"
    info "Node C resumed at height=${after}"
    pass "Mid-IBD restart phase complete"
}

phase3_compete_and_heal() {
    log_header "Phase 3 - Competing Fork Exposure + Heal"

    info "Releasing withheld fork to all IBD nodes"
    connect_bidirectional "${IDX_A}" "${IDX_F}"
    connect_bidirectional "${IDX_B}" "${IDX_F}"
    connect_bidirectional "${IDX_C}" "${IDX_S}"
    connect_bidirectional "${IDX_S}" "${IDX_F}"
    assert_nodes_alive

    # Mine source catch-up blocks to force deterministic winner.
    info "Mining source catch-up blocks: ${SOURCE_POST_BLOCKS}"
    mine_blocks_idx "${IDX_S}" "${SOURCE_POST_BLOCKS}"
    assert_nodes_alive

    local attempts=0
    while true; do
        local work_s work_f
        work_s="$(chainwork_try_idx "${IDX_S}")"
        work_f="$(chainwork_try_idx "${IDX_F}")"
        if [[ "${work_s}" == "NA" || "${work_f}" == "NA" ]]; then
            print_network_state
            fail "Node unavailable while evaluating chainwork winner (S=${work_s}, F=${work_f})"
        fi
        if [[ "$(hex_gt "${work_s}" "${work_f}")" == "1" ]]; then
            break
        fi
        attempts=$((attempts + 1))
        if [[ "${attempts}" -gt 20 ]]; then
            fail "Source failed to overtake competing fork chainwork"
        fi
        warn "Source still behind fork, mining additional ${SOURCE_WIN_STEP} blocks"
        mine_blocks_idx "${IDX_S}" "${SOURCE_WIN_STEP}"
        assert_nodes_alive
    done

    # Ensure source wins by more than a tiny edge.
    local h_s h_f
    h_s="$(rpc_scalar_idx "${IDX_S}" "getblockcount" "[]" '.')"
    h_f="$(rpc_scalar_idx "${IDX_F}" "getblockcount" "[]" '.')"
    if [[ "${h_s}" -lt $((h_f + SOURCE_WIN_MARGIN)) ]]; then
        local extra
        extra=$((h_f + SOURCE_WIN_MARGIN - h_s))
        warn "Increasing source margin by mining ${extra} extra blocks"
        mine_blocks_idx "${IDX_S}" "${extra}"
    fi

    # Full-heal connectivity fanout.
    connect_bidirectional "${IDX_A}" "${IDX_S}"
    connect_bidirectional "${IDX_B}" "${IDX_S}"
    connect_bidirectional "${IDX_C}" "${IDX_S}"
    connect_bidirectional "${IDX_A}" "${IDX_F}"
    connect_bidirectional "${IDX_B}" "${IDX_F}"

    if ! wait_nodes_converged_with_progress "${CONVERGE_TIMEOUT}"; then
        warn "Initial heal stalled; attempting explicit withheld-block release to lagging nodes"
        recover_lagging_nodes_from_source
        if ! wait_nodes_converged_with_progress "${CONVERGE_TIMEOUT}"; then
            fail "Nodes failed to converge after fork heal"
        fi
    fi

    # Final nudge block to assert post-heal stability.
    mine_blocks_idx "${IDX_S}" 1
    if ! wait_nodes_converged_with_progress "${CONVERGE_TIMEOUT}"; then
        fail "Nodes diverged after post-heal stability block"
    fi

    pass "Competing fork heal complete"
}

final_gate() {
    log_header "Final Gate"

    assert_nodes_alive

    if ! all_nodes_converged; then
        print_network_state
        fail "Final convergence check failed"
    fi

    assert_hash_work_consistent
    scan_logs_for_fatal

    assert_rss_not_runaway_idx "${IDX_A}" "${RSS_BASELINES[$IDX_A]}"
    assert_rss_not_runaway_idx "${IDX_B}" "${RSS_BASELINES[$IDX_B]}"
    assert_rss_not_runaway_idx "${IDX_C}" "${RSS_BASELINES[$IDX_C]}"

    local final_h final_hash final_work
    final_h="$(rpc_scalar_idx "${IDX_S}" "getblockcount" "[]" '.')"
    final_hash="$(rpc_scalar_idx "${IDX_S}" "getbestblockhash" "[]" '.')"
    final_work="$(chainwork_idx "${IDX_S}")"

    echo "FINAL_HEIGHT=${final_h}"
    echo "FINAL_TIP_HASH=${final_hash}"
    echo "FINAL_CHAINWORK=${final_work}"
    echo "WORKDIR=${WORKDIR}"
    print_network_state

    pass "IBD torture gate passed"
}

main() {
    log_header "IBD Torture Harness"
    info "Profile=${PROFILE}"
    info "Workdir: ${WORKDIR}"
    info "RPC base=${BASE_RPC_PORT}, P2P base=${BASE_P2P_PORT}"
    info "dinerod=${DINEROD}"
    info "Knobs: shared=${SHARED_BASE_BLOCKS} src_pre=${SOURCE_PRE_BLOCKS} fork_pre=${FORK_PRE_BLOCKS} src_withheld=${SOURCE_WITHHELD_BLOCKS} fork_withheld=${FORK_WITHHELD_BLOCKS} src_post=${SOURCE_POST_BLOCKS} ibd_target=${IBD_PROGRESS_TARGET} churn_loops=${CHURN_LOOPS} phase1_start_timeout=${PHASE1_START_TIMEOUT} phase1_no_progress=${PHASE1_NO_PROGRESS_TIMEOUT} mine_batch=${MINE_BATCH_SIZE}"

    assert_distinct_datadirs

    phase0_prepare_forks
    phase1_ibd_with_adversity
    phase2_restart_mid_sync
    phase3_compete_and_heal
    final_gate
}

main "$@"
