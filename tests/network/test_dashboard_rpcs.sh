#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Smoke test for the 3 dashboard RPC additions:
#   1. getnetworkinfo.node_id_hex (40-char hex)
#   2. getpeerinfo[].ping_ms + .quality_score (always present)
#   3. dynamic_p2p.observe (valid shape)
#
# Two-node topology:
#   Node A (rpcport=19002, p2pport=19001, HOME=TMP/home_a)
#   Node B (rpcport=19012, p2pport=19011, HOME=TMP/home_b, addnode=A)
#
# Each node gets an isolated HOME so default datadir/lock paths don't collide.
# dinero-cli connects via -rpcport (cookie lives at HOME/.dinero/.cookie).

set -euo pipefail

DINEROD="${DINEROD:?DINEROD must point to dinerod binary}"
DINERO_CLI="${DINERO_CLI:?DINERO_CLI must point to dinero-cli binary}"
TMP="$(mktemp -d)"
HA="$TMP/home_a"
HB="$TMP/home_b"
mkdir -p "$HA" "$HB"

cleanup() {
    HOME="$HA" "$DINERO_CLI" -rpcport=19002 stop 2>/dev/null || true
    HOME="$HB" "$DINERO_CLI" -rpcport=19012 stop 2>/dev/null || true
    sleep 2
    rm -rf "$TMP"
}
trap cleanup EXIT

wait_rpc() {
    local home_dir="$1" port="$2" deadline=$((SECONDS + 30))
    until HOME="$home_dir" "$DINERO_CLI" -rpcport="$port" getnetworkinfo >/dev/null 2>&1; do
        [ $SECONDS -lt $deadline ] || { echo "FAIL: daemon at port $port did not become ready within 30s"; exit 1; }
        sleep 1
    done
}

# Spin two regtest nodes so getpeerinfo has at least one entry.
HOME="$HA" "$DINEROD" --regtest --rpcport=19002 --p2pport=19001 --listen -daemon
wait_rpc "$HA" 19002
echo "Node A ready"

HOME="$HB" "$DINEROD" --regtest --rpcport=19012 --p2pport=19011 \
    --addnode=127.0.0.1:19001 -daemon
wait_rpc "$HB" 19012
echo "Node B ready"

# Allow time for peer handshake to complete.
sleep 6

cli_a() { HOME="$HA" "$DINERO_CLI" -rpcport=19002 "$@"; }

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
