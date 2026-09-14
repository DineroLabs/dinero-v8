#!/usr/bin/env bash
#
# Regtest reproduction for issue #738 — "node on a minority tip never receives
# competing headers from directly connected peers".
#
# Field shape (mainnet, 2026-09-13): SJ sat on its own self-mined 4-block branch
# while its long-lived INBOUND connections from NA/EU1 (12+ blocks ahead on a
# heavier branch) stayed up the whole time. getchaintips on SJ showed exactly
# ONE tip; headers == blocks == its own height; getpeerinfo heights for those
# peers were stale on both sides. Manually submitblock-ing the competing bodies
# unwedged it — relay and body fetch work once the fork is bridged; what never
# happens is the initial delivery of the competing headers.
#
# Two families of variants, each on fresh datadirs with two real regtest nodes
# (A = the node that ends up on the minority tip, B = the healthy peer). In every
# variant A mines its own branch to its OWN wallet, B is the heavier chain.
#
#  reconnect-*  (issue's literal repro suggestion) A<->B connected & synced;
#               setnetworkactive false on both; A mines MINORITY, B mines
#               MAJORITY; heal + reconnect. reconnect-outbound: A dials B.
#               reconnect-inbound: B dials A (A sees B as INBOUND).
#               NOTE: the heal produces a fresh handshake, whose on-connect
#               getheaders pull is a path the field node never had.
#
#  live-*       The connection is NEVER dropped (field condition). B dials A.
#               B mines BASE, A syncs. A mines MINORITY (B follows). B then
#               invalidateblock()s A's first fork block and mines MAJORITY on
#               the common history, so B is on a heavier competing branch
#               while still connected to A.
#    live-race              B's pushes on. Control: A must reorg promptly.
#    live-muted             B started with DINERO_TEST_SUPPRESS_ANNOUNCEMENTS=1
#                           (the regtest hook from the #214 test: B never pushes
#                           inv/cmpctblock but still ANSWERS getheaders). A runs
#                           with a short stale-tip threshold. A mines its branch
#                           once, then idles. Control: #214 stale-tip recovery
#                           must re-probe B and A must reorg.
#    live-muted-selfmining  Same as live-muted, but A keeps mining its own
#                           branch (one block every SELFMINE_INTERVAL s, below
#                           the staleness threshold) while B keeps extending its
#                           heavier branch faster — the field pool. A must still
#                           discover B's chain within SELFMINE_WINDOW s.
#
# Hard assertions on A (a variant fails if any does not hold):
#   1. A converged onto B's branch (tip equality; for the self-mining variant,
#      A's block at the first fork height equals B's)
#   2. getchaintips(A) lists B's tip with status "active"
#   3. getpeerinfo(A) for peer B: max(best_known_height, synced_headers) has
#      advanced to at least B's branch height (heights not stale)
# Reported, not asserted: whether getchaintips(A) still lists A's abandoned
# minority tip as "valid-fork" (currently it is dropped entirely; unrelated to
# the relay defect, so it must not be what fails this test).
#
# PASS = every variant satisfies all hard assertions. FAIL (exit 1) = #738
# reproduced; failing variant, assertion and both nodes' relevant log lines
# are printed. Exit 2 = harness/setup error (not a verdict).
#
# Tunables via env: BASE (8), MINORITY (4), MAJORITY (6), CONVERGE_WAIT (45),
# SELFMINE_INTERVAL (4), SELFMINE_WINDOW (48), STALE_THRESHOLD_SECS (10),
# VARIANTS (all five, space separated), KEEP_ON_FAIL (1).
#
# Requires: curl, jq. DINEROD env or ${ROOT_DIR}/build/dinerod.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=lib/port_alloc.sh
source "${SCRIPT_DIR}/lib/port_alloc.sh"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${SCRIPT_DIR}/helpers/daemon_process_cleanup.sh"

if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 2; }
else
    DINEROD="${ROOT_DIR}/build/dinerod"
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD}); set DINEROD=/path/to/dinerod" >&2
        exit 2
    }
fi
command -v curl >/dev/null || { echo "curl is required" >&2; exit 2; }
command -v jq   >/dev/null || { echo "jq is required" >&2; exit 2; }

BASE="${BASE:-8}"
MINORITY="${MINORITY:-4}"
MAJORITY="${MAJORITY:-6}"
CONVERGE_WAIT="${CONVERGE_WAIT:-45}"
SELFMINE_INTERVAL="${SELFMINE_INTERVAL:-4}"
SELFMINE_WINDOW="${SELFMINE_WINDOW:-48}"
STALE_THRESHOLD_SECS="${STALE_THRESHOLD_SECS:-10}"
VARIANTS="${VARIANTS:-reconnect-outbound reconnect-inbound live-race live-muted live-muted-selfmining}"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-1}"

if (( MAJORITY <= MINORITY )); then
    echo "MAJORITY (${MAJORITY}) must exceed MINORITY (${MINORITY})" >&2
    exit 2
fi
if (( SELFMINE_INTERVAL >= STALE_THRESHOLD_SECS )); then
    echo "SELFMINE_INTERVAL (${SELFMINE_INTERVAL}) must be below STALE_THRESHOLD_SECS (${STALE_THRESHOLD_SECS})" >&2
    exit 2
fi

PIDS=()
DIRS=()
FAILED_VARIANTS=()
TEST_FAILED=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
setup_error() { printf '[SETUP-ERROR] %s\n' "$*" >&2; exit 2; }

cleanup() {
    local rc=0
    for p in "${PIDS[@]:-}"; do
        [[ -n "${p}" ]] && { dinero_stop_process "${p}" "dinerod" || rc=1; }
    done
    for d in "${DIRS[@]:-}"; do
        [[ -n "${d}" ]] && { dinero_stop_datadir_processes "${d}" || rc=1; }
    done
    if [[ "${TEST_FAILED}" == "1" && "${KEEP_ON_FAIL}" == "1" ]]; then
        printf '[INFO] datadirs/logs retained for inspection:\n'
        for d in "${DIRS[@]:-}"; do [[ -n "${d}" ]] && printf '       %s (log: %s.log)\n' "${d}" "${d}"; done
    else
        for d in "${DIRS[@]:-}"; do [[ -n "${d}" ]] && rm -rf "${d}" "${d}.log"; done
    fi
    return "${rc}"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# RPC + node helpers (same cookie-auth curl pattern as the other two-node
# integration scripts: test_p2p_staleness_recovery.sh, test_compact_block_relay_e2e.sh)
# ---------------------------------------------------------------------------
cookie_file() {
    local datadir="$1"
    [[ -f "${datadir}/.cookie" ]] && { printf '%s\n' "${datadir}/.cookie"; return 0; }
    [[ -f "${datadir}/regtest/.cookie" ]] && { printf '%s\n' "${datadir}/regtest/.cookie"; return 0; }
    return 1
}

# rpc_call RPC_PORT DATADIR METHOD PARAMS_JSON -> full JSON-RPC envelope
rpc_call() {
    local rpc_port="$1" datadir="$2" method="$3" params_json="${4:-[]}"
    local cookie_path cookie
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --max-time 10 --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${rpc_port}/"
}

# rpc RPC_PORT DATADIR METHOD [PARAMS_JSON] -> .result only (raw jq -r)
rpc() { rpc_call "$@" | jq -r '.result'; }

# start_node DATADIR RPC_PORT P2P_PORT STRATUM_PORT WALLET_PORT [ENV_KV ...] -> echoes PID
start_node() {
    local datadir="$1" rpc_port="$2" p2p_port="$3" stratum_port="$4" wallet_port="$5"
    shift 5
    mkdir -p "${datadir}"
    env "$@" "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --stratumport="${stratum_port}" \
        --wallet-socket-port="${wallet_port}" \
        --listen=1 \
        >"${datadir}.log" 2>&1 &
    printf '%s\n' "$!"
}

wait_rpc() {
    local rpc_port="$1" datadir="$2"
    for _ in $(seq 1 60); do
        if rpc_call "${rpc_port}" "${datadir}" getblockcount '[]' 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# wait_for "shell condition" TRIES  -> 0 when it became true, 1 on timeout (1s poll)
wait_for() {
    local cmd="$1" tries="${2:-60}"
    for _ in $(seq 1 "${tries}"); do
        if eval "${cmd}"; then return 0; fi
        sleep 1
    done
    return 1
}

height_of()  { rpc "$1" "$2" getblockcount; }
tip_of()     { rpc "$1" "$2" getbestblockhash; }
conns_of()   { rpc "$1" "$2" getconnectioncount; }
hash_at()    { rpc "$1" "$2" getblockhash "[$3]"; }
mine()       { rpc_call "$1" "$2" generatetoaddress "[$3,\"$4\"]" >/dev/null; }

# peer_field RPC DATADIR FIELD -> FIELD of the node's (single) connected peer.
# Each topology has exactly one A<->B link, and an INBOUND peer shows up with
# an ephemeral source port, so we cannot key on the remote P2P port; pick the
# connected entry. `// empty` would swallow a legitimate `false`, hence the if.
peer_field() {
    local rpc_port="$1" datadir="$2" field="$3"
    rpc "${rpc_port}" "${datadir}" getpeerinfo \
        | jq -r --arg f "${field}" \
            '[.[] | select(.connected != false)] | first | if . == null then empty else .[$f] end'
}

tip_status() { # RPC DATADIR HASH -> status string from getchaintips or "absent"
    rpc "$1" "$2" getchaintips | jq -r --arg h "$3" '[.[] | select(.hash == $h)] | first | .status // "absent"'
}

dump_diagnostics() {
    local tag="$1" a_rpc="$2" a_dir="$3" b_rpc="$4" b_dir="$5"
    printf -- '---- [%s] A getchaintips ----\n' "${tag}"
    rpc "${a_rpc}" "${a_dir}" getchaintips | jq -c '.[]' 2>/dev/null || true
    printf -- '---- [%s] A getblockchaininfo ----\n' "${tag}"
    rpc "${a_rpc}" "${a_dir}" getblockchaininfo | jq -c '{blocks, headers, bestblockhash, initialblockdownload}' 2>/dev/null || true
    printf -- '---- [%s] A getpeerinfo (height fields) ----\n' "${tag}"
    rpc "${a_rpc}" "${a_dir}" getpeerinfo \
        | jq -c '.[] | {addr, inbound, connected, startingheight, best_known_height, synced_headers, synced_blocks, compactblocks_announce}' 2>/dev/null || true
    printf -- '---- [%s] B getpeerinfo (height fields) ----\n' "${tag}"
    rpc "${b_rpc}" "${b_dir}" getpeerinfo \
        | jq -c '.[] | {addr, inbound, connected, startingheight, best_known_height, synced_headers, synced_blocks, compactblocks_announce}' 2>/dev/null || true
    printf -- '---- [%s] A log: header-sync / announce / stale-tip lines (last 40) ----\n' "${tag}"
    grep -aE 'getheaders sent|getheaders not sent|Requesting headers|reports higher tip|Stale tip|Sending [0-9]+ headers|Received GETHEADERS|No new headers|OnInv received|Broadcast (cmpctblock|inv)|REORG DETECTED|suppressing announcement' \
        "${a_dir}.log" 2>/dev/null | tail -40 || true
    printf -- '---- [%s] B log: header-sync / announce lines (last 25) ----\n' "${tag}"
    grep -aE 'getheaders sent|getheaders not sent|Requesting headers|reports higher tip|Stale tip|Sending [0-9]+ headers|Received GETHEADERS|No new headers|OnInv received|Broadcast (cmpctblock|inv)|REORG DETECTED|suppressing announcement' \
        "${b_dir}.log" 2>/dev/null | tail -25 || true
}

# ---------------------------------------------------------------------------
# Assertions shared by every variant.
# check_outcome VARIANT A_RPC A_DIR B_RPC B_DIR A_OLD_TIP B_FORK1 B_BRANCH_H CONVERGED(0/1)
# ---------------------------------------------------------------------------
check_outcome() {
    local variant="$1" a_rpc="$2" a_dir="$3" b_rpc="$4" b_dir="$5"
    local a_old_tip="$6" b_fork1="$7" b_branch_h="$8" converged="$9"
    local failures=0
    local a_tip a_h b_tip b_h
    a_tip="$(tip_of "${a_rpc}" "${a_dir}")"; a_h="$(height_of "${a_rpc}" "${a_dir}")"
    b_tip="$(tip_of "${b_rpc}" "${b_dir}")"; b_h="$(height_of "${b_rpc}" "${b_dir}")"

    # 1. converged onto B's branch
    if [[ "${converged}" == "1" ]]; then
        pass "[${variant}] A converged onto B's branch (A ${a_tip:0:16}… @${a_h}; B ${b_tip:0:16}… @${b_h})"
    else
        printf '[FAIL] [%s] A did NOT converge onto B'\''s branch: A tip=%s @%s (fork block @%s: %s…), B tip=%s @%s (fork block: %s…)\n' \
            "${variant}" "${a_tip}" "${a_h}" "$((BASE + 1))" "$(hash_at "${a_rpc}" "${a_dir}" $((BASE + 1)) | cut -c1-16)" \
            "${b_tip}" "${b_h}" "${b_fork1:0:16}"
        failures=$((failures + 1))
    fi

    # 2. getchaintips(A): B's tip active (+ report the old-tip status)
    local st_b st_old ntips
    st_b="$(tip_status "${a_rpc}" "${a_dir}" "${b_tip}")"
    st_old="$(tip_status "${a_rpc}" "${a_dir}" "${a_old_tip}")"
    ntips="$(rpc "${a_rpc}" "${a_dir}" getchaintips | jq -r length)"
    if [[ "${st_b}" == "active" ]]; then
        pass "[${variant}] getchaintips(A): B tip ${b_tip:0:16}… status=active (${ntips} tip(s))"
    else
        printf '[FAIL] [%s] getchaintips(A) has %s tip(s); B tip %s… status=%s (want active)\n' \
            "${variant}" "${ntips}" "${b_tip:0:16}" "${st_b}"
        failures=$((failures + 1))
    fi
    info "[${variant}] getchaintips(A): A's abandoned minority tip ${a_old_tip:0:16}… status=${st_old} (expected valid-fork; reported only)"

    # 3. A's view of peer B's height reached B's branch height
    local bk sh known
    bk="$(peer_field "${a_rpc}" "${a_dir}" best_known_height)"; bk="${bk:-0}"
    sh="$(peer_field "${a_rpc}" "${a_dir}" synced_headers)";    sh="${sh:-0}"
    known=$(( bk > sh ? bk : sh ))
    if (( known >= b_branch_h )); then
        pass "[${variant}] getpeerinfo(A) peer B: best_known_height=${bk} synced_headers=${sh} (>= ${b_branch_h})"
    else
        printf '[FAIL] [%s] getpeerinfo(A) peer B height is STALE: best_known_height=%s synced_headers=%s, B branch height %s (B now @%s)\n' \
            "${variant}" "${bk}" "${sh}" "${b_branch_h}" "${b_h}"
        failures=$((failures + 1))
    fi

    if (( failures > 0 )); then
        TEST_FAILED=1
        FAILED_VARIANTS+=("${variant}(${failures} assertion(s))")
        dump_diagnostics "${variant}" "${a_rpc}" "${a_dir}" "${b_rpc}" "${b_dir}"
    fi
}

# ---------------------------------------------------------------------------
# One variant = one fresh two-node topology.
# ---------------------------------------------------------------------------
run_variant() {
    local variant="$1"
    local base_port a_rpc b_rpc a_p2p b_p2p a_strat b_strat a_wal b_wal
    base_port="$(alloc_port_base 8)" || setup_error "[${variant}] could not allocate ports"
    a_rpc=$((base_port + 0)); b_rpc=$((base_port + 1))
    a_p2p=$((base_port + 2)); b_p2p=$((base_port + 3))
    a_strat=$((base_port + 4)); b_strat=$((base_port + 5))
    a_wal=$((base_port + 6)); b_wal=$((base_port + 7))

    local a_dir b_dir
    a_dir="$(mktemp -d "${TMPDIR:-/tmp}/dinero_738_${variant}_A_XXXXXX")"
    b_dir="$(mktemp -d "${TMPDIR:-/tmp}/dinero_738_${variant}_B_XXXXXX")"
    DIRS+=("${a_dir}" "${b_dir}")

    local -a a_env=() b_env=()
    case "${variant}" in
        reconnect-outbound|reconnect-inbound|live-race) ;;
        live-muted|live-muted-selfmining)
            a_env=("DINERO_TEST_STALENESS_THRESHOLD_SECS=${STALE_THRESHOLD_SECS}"
                   "DINERO_TEST_STALENESS_GETHEADERS_INTERVAL_SECS=2")
            b_env=("DINERO_TEST_SUPPRESS_ANNOUNCEMENTS=1")
            ;;
        *) setup_error "unknown variant '${variant}'" ;;
    esac

    info "=== variant '${variant}': A rpc=${a_rpc} p2p=${a_p2p} env=[${a_env[*]:-}] | B rpc=${b_rpc} p2p=${b_p2p} env=[${b_env[*]:-}] ==="

    local a_pid b_pid
    a_pid="$(start_node "${a_dir}" "${a_rpc}" "${a_p2p}" "${a_strat}" "${a_wal}" ${a_env[@]+"${a_env[@]}"})"
    b_pid="$(start_node "${b_dir}" "${b_rpc}" "${b_p2p}" "${b_strat}" "${b_wal}" ${b_env[@]+"${b_env[@]}"})"
    PIDS+=("${a_pid}" "${b_pid}")
    wait_rpc "${a_rpc}" "${a_dir}" || { tail -40 "${a_dir}.log" >&2; setup_error "[${variant}] node A RPC did not come up"; }
    wait_rpc "${b_rpc}" "${b_dir}" || { tail -40 "${b_dir}.log" >&2; setup_error "[${variant}] node B RPC did not come up"; }

    # Each node mines to its OWN wallet (the field stuck node self-mined its branch).
    local addr_a addr_b
    addr_a="$(rpc_call "${a_rpc}" "${a_dir}" wallet.createhd '["miner-a"]' | jq -r '.result.first_address // empty')"
    addr_b="$(rpc_call "${b_rpc}" "${b_dir}" wallet.createhd '["miner-b"]' | jq -r '.result.first_address // empty')"
    [[ -n "${addr_a}" && -n "${addr_b}" ]] || setup_error "[${variant}] could not create wallets (A='${addr_a}' B='${addr_b}')"

    connect_ab() {
        case "${variant}" in
            reconnect-outbound) rpc_call "${a_rpc}" "${a_dir}" addnode "[\"127.0.0.1:${b_p2p}\",\"onetry\"]" >/dev/null ;;
            *)                  rpc_call "${b_rpc}" "${b_dir}" addnode "[\"127.0.0.1:${a_p2p}\",\"onetry\"]" >/dev/null ;;
        esac
    }
    wait_connected() {
        wait_for "[[ \$(conns_of ${a_rpc} ${a_dir}) -ge 1 && \$(conns_of ${b_rpc} ${b_dir}) -ge 1 ]]" 30
    }

    # --- common history: B mines BASE BEFORE connecting so a muted B still
    #     gets A synced via A's on-connect getheaders pull (same as the #214 test).
    mine "${b_rpc}" "${b_dir}" "${BASE}" "${addr_b}"
    wait_for "[[ \$(height_of ${b_rpc} ${b_dir}) -eq ${BASE} ]]" 30 || setup_error "[${variant}] B did not mine to ${BASE}"
    connect_ab
    wait_connected || setup_error "[${variant}] A and B never connected"
    wait_for "[[ \$(tip_of ${a_rpc} ${a_dir}) == \$(tip_of ${b_rpc} ${b_dir}) ]]" 60 \
        || setup_error "[${variant}] A did not sync common history to ${BASE} (A=$(height_of "${a_rpc}" "${a_dir}"))"
    info "[${variant}] common history @${BASE} tip $(tip_of "${a_rpc}" "${a_dir}" | cut -c1-16)…; A sees B as inbound=$(peer_field "${a_rpc}" "${a_dir}" inbound)"

    local a_old_tip b_tip b_fork1 a_branch_h=$((BASE + MINORITY)) b_branch_h=$((BASE + MAJORITY)) converged=0

    case "${variant}" in
    reconnect-*)
        # --- partition ---
        rpc_call "${a_rpc}" "${a_dir}" setnetworkactive '[false]' >/dev/null
        rpc_call "${b_rpc}" "${b_dir}" setnetworkactive '[false]' >/dev/null
        wait_for "[[ \$(conns_of ${a_rpc} ${a_dir}) -eq 0 && \$(conns_of ${b_rpc} ${b_dir}) -eq 0 ]]" 30 \
            || setup_error "[${variant}] partition did not drop the A<->B connection"
        # --- diverge ---
        mine "${a_rpc}" "${a_dir}" "${MINORITY}" "${addr_a}"
        mine "${b_rpc}" "${b_dir}" "${MAJORITY}" "${addr_b}"
        wait_for "[[ \$(height_of ${a_rpc} ${a_dir}) -eq ${a_branch_h} ]]" 30 || setup_error "[${variant}] A did not mine its ${MINORITY}-block branch"
        wait_for "[[ \$(height_of ${b_rpc} ${b_dir}) -eq ${b_branch_h} ]]" 30 || setup_error "[${variant}] B did not mine its ${MAJORITY}-block branch"
        a_old_tip="$(tip_of "${a_rpc}" "${a_dir}")"; b_tip="$(tip_of "${b_rpc}" "${b_dir}")"
        b_fork1="$(hash_at "${b_rpc}" "${b_dir}" $((BASE + 1)))"
        [[ "${a_old_tip}" != "${b_tip}" ]] || setup_error "[${variant}] branches did not diverge"
        info "[${variant}] diverged: A(minority) ${a_old_tip:0:16}… @${a_branch_h}  B(majority) ${b_tip:0:16}… @${b_branch_h}"
        # --- heal ---
        rpc_call "${a_rpc}" "${a_dir}" setnetworkactive '[true]' >/dev/null
        rpc_call "${b_rpc}" "${b_dir}" setnetworkactive '[true]' >/dev/null
        connect_ab
        wait_connected || setup_error "[${variant}] A and B did not reconnect after heal"
        info "[${variant}] healed (A sees B inbound=$(peer_field "${a_rpc}" "${a_dir}" inbound)); waiting up to ${CONVERGE_WAIT}s for A to reorg…"
        wait_for "[[ \$(tip_of ${a_rpc} ${a_dir}) == ${b_tip} ]]" "${CONVERGE_WAIT}" && converged=1
        ;;

    live-*)
        # --- A mines its own branch while connected; B follows it (A's pushes are on).
        mine "${a_rpc}" "${a_dir}" "${MINORITY}" "${addr_a}"
        wait_for "[[ \$(height_of ${a_rpc} ${a_dir}) -eq ${a_branch_h} ]]" 30 || setup_error "[${variant}] A did not mine its ${MINORITY}-block branch"
        wait_for "[[ \$(tip_of ${b_rpc} ${b_dir}) == \$(tip_of ${a_rpc} ${a_dir}) ]]" 60 \
            || setup_error "[${variant}] B did not follow A's branch to ${a_branch_h} (B=$(height_of "${b_rpc}" "${b_dir}"))"
        a_old_tip="$(tip_of "${a_rpc}" "${a_dir}")"
        local a_fork1; a_fork1="$(hash_at "${a_rpc}" "${a_dir}" $((BASE + 1)))"
        # --- B abandons A's branch locally and builds the heavier competing branch,
        #     connection to A untouched.
        rpc_call "${b_rpc}" "${b_dir}" invalidateblock "[\"${a_fork1}\"]" >/dev/null
        wait_for "[[ \$(height_of ${b_rpc} ${b_dir}) -eq ${BASE} ]]" 30 || setup_error "[${variant}] B did not roll back to ${BASE} after invalidateblock"
        mine "${b_rpc}" "${b_dir}" "${MAJORITY}" "${addr_b}"
        wait_for "[[ \$(height_of ${b_rpc} ${b_dir}) -eq ${b_branch_h} ]]" 30 || setup_error "[${variant}] B did not mine its ${MAJORITY}-block branch"
        b_tip="$(tip_of "${b_rpc}" "${b_dir}")"; b_fork1="$(hash_at "${b_rpc}" "${b_dir}" $((BASE + 1)))"
        [[ "${b_fork1}" != "${a_fork1}" ]] || setup_error "[${variant}] B did not fork away from A's branch"
        (( $(conns_of "${a_rpc}" "${a_dir}") >= 1 )) || setup_error "[${variant}] A<->B link dropped during divergence (must stay up)"
        info "[${variant}] diverged on a LIVE link: A(minority) ${a_old_tip:0:16}… @${a_branch_h}  B(majority) ${b_tip:0:16}… @${b_branch_h}; A sees B inbound=$(peer_field "${a_rpc}" "${a_dir}" inbound)"

        if [[ "${variant}" == "live-muted-selfmining" ]]; then
            # The field pool: A keeps extending its own branch faster than the
            # stale-tip threshold; B keeps extending the heavier branch (2:1).
            local rounds=$(( SELFMINE_WINDOW / SELFMINE_INTERVAL )) r
            info "[${variant}] self-mining window: ${rounds} rounds x ${SELFMINE_INTERVAL}s (A +1, B +2 per round; threshold ${STALE_THRESHOLD_SECS}s)"
            for (( r = 1; r <= rounds; r++ )); do
                mine "${a_rpc}" "${a_dir}" 1 "${addr_a}"
                mine "${b_rpc}" "${b_dir}" 2 "${addr_b}"
                sleep "${SELFMINE_INTERVAL}"
                if [[ "$(hash_at "${a_rpc}" "${a_dir}" $((BASE + 1)))" == "${b_fork1}" ]]; then
                    converged=1
                    info "[${variant}] A switched to B's branch during round ${r}"
                    break
                fi
            done
            (( $(conns_of "${a_rpc}" "${a_dir}") >= 1 )) || setup_error "[${variant}] A<->B link dropped during the self-mining window"
            info "[${variant}] after window: A @$(height_of "${a_rpc}" "${a_dir}") headers=$(rpc "${a_rpc}" "${a_dir}" getblockchaininfo | jq -r .headers) conns=$(conns_of "${a_rpc}" "${a_dir}"), B @$(height_of "${b_rpc}" "${b_dir}"); converged=${converged}"
            b_tip="$(tip_of "${b_rpc}" "${b_dir}")"
        else
            info "[${variant}] waiting up to ${CONVERGE_WAIT}s for A to reorg over the live link…"
            wait_for "[[ \$(tip_of ${a_rpc} ${a_dir}) == ${b_tip} ]]" "${CONVERGE_WAIT}" && converged=1
            [[ "${variant}" == "live-muted" ]] && info "[${variant}] 'Stale tip' recovery log lines on A: $(grep -ac 'Stale tip' "${a_dir}.log")"
        fi
        ;;
    esac

    check_outcome "${variant}" "${a_rpc}" "${a_dir}" "${b_rpc}" "${b_dir}" "${a_old_tip}" "${b_fork1}" "${b_branch_h}" "${converged}"

    if [[ "${variant}" == "live-muted-selfmining" && "${converged}" == "0" ]]; then
        # Diagnostic only (after the verdict): does A recover once it STOPS
        # mining, i.e. is the #214 stale-tip recovery merely blind while A's
        # own header tip keeps advancing?
        local post_wait=$(( STALE_THRESHOLD_SECS + 20 ))
        if wait_for "[[ \$(hash_at ${a_rpc} ${a_dir} $((BASE + 1))) == ${b_fork1} ]]" "${post_wait}"; then
            info "[${variant}] DIAG: A converged within ${post_wait}s AFTER self-mining stopped ('Stale tip' recovery log lines on A: $(grep -ac 'Stale tip' "${a_dir}.log")) — recovery is blind while A's own tip advances"
        else
            info "[${variant}] DIAG: A still not converged ${post_wait}s after self-mining stopped ('Stale tip' log lines on A: $(grep -ac 'Stale tip' "${a_dir}.log"))"
        fi
    fi

    # Stop this variant's nodes before the next one (fresh topology per variant).
    dinero_stop_process "${a_pid}" "node A (${variant})" || true
    dinero_stop_process "${b_pid}" "node B (${variant})" || true
}

for v in ${VARIANTS}; do
    run_variant "${v}"
done

echo
if [[ "${TEST_FAILED}" == "1" ]]; then
    printf '=== FAIL: #738 reproduced in variant(s): %s ===\n' "${FAILED_VARIANTS[*]}"
    exit 1
fi
printf '=== PASS: all variants (%s) converged with B active in chaintips and fresh peer heights ===\n' "${VARIANTS}"
exit 0
