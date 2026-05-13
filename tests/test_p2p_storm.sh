#!/usr/bin/env bash
#
# P2P Storm Harness (regtest)
#
# Deterministic, script-driven adversarial network stress:
# - Phase 1: Rapid block relay pressure (INV-like flood via rapid block production)
# - Phase 2: Unknown-parent block flood via submitblock (RPC-level orphan-like pressure)
# - Phase 3: Reconnect churn with per-loop block propagation
# - Phase 4: Partition + heal with competing chain tips
# - Phase 5: Mempool activity during partition and post-heal reconciliation checks
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

if ! command -v curl >/dev/null 2>&1 || ! command -v jq >/dev/null 2>&1; then
    echo -e "${RED}FAIL:${NC} requires curl and jq" >&2
    exit 1
fi

# Tunables
NODE_COUNT=5
PHASE1_BLOCKS="${PHASE1_BLOCKS:-50}"
PHASE1_BATCH_SIZE="${PHASE1_BATCH_SIZE:-10}"
PHASE2_ORPHAN_BLOCKS="${PHASE2_ORPHAN_BLOCKS:-100}"
PHASE3_CHURN_LOOPS="${PHASE3_CHURN_LOOPS:-50}"
PHASE4_BLOCKS_A="${PHASE4_BLOCKS_A:-15}"
PHASE4_BLOCKS_B="${PHASE4_BLOCKS_B:-10}"
PHASE4_WIN_MARGIN="${PHASE4_WIN_MARGIN:-5}"
PHASE5_TX_PER_SIDE="${PHASE5_TX_PER_SIDE:-10}"

BOOTSTRAP_BLOCKS="${BOOTSTRAP_BLOCKS:-130}"         # Coinbase maturity safety
BOOTSTRAP_SEND_AMOUNT="${BOOTSTRAP_SEND_AMOUNT:-25.0}"
TX_AMOUNT="${TX_AMOUNT:-0.01}"

RPC_READY_TIMEOUT="${RPC_READY_TIMEOUT:-45}"
SYNC_TIMEOUT="${SYNC_TIMEOUT:-240}"
GROUP_SYNC_TIMEOUT="${GROUP_SYNC_TIMEOUT:-120}"
CHURN_LOOP_SYNC_TIMEOUT="${CHURN_LOOP_SYNC_TIMEOUT:-90}"
PEER_CONNECT_TIMEOUT="${PEER_CONNECT_TIMEOUT:-30}"
HEAL_BASE_TIMEOUT="${HEAL_BASE_TIMEOUT:-120}"
HEAL_PROGRESS_EXTENSION="${HEAL_PROGRESS_EXTENSION:-30}"
HEAL_MAX_TIMEOUT="${HEAL_MAX_TIMEOUT:-600}"
HEAL_NO_PROGRESS_TIMEOUT="${HEAL_NO_PROGRESS_TIMEOUT:-30}"

PEER_LEAK_ALLOWANCE="${PEER_LEAK_ALLOWANCE:-2}"
RSS_DELTA_LIMIT_KB="${RSS_DELTA_LIMIT_KB:-1048576}" # 1 GiB safety band
KEEP_DATADIR="${KEEP_DATADIR:-0}"

BASE_RPC_PORT="${BASE_RPC_PORT:-19600}"
BASE_P2P_PORT="${BASE_P2P_PORT:-20600}"
BASE_WALLET_SOCKET_PORT="${BASE_WALLET_SOCKET_PORT:-21600}"

WORKDIR="${WORKDIR:-/tmp/din_p2p_storm_$$}"
mkdir -p "${WORKDIR}"

# Fixed node identity
NODE_NAMES=("A" "B" "C" "D" "E")
NODE_WALLETS=("storm_a" "storm_b" "storm_c" "storm_d" "storm_e")
RPC_PORTS=()
P2P_PORTS=()
WALLET_SOCKET_PORTS=()
DATADIRS=()
PIDS=("" "" "" "" "")
MINER_ADDRS=("" "" "" "" "")

for i in 0 1 2 3 4; do
    RPC_PORTS[$i]="$((BASE_RPC_PORT + i))"
    P2P_PORTS[$i]="$((BASE_P2P_PORT + i))"
    WALLET_SOCKET_PORTS[$i]="$((BASE_WALLET_SOCKET_PORT + i))"
    DATADIRS[$i]="${WORKDIR}/node_${NODE_NAMES[$i]}"
done

BASELINE_RSS_A=0

log_header() {
    echo -e "\n${CYAN}================================================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}================================================================${NC}"
}

info() { echo -e "${BLUE}INFO:${NC} $*"; }
warn() { echo -e "${YELLOW}WARN:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }
fail() { echo -e "${RED}FAIL:${NC} $*" >&2; exit 1; }

idx_of() {
    case "$1" in
        A) echo 0 ;;
        B) echo 1 ;;
        C) echo 2 ;;
        D) echo 3 ;;
        E) echo 4 ;;
        *) return 1 ;;
    esac
}

name_of() {
    local idx="$1"
    echo "${NODE_NAMES[$idx]}"
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
    local waited=0
    while [[ "${waited}" -lt "${RPC_READY_TIMEOUT}" ]]; do
        if rpc_raw_idx "${idx}" "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
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

    # Best-effort cleanup for stale process bound to this datadir.
    pkill -f "dinerod.*${DATADIRS[$idx]}" 2>/dev/null || true
    sleep 0.2

    "${DINEROD}" \
        --regtest \
        --datadir="${DATADIRS[$idx]}" \
        --rpcport="${RPC_PORTS[$idx]}" \
        --port="${P2P_PORTS[$idx]}" \
        --wallet-socket-port="${WALLET_SOCKET_PORTS[$idx]}" \
        --debug \
        > "${DATADIRS[$idx]}/daemon.log" 2>&1 &

    PIDS[$idx]="$!"

    if ! wait_for_rpc_idx "${idx}"; then
        fail "Node $(name_of "${idx}") failed to become RPC-ready"
    fi
}

stop_node_idx() {
    local idx="$1"
    local pid="${PIDS[$idx]:-}"

    if [[ -n "${pid}" ]]; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    pkill -f "dinerod.*${DATADIRS[$idx]}" 2>/dev/null || true
    PIDS[$idx]=""
}

start_all_nodes() {
    local fresh="$1"
    local i
    for i in 0 1 2 3 4; do
        start_node_idx "${i}" "${fresh}"
    done
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

get_balance_idx() {
    local idx="$1"
    rpc_result_idx "${idx}" "wallet.getbalance" "[]" | jq -r '.spendable // .confirmed // .total // (if type=="number" then . else 0 end)'
}

mine_blocks_idx() {
    local idx="$1" count="$2"
    if [[ -z "${MINER_ADDRS[$idx]}" ]]; then
        ensure_wallet_idx "${idx}"
    fi
    rpc_result_idx "${idx}" "generatetoaddress" "[${count},\"${MINER_ADDRS[$idx]}\"]" >/dev/null
}

send_to_address_idx() {
    local idx="$1" addr="$2" amount="$3"
    rpc_result_idx "${idx}" "wallet.sendtoaddress" "[\"${addr}\",${amount},\"\",\"\",true]"
}

connection_count_idx() {
    local idx="$1"
    rpc_try_scalar_idx "${idx}" "getconnectioncount" "[]" '.' "0"
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

wait_peer_min_idx() {
    local idx="$1" min_peers="$2"
    local waited=0
    while [[ "${waited}" -lt "${PEER_CONNECT_TIMEOUT}" ]]; do
        local cnt
        cnt="$(connection_count_idx "${idx}")"
        if [[ "${cnt}" =~ ^[0-9]+$ ]] && [[ "${cnt}" -ge "${min_peers}" ]]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
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

print_network_state() {
    local i
    for i in 0 1 2 3 4; do
        local triplet
        triplet="$(state_triplet_idx "${i}")"
        local h hash work
        IFS='|' read -r h hash work <<< "${triplet}"
        echo "  $(name_of "${i}"): height=${h} hash=${hash} work=${work}"
    done
}

print_peer_state() {
    local i
    for i in 0 1 2 3 4; do
        local cnt
        cnt="$(connection_count_idx "${i}")"
        echo "  $(name_of "${i}"): peers=${cnt}"
    done
}

print_leader_chaintips() {
    local tips_a tips_b
    tips_a="$(rpc_result_idx 0 "getchaintips" "[]" 2>/dev/null | jq -c '.' 2>/dev/null || echo "N/A")"
    tips_b="$(rpc_result_idx 1 "getchaintips" "[]" 2>/dev/null | jq -c '.' 2>/dev/null || echo "N/A")"
    echo "  chaintips(A)=${tips_a}"
    echo "  chaintips(B)=${tips_b}"
}

all_nodes_converged() {
    local base_h base_hash base_work
    local base_triplet
    base_triplet="$(state_triplet_idx 0)"
    IFS='|' read -r base_h base_hash base_work <<< "${base_triplet}"

    if [[ "${base_h}" == "NA" || "${base_hash}" == "NA" || "${base_work}" == "NA" ]]; then
        return 1
    fi

    local i
    for i in 1 2 3 4; do
        local t h hash work
        t="$(state_triplet_idx "${i}")"
        IFS='|' read -r h hash work <<< "${t}"

        if [[ "${h}" != "${base_h}" || "${hash}" != "${base_hash}" || "${work}" != "${base_work}" ]]; then
            return 1
        fi
    done

    return 0
}

snapshot_all_nodes() {
    local i
    local s=""
    for i in 0 1 2 3 4; do
        s+="${i}:$(state_triplet_idx "${i}")|"
    done
    echo "${s}"
}

wait_nodes_converged() {
    local timeout="$1"
    local waited=0

    while [[ "${waited}" -lt "${timeout}" ]]; do
        local base_h base_hash base_work
        local base_triplet
        base_triplet="$(state_triplet_idx 0)"
        IFS='|' read -r base_h base_hash base_work <<< "${base_triplet}"

        if [[ "${base_h}" == "NA" || "${base_hash}" == "NA" ]]; then
            sleep 1
            waited=$((waited + 1))
            continue
        fi

        local all_equal=1
        local i
        for i in 1 2 3 4; do
            local t h hash work
            t="$(state_triplet_idx "${i}")"
            IFS='|' read -r h hash work <<< "${t}"

            if [[ "${h}" != "${base_h}" || "${hash}" != "${base_hash}" ]]; then
                all_equal=0
                break
            fi

            if [[ "${base_work}" != "NA" && "${work}" != "NA" && "${work}" != "${base_work}" ]]; then
                all_equal=0
                break
            fi
        done

        if [[ "${all_equal}" == "1" ]]; then
            return 0
        fi

        sleep 1
        waited=$((waited + 1))
    done

    return 1
}

wait_nodes_converged_with_progress() {
    local base_timeout="$1"
    local waited=0
    local no_progress=0
    local deadline="${base_timeout}"
    local previous_snapshot=""

    while [[ "${waited}" -lt "${HEAL_MAX_TIMEOUT}" && "${waited}" -lt "${deadline}" ]]; do
        if all_nodes_converged; then
            return 0
        fi

        local snapshot
        snapshot="$(snapshot_all_nodes)"
        if [[ "${snapshot}" != "${previous_snapshot}" ]]; then
            previous_snapshot="${snapshot}"
            no_progress=0

            local extended_deadline=$((waited + HEAL_PROGRESS_EXTENSION))
            if [[ "${extended_deadline}" -gt "${deadline}" ]]; then
                deadline="${extended_deadline}"
                if [[ "${deadline}" -gt "${HEAL_MAX_TIMEOUT}" ]]; then
                    deadline="${HEAL_MAX_TIMEOUT}"
                fi
            fi
        else
            no_progress=$((no_progress + 1))
            if [[ "${no_progress}" -ge "${HEAL_NO_PROGRESS_TIMEOUT}" ]]; then
                warn "Heal stalled for ${no_progress}s without state change"
                print_network_state
                print_peer_state
                print_leader_chaintips
                return 1
            fi
        fi

        sleep 1
        waited=$((waited + 1))
    done

    warn "Heal timed out after ${waited}s (deadline=${deadline}s)"
    print_network_state
    print_peer_state
    print_leader_chaintips
    return 1
}

wait_group_converged() {
    local timeout="$1"
    shift
    local members=("$@")

    local waited=0
    while [[ "${waited}" -lt "${timeout}" ]]; do
        local first_idx
        first_idx="$(idx_of "${members[0]}")"
        local base_triplet
        base_triplet="$(state_triplet_idx "${first_idx}")"

        local base_h base_hash base_work
        IFS='|' read -r base_h base_hash base_work <<< "${base_triplet}"

        local ok=1
        local m
        for m in "${members[@]}"; do
            local i t h hash work
            i="$(idx_of "${m}")"
            t="$(state_triplet_idx "${i}")"
            IFS='|' read -r h hash work <<< "${t}"

            if [[ "${h}" != "${base_h}" || "${hash}" != "${base_hash}" ]]; then
                ok=0
                break
            fi

            if [[ "${base_work}" != "NA" && "${work}" != "NA" && "${work}" != "${base_work}" ]]; then
                ok=0
                break
            fi
        done

        if [[ "${ok}" == "1" ]]; then
            return 0
        fi

        sleep 1
        waited=$((waited + 1))
    done

    return 1
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

submit_with_height_eval_idx() {
    local idx="$1" hex="$2"
    local before after

    before="$(rpc_try_scalar_idx "${idx}" "getblockcount" "[]" '.' "RPC_DOWN")"
    if [[ "${before}" == "RPC_DOWN" ]]; then
        echo "RPCERR|0|0|rpc_down"
        return 0
    fi

    local resp
    if ! resp="$(rpc_raw_idx "${idx}" "blockchain.submitblock" "[\"${hex}\"]" 2>/dev/null)"; then
        echo "RPCERR|${before}|${before}|rpc_submit_failed"
        return 0
    fi

    after="$(rpc_try_scalar_idx "${idx}" "getblockcount" "[]" '.' "${before}")"

    if echo "${resp}" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "${resp}" | jq -r '.error.message // (.error|tostring)')"
        echo "RPCERR|${before}|${after}|${emsg}"
        return 0
    fi

    if [[ "${after}" -gt "${before}" ]]; then
        echo "ACCEPT|${before}|${after}|connected"
    else
        echo "REJECT|${before}|${after}|not_connected"
    fi
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

scan_logs_for_fatal() {
    local i
    for i in 0 1 2 3 4; do
        local logf="${DATADIRS[$i]}/daemon.log"
        if [[ ! -f "${logf}" ]]; then
            continue
        fi
        local fatal_lines
        fatal_lines="$(
            grep -aE "ASSERTION FAILED|\\bFATAL\\b|Segmentation fault|AddressSanitizer|terminate called|panic:" "${logf}" \
                | grep -avE "\\[MINING ASSERT\\]" \
                || true
        )"
        if [[ -n "${fatal_lines}" ]]; then
            echo "Potential fatal signature found in ${logf}:" >&2
            echo "${fatal_lines}" | head -n 20 >&2
            fail "Fatal/assert signature detected in node logs"
        fi
    done
}

setup_mesh() {
    # Requested baseline mesh:
    # A<->B, A<->C, A<->D, A<->E, B<->C, C<->D
    connect_bidirectional 0 1
    connect_bidirectional 0 2
    connect_bidirectional 0 3
    connect_bidirectional 0 4
    connect_bidirectional 1 2
    connect_bidirectional 2 3
}

phase_bootstrap() {
    log_header "Phase 0 - Bootstrap"

    start_all_nodes 1

    ensure_wallet_idx 0

    info "Mining bootstrap blocks on node A (${BOOTSTRAP_BLOCKS})"
    mine_blocks_idx 0 "${BOOTSTRAP_BLOCKS}"

    # Connect A<->B after A has produced the bootstrap chain so B requests
    # the full header set in one shot instead of relying on unsolicited block relay.
    connect_bidirectional 0 1
    if ! wait_peer_min_idx 0 1; then
        fail "Anchor node A failed to reach minimum peer count during bootstrap"
    fi

    if ! wait_group_converged "${SYNC_TIMEOUT}" A B; then
        print_network_state
        fail "Bootstrap sync failed (A/B)"
    fi

    # Delay B wallet initialization until after A/B chain convergence.
    # This keeps bootstrap deterministic even if wallet init triggers
    # local chain activity on some builds.
    ensure_wallet_idx 1

    info "Funding node B wallet from node A"
    local b_funding_addr
    b_funding_addr="$(get_new_address_idx 1)"
    [[ -n "${b_funding_addr}" ]] || fail "Failed to obtain funding address on node B"

    send_to_address_idx 0 "${b_funding_addr}" "${BOOTSTRAP_SEND_AMOUNT}" >/dev/null
    mine_blocks_idx 0 2

    if ! wait_group_converged "${SYNC_TIMEOUT}" A B; then
        print_network_state
        fail "Post-funding sync failed (A/B)"
    fi

    rpc_result_idx 0 "wallet.rescanblockchain" "[0]" >/dev/null 2>&1 || true
    rpc_result_idx 1 "wallet.rescanblockchain" "[0]" >/dev/null 2>&1 || true

    # Join remaining peers C/D/E incrementally and wait for convergence after each join.
    info "Joining remaining peers C/D/E incrementally"
    connect_bidirectional 0 2
    if ! wait_group_converged "${SYNC_TIMEOUT}" A B C; then
        print_network_state
        fail "Bootstrap join failed after connecting C"
    fi
    connect_bidirectional 0 3
    if ! wait_group_converged "${SYNC_TIMEOUT}" A B C D; then
        print_network_state
        fail "Bootstrap join failed after connecting D"
    fi
    connect_bidirectional 0 4
    if ! wait_nodes_converged "${SYNC_TIMEOUT}"; then
        print_network_state
        fail "Bootstrap join failed after connecting E"
    fi

    # Add extra mesh edges after all peers are in sync.
    connect_bidirectional 1 2
    connect_bidirectional 2 3

    local bal_a bal_b
    bal_a="$(get_balance_idx 0)"
    bal_b="$(get_balance_idx 1)"
    info "Wallet balances: A=${bal_a}, B=${bal_b}"

    BASELINE_RSS_A="$(rss_kb_idx 0)"
    [[ "${BASELINE_RSS_A}" =~ ^[0-9]+$ ]] || BASELINE_RSS_A=0

    pass "Bootstrap complete"
}

phase1_inv_flood() {
    log_header "Phase 1 - Relay Flood"

    local pre_conn pre_h post_h post_conn
    pre_conn="$(connection_count_idx 0)"
    pre_h="$(rpc_scalar_idx 0 "getblockcount" "[]" '.')"

    local remaining="${PHASE1_BLOCKS}"
    local mined_total=0
    while [[ "${remaining}" -gt 0 ]]; do
        local batch="${PHASE1_BATCH_SIZE}"
        if [[ "${batch}" -le 0 ]]; then
            batch=1
        fi
        if [[ "${remaining}" -lt "${batch}" ]]; then
            batch="${remaining}"
        fi

        info "Node B mining rapid batch: ${batch} (remaining before batch=${remaining})"
        mine_blocks_idx 1 "${batch}"
        mined_total=$((mined_total + batch))
        remaining=$((remaining - batch))

        if ! wait_nodes_converged_with_progress "${SYNC_TIMEOUT}"; then
            print_network_state
            fail "Phase 1 convergence failure after batch size=${batch}, mined_total=${mined_total}"
        fi
    done

    post_h="$(rpc_scalar_idx 0 "getblockcount" "[]" '.')"
    post_conn="$(connection_count_idx 0)"

    if [[ "${post_h}" -lt $((pre_h + mined_total)) ]]; then
        fail "Phase 1 height growth too small: before=${pre_h} after=${post_h} expected_delta=${mined_total}"
    fi

    if [[ "${post_conn}" -gt $((pre_conn + PEER_LEAK_ALLOWANCE)) ]]; then
        fail "Phase 1 peer count leak on A: before=${pre_conn} after=${post_conn}"
    fi

    assert_nodes_alive
    assert_rss_not_runaway_idx 0 "${BASELINE_RSS_A}"

    pass "Phase 1 complete"
}

phase2_orphan_flood() {
    log_header "Phase 2 - Unknown Parent Flood"

    local tip_hash sample_hex
    tip_hash="$(rpc_scalar_idx 0 "getbestblockhash" "[]" '.')"
    sample_hex="$(rpc_result_idx 0 "getblock" "[\"${tip_hash}\",0]" | jq -r 'if type=="object" then (.hex // "") else . end')"
    [[ -n "${sample_hex}" && "${sample_hex}" != "null" ]] || fail "Could not obtain sample block hex"

    local before_h after_h accepts rejects
    before_h="$(rpc_scalar_idx 0 "getblockcount" "[]" '.')"
    accepts=0
    rejects=0

    info "Submitting ${PHASE2_ORPHAN_BLOCKS} unknown-parent blocks via RPC"

    local i
    for ((i=1; i<=PHASE2_ORPHAN_BLOCKS; i++)); do
        local prev_hex orphan_hex ev status
        prev_hex="$(printf "%064x" "$((0xA0000000 + i))")"

        # Header layout: 4-byte version (8 hex chars), then 32-byte prevhash (64 hex chars).
        orphan_hex="${sample_hex:0:8}${prev_hex}${sample_hex:72}"

        ev="$(submit_with_height_eval_idx 0 "${orphan_hex}")"
        IFS='|' read -r status _ _ _ <<< "${ev}"

        if [[ "${status}" == "ACCEPT" ]]; then
            accepts=$((accepts + 1))
        else
            rejects=$((rejects + 1))
        fi
    done

    after_h="$(rpc_scalar_idx 0 "getblockcount" "[]" '.')"

    info "Phase 2 results: accepts=${accepts} rejects=${rejects}"

    if [[ "${accepts}" -ne 0 ]]; then
        fail "Unknown-parent flood unexpectedly connected blocks"
    fi

    if [[ "${after_h}" -ne "${before_h}" ]]; then
        fail "Phase 2 height changed unexpectedly: before=${before_h} after=${after_h}"
    fi

    mine_blocks_idx 0 1
    if ! wait_nodes_converged "${SYNC_TIMEOUT}"; then
        print_network_state
        fail "Phase 2 post-valid-block convergence failure"
    fi

    assert_nodes_alive
    assert_rss_not_runaway_idx 0 "${BASELINE_RSS_A}"

    pass "Phase 2 complete"
}

phase3_disconnect_churn() {
    log_header "Phase 3 - Reconnect Churn"

    local i
    for ((i=1; i<=PHASE3_CHURN_LOOPS; i++)); do
        stop_node_idx 4   # E
        start_node_idx 4 0

        connect_bidirectional 0 4

        mine_blocks_idx 0 1

        if ! wait_nodes_converged "${CHURN_LOOP_SYNC_TIMEOUT}"; then
            print_network_state
            fail "Phase 3 convergence failed at loop ${i}"
        fi

        if (( i % 10 == 0 )); then
            info "Churn progress: ${i}/${PHASE3_CHURN_LOOPS}"
            assert_rss_not_runaway_idx 0 "${BASELINE_RSS_A}"
        fi

        assert_nodes_alive
    done

    pass "Phase 3 complete"
}

create_mempool_burst_idx() {
    local idx="$1" count="$2"
    local created=0

    local k
    for ((k=1; k<=count; k++)); do
        local addr
        addr="$(get_new_address_idx "${idx}" 2>/dev/null || true)"
        if [[ -z "${addr}" ]]; then
            continue
        fi

        if send_to_address_idx "${idx}" "${addr}" "${TX_AMOUNT}" >/dev/null 2>&1; then
            created=$((created + 1))
        fi
    done

    echo "${created}"
}

mempool_size_idx() {
    local idx="$1"
    rpc_result_idx "${idx}" "getrawmempool" "[]" | jq -r 'if type=="array" then length else 0 end'
}

phase4_and_phase5() {
    log_header "Phase 4/5 - Partition, Reorg Heal, Mempool"

    # Controlled restart to ensure no stale cross-partition links.
    stop_all_nodes
    start_all_nodes 0

    ensure_wallet_idx 0
    ensure_wallet_idx 1

    # Partition groups:
    # Group 1: A <-> C
    # Group 2: B <-> D, B <-> E, D <-> E
    connect_bidirectional 0 2
    connect_bidirectional 1 3
    connect_bidirectional 1 4
    connect_bidirectional 3 4

    if ! wait_group_converged "${GROUP_SYNC_TIMEOUT}" A C; then
        fail "Group 1 failed to converge before partition mining"
    fi
    if ! wait_group_converged "${GROUP_SYNC_TIMEOUT}" B D E; then
        fail "Group 2 failed to converge before partition mining"
    fi

    local mine_a="${PHASE4_BLOCKS_A}"
    local mine_b="${PHASE4_BLOCKS_B}"
    if [[ "${mine_b}" -le "${mine_a}" ]]; then
        mine_b=$((mine_a + PHASE4_WIN_MARGIN))
        warn "Adjusting partition winner margin: A=${mine_a}, B=${mine_b}"
    fi

    info "Partition mining: A-side=${mine_a}, B-side=${mine_b}"
    mine_blocks_idx 0 "${mine_a}"
    mine_blocks_idx 1 "${mine_b}"

    if ! wait_group_converged "${GROUP_SYNC_TIMEOUT}" A C; then
        fail "Group 1 failed to converge after partition mining"
    fi
    if ! wait_group_converged "${GROUP_SYNC_TIMEOUT}" B D E; then
        fail "Group 2 failed to converge after partition mining"
    fi

    local hash_a hash_b
    hash_a="$(rpc_scalar_idx 0 "getbestblockhash" "[]" '.')"
    hash_b="$(rpc_scalar_idx 1 "getbestblockhash" "[]" '.')"
    if [[ "${hash_a}" == "${hash_b}" ]]; then
        fail "Partition did not produce divergent tips (unexpected equal hashes)"
    fi

    info "Creating mempool activity on both partition sides"
    local a_created b_created
    a_created="$(create_mempool_burst_idx 0 "${PHASE5_TX_PER_SIDE}")"
    b_created="$(create_mempool_burst_idx 1 "${PHASE5_TX_PER_SIDE}")"

    info "Partition mempool tx created: A=${a_created}, B=${b_created}"
    if [[ "${a_created}" -eq 0 || "${b_created}" -eq 0 ]]; then
        fail "Insufficient partition mempool activity (A=${a_created}, B=${b_created})"
    fi

    local mp_a_before mp_b_before
    mp_a_before="$(mempool_size_idx 0)"
    mp_b_before="$(mempool_size_idx 1)"
    info "Partition mempool size: A=${mp_a_before}, B=${mp_b_before}"

    # Heal partition through anchor A plus direct winner-side fanout from B.
    info "Healing partition via anchor A and winner links from B"
    local pre_a_peers
    pre_a_peers="$(connection_count_idx 0)"
    [[ "${pre_a_peers}" =~ ^[0-9]+$ ]] || pre_a_peers=0
    connect_bidirectional 1 0
    if ! wait_peer_min_idx 0 $((pre_a_peers + 1)); then
        fail "Heal failed: anchor A did not connect to leader B"
    fi
    connect_bidirectional 2 0
    connect_bidirectional 3 0
    connect_bidirectional 4 0
    connect_bidirectional 2 1
    connect_bidirectional 3 1
    connect_bidirectional 4 1

    # Nudge relay paths from the winner side after heal links are in place.
    mine_blocks_idx 1 1

    if ! wait_nodes_converged_with_progress "${HEAL_BASE_TIMEOUT}"; then
        fail "Network failed to heal/reconverge"
    fi

    # Final block after heal to force reconciliation path on winning side.
    mine_blocks_idx 1 1
    if ! wait_nodes_converged_with_progress "${HEAL_BASE_TIMEOUT}"; then
        fail "Post-heal reconciliation block did not converge"
    fi

    # Ensure mempool RPC remains healthy and returns de-duplicated txids.
    local i
    for i in 0 1 2 3 4; do
        local raw total unique
        raw="$(rpc_result_idx "${i}" "getrawmempool" "[]")"
        total="$(echo "${raw}" | jq -r 'if type=="array" then length else 0 end')"
        unique="$(echo "${raw}" | jq -r 'if type=="array" then (unique | length) else 0 end')"
        if [[ "${total}" != "${unique}" ]]; then
            fail "Node $(name_of "${i}") mempool contains duplicate txids"
        fi
    done

    assert_nodes_alive
    assert_rss_not_runaway_idx 0 "${BASELINE_RSS_A}"

    pass "Phase 4/5 complete"
}

final_gate() {
    log_header "Final Gate"

    assert_nodes_alive

    if ! wait_nodes_converged "${SYNC_TIMEOUT}"; then
        print_network_state
        fail "Final convergence check failed"
    fi

    scan_logs_for_fatal

    local final_h final_hash final_work
    final_h="$(rpc_scalar_idx 0 "getblockcount" "[]" '.')"
    final_hash="$(rpc_scalar_idx 0 "getbestblockhash" "[]" '.')"
    final_work="$(rpc_result_idx 0 "getblockchaininfo" "[]" | jq -r '.chainwork // "NA"')"

    echo "FINAL_HEIGHT=${final_h}"
    echo "FINAL_TIP_HASH=${final_hash}"
    echo "FINAL_CHAINWORK=${final_work}"
    echo "WORKDIR=${WORKDIR}"
    echo "RSS_NODE_A_KB=$(rss_kb_idx 0)"

    print_network_state
    pass "P2P storm gate passed"
}

main() {
    log_header "P2P Storm Harness"
    info "Nodes: ${NODE_NAMES[*]}"
    info "Ports: RPC base=${BASE_RPC_PORT}, P2P base=${BASE_P2P_PORT}"
    info "Workdir: ${WORKDIR}"

    phase_bootstrap
    phase1_inv_flood
    phase2_orphan_flood
    phase3_disconnect_churn
    phase4_and_phase5
    final_gate
}

main "$@"
