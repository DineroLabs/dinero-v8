#!/usr/bin/env bash
# Two inbound peers compete for one total slot. The second must be refused
# even though the per-direction inbound limit has room; the unit test covers
# mixed inbound/outbound and virtual peers.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
BASE_PORT="${BASE_PORT:-42600}"
WORK="$(mktemp -d)"
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do kill "${pid}" 2>/dev/null || true; done
    for pid in "${PIDS[@]:-}"; do wait "${pid}" 2>/dev/null || true; done
    [[ "${KEEP_WORK:-0}" == 1 ]] || rm -rf "${WORK}"
}
trap cleanup EXIT
fail() {
    echo "FAIL: $*" >&2
    for name in a b c; do
        [[ -f "${WORK}/${name}.log" ]] && tail -12 "${WORK}/${name}.log" >&2
    done
    exit 1
}
cookie() {
    for file in "$1/.cookie" "$1/regtest/.cookie"; do
        [[ -f "${file}" ]] && { tr -d '\n' < "${file}"; return 0; }
    done
    return 1
}
rpc() {
    local auth
    auth="$(cookie "$2")" || return 1
    curl -sS --fail --max-time 10 --user "${auth}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$3\",\"params\":[]}" \
        "http://127.0.0.1:$1/"
}
wait_rpc() {
    for _ in $(seq 1 60); do rpc "$1" "$2" getblockcount >/dev/null 2>&1 && return 0; sleep 1; done
    return 1
}
peers() {
    rpc "$1" "$2" getpeerinfo | python3 -c '
import json,sys
result=json.load(sys.stdin)["result"]
print(sum(1 for peer in result if peer.get("connected", True)))'
}
start() {
    local name="$1" rpc_port="$2" p2p_port="$3"; shift 3
    mkdir -p "${WORK}/${name}"
    "${DINEROD}" --regtest --datadir="${WORK}/${name}" \
        --rpcport="${rpc_port}" --port="${p2p_port}" --quic=0 \
        --p2p.stun.enabled=0 --portmap=0 "$@" >"${WORK}/${name}.log" 2>&1 &
    PIDS+=("$!")
}

[[ -x "${DINEROD}" ]] || fail "dinerod missing: ${DINEROD}"
start a "$((BASE_PORT + 1))" "$((BASE_PORT + 101))" --connect=0 --maxconnections=1
wait_rpc "$((BASE_PORT + 1))" "${WORK}/a" || fail "A did not start"
start b "$((BASE_PORT + 2))" "$((BASE_PORT + 102))" \
    --connect="127.0.0.1:$((BASE_PORT + 101))" --listen=0
wait_rpc "$((BASE_PORT + 2))" "${WORK}/b" || fail "B did not start"
for _ in $(seq 1 30); do
    [[ "$(peers "$((BASE_PORT + 1))" "${WORK}/a")" == 1 ]] && break
    sleep 1
done
[[ "$(peers "$((BASE_PORT + 1))" "${WORK}/a")" == 1 ]] || fail "B did not connect to A"

start c "$((BASE_PORT + 3))" "$((BASE_PORT + 103))" \
    --connect="127.0.0.1:$((BASE_PORT + 101))" --listen=0
wait_rpc "$((BASE_PORT + 3))" "${WORK}/c" || fail "C did not start"
sleep 3
[[ "$(peers "$((BASE_PORT + 1))" "${WORK}/a")" == 1 ]] ||
    fail "A exceeded maxconnections=1 after C dialed"
echo "PASS: maxconnections=1 includes inbound peers; second inbound refused"
