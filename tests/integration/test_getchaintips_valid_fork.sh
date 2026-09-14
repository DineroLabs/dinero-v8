#!/usr/bin/env bash
#
# Regtest gate for issue #741 — "getchaintips never lists abandoned branches as
# valid-fork".
#
# After a reorg the RPC returned exactly one tip (the active one). The branch
# the node had just abandoned vanished instead of being reported with
# status "valid-fork" and its branch length (Bitcoin Core semantics), so an
# operator could not tell "never heard of the other chain" from "saw it and
# switched away". Root cause: the handler enumerated GetCandidateTipsSnapshot(),
# which only holds tips that can still BECOME active — a tip is erased from the
# candidate set when it is activated and never re-added when a reorg abandons it.
#
# Topology (two real regtest nodes, fresh datadirs, same harness pattern as
# test_minority_tip_header_relay.sh):
#
#   1. B mines BASE, A connects and syncs (common history).
#   2. Partition (setnetworkactive false on both).
#   3. A mines SIDE (=2) blocks on its own branch; B mines MAJORITY (=3).
#   4. Heal and reconnect; A must reorg onto B's heavier branch.
#
# Hard assertions on getchaintips(A) after the reorg:
#   a. B's tip is listed with status "active", branchlen 0, height BASE+MAJORITY.
#   b. A's abandoned tip is listed with status "valid-fork", branchlen SIDE,
#      height BASE+SIDE, and a 64-hex chainwork.
#   c. After `invalidateblock` on A's first side-branch block the same tip is
#      reported as "invalid" (branchlen SIDE); after `reconsiderblock` it is
#      "valid-fork" again. The active tip is unaffected throughout.
#
# Reported, not asserted: getchaintips(B) (B may or may not have learned A's
# headers across the heal).
#
# PASS = all assertions hold. FAIL (exit 1) = #741 reproduced. Exit 2 = harness
# or setup error (not a verdict).
#
# Tunables via env: BASE (6), SIDE (2), MAJORITY (3), CONVERGE_WAIT (60),
# KEEP_ON_FAIL (1). Requires: curl, jq. DINEROD env or ${ROOT_DIR}/build/dinerod.

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

BASE="${BASE:-6}"
SIDE="${SIDE:-2}"
MAJORITY="${MAJORITY:-3}"
CONVERGE_WAIT="${CONVERGE_WAIT:-60}"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-1}"

if (( MAJORITY <= SIDE )); then
    echo "MAJORITY (${MAJORITY}) must exceed SIDE (${SIDE})" >&2
    exit 2
fi

PIDS=()
DIRS=()
FAILURES=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*"; FAILURES=$((FAILURES + 1)); }
setup_error() { printf '[SETUP-ERROR] %s\n' "$*" >&2; exit 2; }

cleanup() {
    local rc=0
    for p in "${PIDS[@]:-}"; do
        [[ -n "${p}" ]] && { dinero_stop_process "${p}" "dinerod" || rc=1; }
    done
    for d in "${DIRS[@]:-}"; do
        [[ -n "${d}" ]] && { dinero_stop_datadir_processes "${d}" || rc=1; }
    done
    if (( FAILURES > 0 )) && [[ "${KEEP_ON_FAIL}" == "1" ]]; then
        printf '[INFO] datadirs/logs retained for inspection:\n'
        for d in "${DIRS[@]:-}"; do [[ -n "${d}" ]] && printf '       %s (log: %s.log)\n' "${d}" "${d}"; done
    else
        for d in "${DIRS[@]:-}"; do [[ -n "${d}" ]] && rm -rf "${d}" "${d}.log"; done
    fi
    return "${rc}"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# RPC + node helpers (cookie-auth curl, as in the other two-node scripts)
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

# start_node DATADIR RPC_PORT P2P_PORT STRATUM_PORT WALLET_PORT -> echoes PID
start_node() {
    local datadir="$1" rpc_port="$2" p2p_port="$3" stratum_port="$4" wallet_port="$5"
    mkdir -p "${datadir}"
    "${DINEROD}" \
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

# wait_for "shell condition" TRIES -> 0 when it became true, 1 on timeout (1s poll)
wait_for() {
    local cmd="$1" tries="${2:-60}"
    for _ in $(seq 1 "${tries}"); do
        if eval "${cmd}"; then return 0; fi
        sleep 1
    done
    return 1
}

height_of() { rpc "$1" "$2" getblockcount; }
tip_of()    { rpc "$1" "$2" getbestblockhash; }
conns_of()  { rpc "$1" "$2" getconnectioncount; }
hash_at()   { rpc "$1" "$2" getblockhash "[$3]"; }
mine()      { rpc_call "$1" "$2" generatetoaddress "[$3,\"$4\"]" >/dev/null; }

# tip_entry RPC DATADIR HASH -> compact JSON of the getchaintips entry for HASH, or "null"
tip_entry() {
    rpc "$1" "$2" getchaintips | jq -c --arg h "$3" '[.[] | select(.hash == $h)] | first // null'
}
# tip_field RPC DATADIR HASH FIELD -> field value or "absent"
tip_field() {
    tip_entry "$1" "$2" "$3" | jq -r --arg f "$4" 'if . == null then "absent" else (.[$f] | tostring) end'
}

dump_tips() { # TAG RPC DATADIR
    printf -- '---- [%s] getchaintips ----\n' "$1"
    rpc "$2" "$3" getchaintips | jq -c '.[]' 2>/dev/null || true
}

# expect_tip LABEL RPC DATADIR HASH STATUS BRANCHLEN HEIGHT
expect_tip() {
    local label="$1" rpc_port="$2" datadir="$3" hash="$4" want_status="$5" want_len="$6" want_h="$7"
    local entry st len h
    entry="$(tip_entry "${rpc_port}" "${datadir}" "${hash}")"
    if [[ "${entry}" == "null" ]]; then
        fail "${label}: tip ${hash:0:16}… ABSENT from getchaintips (want status=${want_status} branchlen=${want_len} height=${want_h})"
        return
    fi
    st="$(jq -r '.status' <<<"${entry}")"
    len="$(jq -r '.branchlen' <<<"${entry}")"
    h="$(jq -r '.height' <<<"${entry}")"
    if [[ "${st}" == "${want_status}" && "${len}" == "${want_len}" && "${h}" == "${want_h}" ]]; then
        pass "${label}: tip ${hash:0:16}… status=${st} branchlen=${len} height=${h}"
    else
        fail "${label}: tip ${hash:0:16}… status=${st} branchlen=${len} height=${h} (want ${want_status}/${want_len}/${want_h})"
    fi
}

# ---------------------------------------------------------------------------
# Topology
# ---------------------------------------------------------------------------
base_port="$(alloc_port_base 8)" || setup_error "could not allocate ports"
a_rpc=$((base_port + 0)); b_rpc=$((base_port + 1))
a_p2p=$((base_port + 2)); b_p2p=$((base_port + 3))
a_strat=$((base_port + 4)); b_strat=$((base_port + 5))
a_wal=$((base_port + 6)); b_wal=$((base_port + 7))

a_dir="$(mktemp -d "${TMPDIR:-/tmp}/dinero_741_A_XXXXXX")"
b_dir="$(mktemp -d "${TMPDIR:-/tmp}/dinero_741_B_XXXXXX")"
DIRS+=("${a_dir}" "${b_dir}")

info "A rpc=${a_rpc} p2p=${a_p2p} | B rpc=${b_rpc} p2p=${b_p2p} | BASE=${BASE} SIDE=${SIDE} MAJORITY=${MAJORITY}"

a_pid="$(start_node "${a_dir}" "${a_rpc}" "${a_p2p}" "${a_strat}" "${a_wal}")"
b_pid="$(start_node "${b_dir}" "${b_rpc}" "${b_p2p}" "${b_strat}" "${b_wal}")"
PIDS+=("${a_pid}" "${b_pid}")
wait_rpc "${a_rpc}" "${a_dir}" || { tail -40 "${a_dir}.log" >&2; setup_error "node A RPC did not come up"; }
wait_rpc "${b_rpc}" "${b_dir}" || { tail -40 "${b_dir}.log" >&2; setup_error "node B RPC did not come up"; }

addr_a="$(rpc_call "${a_rpc}" "${a_dir}" wallet.createhd '["miner-a"]' | jq -r '.result.first_address // empty')"
addr_b="$(rpc_call "${b_rpc}" "${b_dir}" wallet.createhd '["miner-b"]' | jq -r '.result.first_address // empty')"
[[ -n "${addr_a}" && -n "${addr_b}" ]] || setup_error "could not create wallets (A='${addr_a}' B='${addr_b}')"

connect_ab() { rpc_call "${a_rpc}" "${a_dir}" addnode "[\"127.0.0.1:${b_p2p}\",\"onetry\"]" >/dev/null; }
wait_connected() {
    wait_for "[[ \$(conns_of ${a_rpc} ${a_dir}) -ge 1 && \$(conns_of ${b_rpc} ${b_dir}) -ge 1 ]]" 30
}

# 1. common history
mine "${b_rpc}" "${b_dir}" "${BASE}" "${addr_b}"
wait_for "[[ \$(height_of ${b_rpc} ${b_dir}) -eq ${BASE} ]]" 30 || setup_error "B did not mine to ${BASE}"
connect_ab
wait_connected || setup_error "A and B never connected"
wait_for "[[ \$(tip_of ${a_rpc} ${a_dir}) == \$(tip_of ${b_rpc} ${b_dir}) ]]" 60 \
    || setup_error "A did not sync common history to ${BASE} (A=$(height_of "${a_rpc}" "${a_dir}"))"
common_tip="$(tip_of "${a_rpc}" "${a_dir}")"
info "common history @${BASE} tip ${common_tip:0:16}…"

# Sanity: a single tip before any divergence.
ntips0="$(rpc "${a_rpc}" "${a_dir}" getchaintips | jq -r length)"
info "pre-divergence getchaintips(A): ${ntips0} tip(s)"

# 2. partition
rpc_call "${a_rpc}" "${a_dir}" setnetworkactive '[false]' >/dev/null
rpc_call "${b_rpc}" "${b_dir}" setnetworkactive '[false]' >/dev/null
wait_for "[[ \$(conns_of ${a_rpc} ${a_dir}) -eq 0 && \$(conns_of ${b_rpc} ${b_dir}) -eq 0 ]]" 30 \
    || setup_error "partition did not drop the A<->B connection"

# 3. diverge
a_side_h=$((BASE + SIDE)); b_branch_h=$((BASE + MAJORITY))
mine "${a_rpc}" "${a_dir}" "${SIDE}" "${addr_a}"
mine "${b_rpc}" "${b_dir}" "${MAJORITY}" "${addr_b}"
wait_for "[[ \$(height_of ${a_rpc} ${a_dir}) -eq ${a_side_h} ]]" 30 || setup_error "A did not mine its ${SIDE}-block side branch"
wait_for "[[ \$(height_of ${b_rpc} ${b_dir}) -eq ${b_branch_h} ]]" 30 || setup_error "B did not mine its ${MAJORITY}-block branch"
a_old_tip="$(tip_of "${a_rpc}" "${a_dir}")"
a_fork1="$(hash_at "${a_rpc}" "${a_dir}" $((BASE + 1)))"
b_tip="$(tip_of "${b_rpc}" "${b_dir}")"
[[ "${a_old_tip}" != "${b_tip}" ]] || setup_error "branches did not diverge"
info "diverged: A(side) ${a_old_tip:0:16}… @${a_side_h} (first side block ${a_fork1:0:16}…)  B(majority) ${b_tip:0:16}… @${b_branch_h}"

# 4. heal -> A must reorg onto B's branch
rpc_call "${a_rpc}" "${a_dir}" setnetworkactive '[true]' >/dev/null
rpc_call "${b_rpc}" "${b_dir}" setnetworkactive '[true]' >/dev/null
connect_ab
wait_connected || setup_error "A and B did not reconnect after heal"
if ! wait_for "[[ \$(tip_of ${a_rpc} ${a_dir}) == ${b_tip} ]]" "${CONVERGE_WAIT}"; then
    dump_tips "A (no reorg)" "${a_rpc}" "${a_dir}"
    setup_error "A did not reorg onto B's branch within ${CONVERGE_WAIT}s (A=$(height_of "${a_rpc}" "${a_dir}"); precondition for #741, see #738)"
fi
info "A reorged: tip now ${b_tip:0:16}… @$(height_of "${a_rpc}" "${a_dir}")"

# ---------------------------------------------------------------------------
# Assertions
# ---------------------------------------------------------------------------
dump_tips "A after reorg" "${a_rpc}" "${a_dir}"
ntips="$(rpc "${a_rpc}" "${a_dir}" getchaintips | jq -r length)"
if (( ntips >= 2 )); then
    pass "getchaintips(A) lists ${ntips} tips (active + abandoned branch)"
else
    fail "getchaintips(A) lists only ${ntips} tip(s); the abandoned ${SIDE}-block branch was dropped (#741)"
fi

expect_tip "active tip" "${a_rpc}" "${a_dir}" "${b_tip}" "active" 0 "${b_branch_h}"
expect_tip "abandoned branch" "${a_rpc}" "${a_dir}" "${a_old_tip}" "valid-fork" "${SIDE}" "${a_side_h}"

cw="$(tip_field "${a_rpc}" "${a_dir}" "${a_old_tip}" chainwork)"
if [[ "${cw}" =~ ^[0-9a-f]{64}$ ]]; then
    pass "abandoned branch carries a 64-hex chainwork (${cw:48:16})"
else
    fail "abandoned branch chainwork missing/malformed: '${cw}'"
fi

# invalid -> valid-fork round trip via operator RPCs (branch stays a tip either way)
inv="$(rpc_call "${a_rpc}" "${a_dir}" invalidateblock "[\"${a_fork1}\"]")"
if jq -e '.error == null' <<<"${inv}" >/dev/null 2>&1; then
    wait_for "[[ \$(tip_field ${a_rpc} ${a_dir} ${a_old_tip} status) == invalid ]]" 10 || true
    dump_tips "A after invalidateblock(side1)" "${a_rpc}" "${a_dir}"
    expect_tip "after invalidateblock" "${a_rpc}" "${a_dir}" "${a_old_tip}" "invalid" "${SIDE}" "${a_side_h}"
    expect_tip "active tip unaffected by invalidateblock" "${a_rpc}" "${a_dir}" "${b_tip}" "active" 0 "${b_branch_h}"
    rec="$(rpc_call "${a_rpc}" "${a_dir}" reconsiderblock "[\"${a_fork1}\"]")"
    if jq -e '.error == null' <<<"${rec}" >/dev/null 2>&1; then
        wait_for "[[ \$(tip_field ${a_rpc} ${a_dir} ${a_old_tip} status) == valid-fork ]]" 10 || true
        dump_tips "A after reconsiderblock(side1)" "${a_rpc}" "${a_dir}"
        expect_tip "after reconsiderblock" "${a_rpc}" "${a_dir}" "${a_old_tip}" "valid-fork" "${SIDE}" "${a_side_h}"
        expect_tip "active tip unaffected by reconsiderblock" "${a_rpc}" "${a_dir}" "${b_tip}" "active" 0 "${b_branch_h}"
    else
        fail "reconsiderblock(${a_fork1:0:16}…) errored: $(jq -c '.error' <<<"${rec}")"
    fi
else
    fail "invalidateblock(${a_fork1:0:16}…) errored: $(jq -c '.error' <<<"${inv}")"
fi

# Reported only: B's view of the fork.
dump_tips "B (reported only)" "${b_rpc}" "${b_dir}"
info "getchaintips(B): A's side tip ${a_old_tip:0:16}… status=$(tip_field "${b_rpc}" "${b_dir}" "${a_old_tip}" status)"

echo
if (( FAILURES > 0 )); then
    printf '=== FAIL: #741 — %s assertion(s) failed ===\n' "${FAILURES}"
    printf -- '---- A log: reorg lines (last 20) ----\n'
    grep -aE 'REORG|ActivateBestChain|valid-fork|getchaintips' "${a_dir}.log" 2>/dev/null | tail -20 || true
    exit 1
fi
printf '=== PASS: getchaintips reports the abandoned %s-block branch as valid-fork (and invalid/valid-fork round trip) ===\n' "${SIDE}"
exit 0
