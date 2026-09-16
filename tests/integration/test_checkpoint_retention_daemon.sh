#!/usr/bin/env bash
# Real local daemon gates after pruning a stopped, independently copied datadir:
# restart, historical spend proof serving, actual mid-sync CSN restart, and a
# competing branch whose fork checkpoint was deleted. No seed data is used.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
RETENTION_COPY="${RETENTION_COPY:-${ROOT_DIR}/build/utreexo_retention_copy}"
MUTATOR="${MUTATOR:-${ROOT_DIR}/build/tests/integration/utreexo_checkpoint_mutator}"
for executable in "${DINEROD}" "${RETENTION_COPY}" "${MUTATOR}"; do
    [[ -x "${executable}" ]] || { echo "Missing executable: ${executable}" >&2; exit 1; }
done
for utility in curl jq python3; do
    command -v "${utility}" >/dev/null || { echo "Missing utility: ${utility}" >&2; exit 1; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/dinero_retention_daemon.XXXXXX")"
SOURCE="${WORK}/source"
COPY="${WORK}/pruned"
CSN="${WORK}/csn"
PID_BRIDGE=""; PID_CSN=""
BRIDGE_DIR="${SOURCE}"
BRIDGE_LOG="${WORK}/source.log"
CSN_LOG="${WORK}/csn-initial.log"
read -r RPC_BRIDGE P2P_BRIDGE WALLET_BRIDGE RPC_CSN P2P_CSN WALLET_CSN < <(python3 - <<'PY'
import socket
sockets = [socket.socket() for _ in range(6)]
try:
    for sock in sockets:
        sock.bind(('127.0.0.1', 0))
        sock.listen(1)
    print(*(sock.getsockname()[1] for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
)

info() { printf '[INFO] %s\n' "$*"; }
fail() { printf '[FAIL] %s\nEvidence: %s\n' "$*" "${WORK}" >&2; exit 1; }
cleanup() {
    local rc=$? cleanup_rc=0
    trap - EXIT
    set +e
    # A mid-sync interception may have left CSN stopped. Resume only our child
    # so shutdown can finish before any temporary files are removed.
    [[ -n "${PID_CSN}" ]] && kill -CONT "${PID_CSN}" 2>/dev/null
    dinero_stop_process "${PID_CSN}" "retention CSN" || cleanup_rc=1
    dinero_stop_process "${PID_BRIDGE}" "retention bridge" || cleanup_rc=1
    if (( rc != 0 || cleanup_rc != 0 )); then
        printf '[INFO] Preserved evidence: %s\n' "${WORK}" >&2
        for log in "${WORK}"/*.log; do
            [[ -f "${log}" ]] || continue
            printf '[INFO] Log tail: %s\n' "${log}" >&2
            tail -50 "${log}" >&2
        done
        (( rc != 0 )) || rc=1
    elif [[ "${KEEP_RETENTION_EVIDENCE:-0}" == 1 ]]; then
        printf '[INFO] Evidence: %s\n' "${WORK}"
    else
        rm -rf "${WORK}"
    fi
    exit "${rc}"
}
trap cleanup EXIT

rpc_raw() {
    local dir=$1 port=$2 method=$3 params=${4:-'[]'} cookie_file cookie
    cookie_file="${dir}/.cookie"
    [[ -f "${cookie_file}" ]] || cookie_file="${dir}/regtest/.cookie"
    [[ -f "${cookie_file}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_file}")"
    [[ -n "${cookie}" ]] || return 1
    curl -sS --connect-timeout 1 --max-time 90 --user "${cookie}" \
        -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${port}/"
}
rpc() {
    local result
    result="$(rpc_raw "$@")" || return 1
    jq -e '.error == null and has("result")' <<<"${result}" >/dev/null || {
        printf '%s\n' "${result}" >&2; return 1;
    }
    jq -c '.result' <<<"${result}"
}
bridge() { rpc "${BRIDGE_DIR}" "${RPC_BRIDGE}" "$@"; }
csn() { rpc "${CSN}" "${RPC_CSN}" "$@"; }
ready() {
    local dir=$1 port=$2 pid=$3 deadline=$((SECONDS + 90))
    while (( SECONDS < deadline )); do
        dinero_process_is_running "${pid}" || return 1
        if rpc "${dir}" "${port}" getblockcount '[]' >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}
start_bridge() {
    local offline=$1
    mkdir -p "${BRIDGE_DIR}"
    local args=(--regtest --datadir="${BRIDGE_DIR}" --rpcport="${RPC_BRIDGE}"
        --port="${P2P_BRIDGE}" --wallet-socket-port="${WALLET_BRIDGE}"
        --utreexo=1 --utreexo-bridge=1 --utreexo.checkpoint_interval=1)
    if [[ "${offline}" == 1 ]]; then
        args+=(--listen=0 --p2p.offline=1)
    else
        args+=(--listen=1 --connect="127.0.0.1:${P2P_CSN}")
    fi
    "${DINEROD}" "${args[@]}" >"${BRIDGE_LOG}" 2>&1 &
    PID_BRIDGE=$!
    ready "${BRIDGE_DIR}" "${RPC_BRIDGE}" "${PID_BRIDGE}" || fail "Bridge RPC did not start"
}
start_csn() {
    mkdir -p "${CSN}"
    "${DINEROD}" --regtest --datadir="${CSN}" --rpcport="${RPC_CSN}" \
        --port="${P2P_CSN}" --wallet-socket-port="${WALLET_CSN}" \
        --utreexo=1 --utreexo-stateless=1 --utreexo.checkpoint_interval=1 \
        --listen=1 --connect="127.0.0.1:${P2P_BRIDGE}" >"${CSN_LOG}" 2>&1 &
    PID_CSN=$!
    ready "${CSN}" "${RPC_CSN}" "${PID_CSN}" || fail "CSN RPC did not start"
}
stop_bridge() {
    dinero_stop_process "${PID_BRIDGE}" "retention bridge" || fail "Bridge did not exit"
    PID_BRIDGE=""
}
stop_csn() {
    dinero_stop_process "${PID_CSN}" "retention CSN" || fail "CSN did not exit"
    PID_CSN=""
}
mine() { bridge generatetoaddress "[$1,\"$2\"]" >/dev/null || fail "Mining $1 blocks failed"; }
state() {
    local dir=$1 port=$2
    local height hash commitment roots
    height="$(rpc "${dir}" "${port}" getblockcount)" || return 1
    hash="$(rpc "${dir}" "${port}" getbestblockhash)" || return 1
    commitment="$(rpc "${dir}" "${port}" blockchain.getutreexocommitment)" || return 1
    roots="$(rpc "${dir}" "${port}" blockchain.getutreexoroots)" || return 1
    jq -cS -n --argjson height "${height}" --argjson hash "${hash}" \
        --argjson commitment "${commitment}" --argjson roots "${roots}" \
        '{height:$height,hash:$hash,commitment:($commitment | {commitment,num_leaves,num_roots,compact_state_bytes}),roots:$roots}'
}
# Local bridge comparisons also cover the complete UTXO response and internal
# forest structure (positions and deletion bookkeeping). CSNs have a compact
# forest, so cross-role comparisons intentionally use state() above.
full_state() {
    local dir=$1 port=$2 label=$3 canonical txouts dump_result forest_hash
    local dump_path="${WORK}/${label}.forest"
    canonical="$(state "${dir}" "${port}")" || return 1
    txouts="$(rpc "${dir}" "${port}" blockchain.gettxoutsetinfo)" || return 1
    dump_result="$(rpc "${dir}" "${port}" utreexo.dumpforestinternal \
        "$(jq -cn --arg path "${dump_path}" '[$path]')")" || return 1
    jq -e '.bytes > 0' <<<"${dump_result}" >/dev/null || return 1
    [[ -s "${dump_path}" ]] || return 1
    forest_hash="$(python3 - "${dump_path}" <<'PY'
import hashlib, pathlib, sys
print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())
PY
    )" || return 1
    jq -cS -n --argjson canonical "${canonical}" --argjson txouts "${txouts}" \
        --arg forest_hash "${forest_hash}" \
        '{canonical:$canonical,txouts:$txouts,forest_sha256:$forest_hash}'
}
assert_state() {
    [[ "$1" == "$2" ]] || fail "$3: canonical state differs; expected=$1 actual=$2"
}
wait_csn_tip() {
    local height=$1 hash=$2 deadline=$((SECONDS + 240)) got_height got_hash
    while (( SECONDS < deadline )); do
        dinero_process_is_running "${PID_CSN}" || fail "CSN exited during sync"
        got_height="$(csn getblockcount 2>/dev/null || true)"
        got_hash="$(csn getbestblockhash 2>/dev/null || true)"
        if [[ "${got_height}" == "${height}" && "${got_hash}" == "${hash}" ]]; then return 0; fi
        sleep 0.2
    done
    fail "CSN did not converge to ${height}/${hash}; last=${got_height}/${got_hash}"
}
checkpoint_present() {
    local expected=$1 output
    output="$("${MUTATOR}" "${COPY}/blockchain/chaindb" --inspect-height "${FORK_HEIGHT}" 2>"${WORK}/probe.stderr")" \
        || fail "Checkpoint probe failed"
    [[ "${output}" == *"checkpoint_present=${expected}"* ]] || fail "Checkpoint precondition failed: ${output}"
    [[ "${output}" == *"checksum_present=${expected}"* ]] || fail "Checksum precondition failed: ${output}"
}
log_contains() {
    local pattern=$1 log
    shift
    for log in "$@"; do
        [[ -f "${log}" ]] || continue
        grep -Eq "${pattern}" "${log}" && return 0
    done
    return 1
}

info 'Building an isolated dense checkpoint source with a real historical spend'
start_bridge 1
wallet="$(bridge wallet.createhd '["retention-gate"]')" || fail 'Wallet creation failed'
MINER="$(jq -r '.first_address // empty' <<<"${wallet}")"
[[ -n "${MINER}" ]] || fail 'Wallet returned no mining address'
mine 110 "${MINER}"
FORK_HEIGHT="$(bridge getblockcount)"
[[ "${FORK_HEIGHT}" == 110 ]] || fail "Unexpected source starting height: ${FORK_HEIGHT}"
FORK_STATE="$(state "${SOURCE}" "${RPC_BRIDGE}")" || fail 'Cannot capture fork state'
FORK_FULL_STATE="$(full_state "${SOURCE}" "${RPC_BRIDGE}" source-fork)" || fail 'Cannot capture full fork state'
recipient="$(bridge wallet.getnewaddress '[]' | jq -r '.address // . // empty')"
[[ -n "${recipient}" ]] || fail 'No recipient address'
send="$(bridge wallet.sendtoaddress "[\"${recipient}\",1.0]")" || fail 'Real spend failed'
SPEND_TXID="$(jq -r '(.txid // .) | strings' <<<"${send}")"
[[ -n "${SPEND_TXID}" ]] || fail 'Spend returned no txid'
mine 1 "${MINER}"
SPEND_BLOCK="$(bridge getbestblockhash)"
SPEND_HASH_PREFIX="$(jq -r '.[0:16]' <<<"${SPEND_BLOCK}")"
decoded="$(bridge getblock "[${SPEND_BLOCK},2]")" || fail 'Cannot inspect mined spend block'
jq -e --arg txid "${SPEND_TXID}" '[.tx[] | if type == "object" then .txid else . end] | index($txid) != null' \
    <<<"${decoded}" >/dev/null || fail 'Spend was not included in historical block'
mine 39 "${MINER}"
ORIGINAL_STATE="$(state "${SOURCE}" "${RPC_BRIDGE}")" || fail 'Cannot capture original tip'
ORIGINAL_FULL_STATE="$(full_state "${SOURCE}" "${RPC_BRIDGE}" source-tip)" || fail 'Cannot capture full original tip'
[[ "${FORK_FULL_STATE}" != "${ORIGINAL_FULL_STATE}" ]] || fail 'Full-state capture did not detect chain changes'
ORIGINAL_HEIGHT="$(bridge getblockcount)"
ORIGINAL_HASH="$(bridge getbestblockhash)"
[[ "${ORIGINAL_HEIGHT}" == 150 ]] || fail 'Source did not reach height 150'
stop_bridge

info 'Copying the stopped source, then verifying and deleting checkpoint 110'
python3 - "${SOURCE}" "${COPY}" <<'PY'
import shutil, sys
shutil.copytree(sys.argv[1], sys.argv[2])
PY
checkpoint_present 1
"${RETENTION_COPY}" --source-datadir "${SOURCE}" --copy-datadir "${COPY}" \
    --network regtest --apply --recent-blocks 6 --historical-interval 50 \
    --replay-per-step 8 --delete-per-batch 7 --pause-ms 0 \
    >"${WORK}/retention.log" 2>&1 || fail 'Offline copy retention failed'
checkpoint_present 0
BRIDGE_DIR="${COPY}"; BRIDGE_LOG="${WORK}/pruned-restart.log"
start_bridge 0
assert_state "${ORIGINAL_STATE}" "$(state "${COPY}" "${RPC_BRIDGE}")" 'Pruned-copy restart'
assert_state "${ORIGINAL_FULL_STATE}" "$(full_state "${COPY}" "${RPC_BRIDGE}" pruned-tip)" 'Pruned-copy complete state restart'

info 'Starting a fresh CSN against the pruned bridge and intercepting real mid-sync'
start_csn
deadline=$((SECONDS + 180)); intercepted=0
while (( SECONDS < deadline )); do
    height="$(csn getblockcount 2>/dev/null || true)"
    if [[ "${height}" =~ ^[0-9]+$ ]] && (( height > 0 && height < ORIGINAL_HEIGHT )); then
        kill -STOP "${PID_CSN}"
        intercepted=1
        break
    fi
    [[ "${height}" != "${ORIGINAL_HEIGHT}" ]] || fail 'Mid-sync interception missed; completed-tip restart is not this gate'
    sleep 0.05
done
[[ "${intercepted}" == 1 ]] || fail 'CSN never reached a mid-sync height'
stop_bridge
kill -TERM "${PID_CSN}"
kill -CONT "${PID_CSN}"
stop_csn
CSN_LOG="${WORK}/csn-mid-restart.log"
start_csn
MID_STATE="$(state "${CSN}" "${RPC_CSN}")" || fail 'Cannot capture restored mid-sync state'
MID_HEIGHT="$(jq -r .height <<<"${MID_STATE}")"
(( MID_HEIGHT > 0 && MID_HEIGHT < ORIGINAL_HEIGHT )) || fail "Persisted height ${MID_HEIGHT} was not mid-sync"
stop_csn
CSN_LOG="${WORK}/csn-mid-repeat.log"
start_csn
assert_state "${MID_STATE}" "$(state "${CSN}" "${RPC_CSN}")" 'Repeated offline mid-sync restart'
BRIDGE_LOG="${WORK}/pruned-relay.log"
start_bridge 0
wait_csn_tip "${ORIGINAL_HEIGHT}" "${ORIGINAL_HASH}"
assert_state "${ORIGINAL_STATE}" "$(state "${CSN}" "${RPC_CSN}")" 'Historical proof relay'
# Require the actual spending block, rather than accepting proof markers from
# unrelated coinbase-only blocks. It may have crossed before interception.
log_contains "Sent utreexo block ${SPEND_HASH_PREFIX}" "${WORK}"/pruned-*.log "${COPY}/p2p.log" \
    || fail 'Pruned bridge did not report serving the historical spend block'
log_contains "Block ${SPEND_HASH_PREFIX}.*validated with (transition|batch) proof \\(height=111\\)" \
    "${WORK}"/csn-*.log "${CSN}/p2p.log" \
    || fail 'Fresh CSN did not report verifying the historical spend proof at height 111'
info "Mid-sync restart passed at ${MID_HEIGHT}; CSN verified historical spend ${SPEND_TXID}"

info 'Disconnecting 40 blocks across deleted checkpoint 110 and mining a longer branch'
bridge invalidateblock "[${SPEND_BLOCK}]" >/dev/null || fail 'Branch invalidation failed'
assert_state "${FORK_STATE}" "$(state "${COPY}" "${RPC_BRIDGE}")" 'Deep disconnect to pruned fork'
assert_state "${FORK_FULL_STATE}" "$(full_state "${COPY}" "${RPC_BRIDGE}" pruned-fork)" 'Deep disconnect complete state restoration'
bridge mempool.clear '[]' >/dev/null || fail 'Mempool clear failed'
mine 41 "${MINER}"
WINNING_HEIGHT="$(bridge getblockcount)"; WINNING_HASH="$(bridge getbestblockhash)"
[[ "${WINNING_HEIGHT}" == 151 && "${WINNING_HASH}" != "${ORIGINAL_HASH}" ]] || fail 'Competing branch did not win'
wait_csn_tip "${WINNING_HEIGHT}" "${WINNING_HASH}"
WINNING_STATE="$(state "${COPY}" "${RPC_BRIDGE}")" || fail 'Cannot capture winning state'
WINNING_FULL_STATE="$(full_state "${COPY}" "${RPC_BRIDGE}" winning-tip)" || fail 'Cannot capture full winning state'
assert_state "${WINNING_STATE}" "$(state "${CSN}" "${RPC_CSN}")" 'CSN competing-branch convergence'
stop_csn
stop_bridge
BRIDGE_LOG="${WORK}/pruned-postreorg-restart.log"
start_bridge 1
assert_state "${WINNING_STATE}" "$(state "${COPY}" "${RPC_BRIDGE}")" 'Post-reorg pruned-copy restart'
assert_state "${WINNING_FULL_STATE}" "$(full_state "${COPY}" "${RPC_BRIDGE}" winning-restarted)" 'Post-reorg complete state restart'
stop_bridge
printf '[PASS] Pruned-copy restart, old-height spend proof relay, mid-sync restart at %s, 40-block disconnect and longer competing branch, post-reorg restart\n' "${MID_HEIGHT}"
