#!/usr/bin/env bash
# Two regtest dinerods where QUIC is the ONLY possible transport: B has no TCP
# listener and no TCP connect target, just --quicconnect to A. A mines; B must
# sync every block over a direct QUIC peer, and both sides must report it as
# transport "quic". Exits non-zero on any failed check.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
BASE_PORT="${BASE_PORT:-39400}"
A_RPC=$((BASE_PORT + 1)); B_RPC=$((BASE_PORT + 2))
A_P2P=$((BASE_PORT + 101)); B_P2P=$((BASE_PORT + 102))
A_QUIC="${A_QUIC:-1}"   # set to 0 to neuter: B must then never sync
BLOCKS=20

WORK="$(mktemp -d)"
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do kill "${pid}" 2>/dev/null || true; done
    for pid in "${PIDS[@]:-}"; do wait "${pid}" 2>/dev/null || true; done
    [[ "${KEEP_WORK:-0}" == 1 ]] || rm -rf "${WORK}"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; echo "--- A log tail"; tail -20 "${WORK}/a.log" || true; echo "--- B log tail"; tail -20 "${WORK}/b.log" || true; exit 1; }
pass() { echo "PASS: $*"; }

cookie() {
    local d="$1"
    for f in "${d}/.cookie" "${d}/regtest/.cookie"; do
        [[ -f "${f}" ]] && { tr -d '\n' < "${f}"; return 0; }
    done
    return 1
}
rpc() {  # rpc <port> <datadir> <method> <params-json>
    local c; c="$(cookie "$2")" || return 1
    curl -sS --fail --max-time 30 --user "${c}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$3\",\"params\":$4}" "http://127.0.0.1:$1/"
}
result() { python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["result"]))'; }
wait_rpc() {
    for _ in $(seq 1 90); do rpc "$1" "$2" getblockcount '[]' >/dev/null 2>&1 && return 0; sleep 1; done
    return 1
}
start() {  # start <name> <rpc> <p2p> <extra args...>
    local name="$1" rpcp="$2" p2p="$3"; shift 3
    mkdir -p "${WORK}/${name}"
    "${DINEROD}" --regtest --datadir="${WORK}/${name}" --rpcport="${rpcp}" --port="${p2p}" \
        --p2p.stun.enabled=0 --portmap=0 "$@" >"${WORK}/${name}.log" 2>&1 &
    PIDS+=("$!")
}

[[ -x "${DINEROD}" ]] || fail "dinerod not found at ${DINEROD}"

start a "${A_RPC}" "${A_P2P}" --listen=1 --quic="${A_QUIC}"
start b "${B_RPC}" "${B_P2P}" --listen=0 --quicconnect="127.0.0.1:${A_P2P}"
wait_rpc "${A_RPC}" "${WORK}/a" || fail "A RPC never came up"
wait_rpc "${B_RPC}" "${WORK}/b" || fail "B RPC never came up"

if [[ "${A_QUIC}" == 1 ]]; then
    listening="$(rpc "${A_RPC}" "${WORK}/a" getnetworkinfo '[]' | result |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["quic_transport"].get("listening"))')"
    [[ "${listening}" == "True" ]] || fail "A quic_transport.listening=${listening}"
    pass "A listens for direct QUIC peers on UDP ${A_P2P}"
fi

addr="$(rpc "${A_RPC}" "${WORK}/a" wallet.createhd '["miner"]' | result |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["first_address"])')"
[[ -n "${addr}" ]] || fail "could not create a mining address on A"
rpc "${A_RPC}" "${WORK}/a" generatetoaddress "[${BLOCKS}, \"${addr}\"]" >/dev/null ||
    fail "generatetoaddress failed on A"
a_height="$(rpc "${A_RPC}" "${WORK}/a" getblockcount '[]' | result)"
[[ "${a_height}" -ge "${BLOCKS}" ]] || fail "A height ${a_height} < ${BLOCKS}"
pass "A mined to height ${a_height}"

b_height=0
for _ in $(seq 1 60); do
    b_height="$(rpc "${B_RPC}" "${WORK}/b" getblockcount '[]' | result)"
    [[ "${b_height}" -ge "${a_height}" ]] && break
    sleep 1
done
[[ "${b_height}" -ge "${a_height}" ]] || fail "B stuck at height ${b_height} (A at ${a_height})"
pass "B synced to height ${b_height}"

b_peers="$(rpc "${B_RPC}" "${WORK}/b" getpeerinfo '[]' | result)"
summary="$(python3 -c '
import json,sys
peers=[p for p in json.loads(sys.argv[1]) if p.get("connected", True)]
print(len(peers), ",".join(sorted(p.get("transport","?") for p in peers)))' "${b_peers}")"
[[ "${summary}" == "1 quic" ]] || fail "B peers: expected exactly one quic peer, got '${summary}'"
pass "B's only peer is transport=quic"

a_peers="$(rpc "${A_RPC}" "${WORK}/a" getpeerinfo '[]' | result)"
python3 -c '
import json,sys
peers=json.loads(sys.argv[1])
assert any(p.get("transport")=="quic" and p.get("inbound") for p in peers), peers' "${a_peers}" ||
    fail "A has no inbound transport=quic peer"
pass "A sees B as an inbound transport=quic peer"
echo "ALL CHECKS PASSED"
