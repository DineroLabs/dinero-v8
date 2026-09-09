#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Smoke test for the 3 dashboard RPC additions:
#   1. getnetworkinfo.node_id_hex (40-char hex)
#   2. getpeerinfo[].ping_ms + .quality_score (always present)
#   3. dynamic_p2p.observe (valid shape)
#
# Explicit datadirs, dynamically allocated ports and owned child PIDs avoid
# platform-dependent HOME discovery and interference with other daemon tests.
set -euo pipefail
DINEROD="${DINEROD:?DINEROD must point to dinerod binary}"
DINERO_CLI="${DINERO_CLI:?DINERO_CLI must point to dinero-cli binary}"
TMP="$(mktemp -d)"
HA="$TMP/a"
HB="$TMP/b"
mkdir -p "$HA" "$HB"
read -r RA PA WA RB PB WB < <(python3 - <<'PORTS'
import socket
sockets=[socket.socket() for _ in range(6)]
for s in sockets: s.bind(('127.0.0.1',0))
print(*(s.getsockname()[1] for s in sockets))
for s in sockets: s.close()
PORTS
)
PIDA=""; PIDB=""
cleanup() {
    code=$?
    for pid in "$PIDA" "$PIDB"; do
        if [[ -n "$pid" ]]; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [[ "$code" == 0 ]]; then
        rm -rf "$TMP"
    else
        echo "Dashboard evidence: $TMP" >&2
        tail -n 60 "$HA/daemon.log" "$HB/daemon.log" 2>/dev/null || true
    fi
}
trap cleanup EXIT
wait_rpc() {
    local dir="$1" port="$2" deadline=$((SECONDS + 30))
    until "$DINERO_CLI" -datadir="$dir" -rpcport="$port" getnetworkinfo >/dev/null 2>&1; do
        [ $SECONDS -lt $deadline ] || { echo "FAIL: daemon at port $port not ready"; exit 1; }
        sleep 1
    done
}
"$DINEROD" --regtest --datadir="$HA" --rpcport="$RA" --port="$PA" \
    --wallet-socket-port="$WA" --listen=1 >"$HA/daemon.log" 2>&1 &
PIDA=$!
wait_rpc "$HA" "$RA"
"$DINEROD" --regtest --datadir="$HB" --rpcport="$RB" --port="$PB" \
    --wallet-socket-port="$WB" --listen=1 --addnode="127.0.0.1:$PA" >"$HB/daemon.log" 2>&1 &
PIDB=$!
wait_rpc "$HB" "$RB"
cli_a() { "$DINERO_CLI" -datadir="$HA" -rpcport="$RA" "$@"; }
for _ in $(seq 1 20); do
    if cli_a getpeerinfo | python3 -c 'import sys,json; sys.exit(0 if json.load(sys.stdin) else 1)'; then break; fi
    sleep 1
 done

# ─── Assertion 1: getnetworkinfo.node_id_hex ────────────────────────────────
NODE_ID=$(cli_a getnetworkinfo | python3 -c \
    "import sys,json; print(json.load(sys.stdin).get('node_id_hex','MISSING'))")
case "$NODE_ID" in
    MISSING) echo "FAIL: getnetworkinfo missing node_id_hex"; exit 1 ;;
    "")      echo "FAIL: node_id_hex is empty (node_identity_ not initialized?)"; exit 1 ;;
esac
[ "${#NODE_ID}" = "40" ] || { echo "FAIL: node_id_hex length is ${#NODE_ID}, want 40"; exit 1; }
echo "PASS: node_id_hex = $NODE_ID"

# ─── Assertion 2: getpeerinfo[].ping_ms + .quality_score ────────────────────
PEER_FIELDS=$(cli_a getpeerinfo | python3 -c "
import sys,json
peers = json.load(sys.stdin)
if not peers: print('NO_PEERS'); sys.exit(0)
p = peers[0]
have_ping  = 'ping_ms' in p
have_score = 'quality_score' in p
print(f'ping_ms={p.get(\"ping_ms\",\"MISSING\")} quality_score={p.get(\"quality_score\",\"MISSING\")} both_present={have_ping and have_score}')
")
echo "$PEER_FIELDS"
echo "$PEER_FIELDS" | grep -q "both_present=True" || { echo "FAIL: peer fields not both present"; exit 1; }
echo "PASS: getpeerinfo has ping_ms + quality_score"

# ─── Assertion 3: dynamic_p2p.observe shape ─────────────────────────────────
cli_a dynamic_p2p.observe | python3 -c "
import sys,json
d = json.load(sys.stdin)
assert 'enabled' in d and isinstance(d['enabled'], bool), 'enabled missing/wrong type'
assert 'mode'    in d and isinstance(d['mode'], str),     'mode missing/wrong type'
assert 'peers'   in d and isinstance(d['peers'], list),   'peers missing/wrong type'
assert 'governor' in d, 'governor key missing'  # may be null in off-mode
print(f'PASS: dynamic_p2p.observe enabled={d[\"enabled\"]} mode={d[\"mode\"]} peers={len(d[\"peers\"])}')
"

echo ""
echo "=== ALL ASSERTIONS PASS ==="
