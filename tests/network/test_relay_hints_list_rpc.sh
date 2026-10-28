#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Integration test for relay_hints.list RPC + getnetworkinfo.relay 24h-counter
# fields (Phase 2b daemon).
#
# Two-node topology with explicit private datadirs, ephemeral ports and
# owned foreground child processes, matching test_dashboard_rpcs.sh.
#
# Asserts:
#   1. relay_hints.list returns the documented shape (rpc_schema, ttl_seconds,
#      max_failures, targets array) — smoke test on an empty hint cache.
#   2. getnetworkinfo.relay has blocks_served_24h + bytes_relayed_24h fields
#      as numeric uint64 on Node A.
#   3. Symmetric: both fields present on Node B (non-relay node).
#
# Note: 2 plain regtest nodes do not exchange RELAY_HINTS gossip unless one is
# a registered relay, so total_targets stays 0. The assertions below test the
# empty-cache shape guarantee and the 24h field presence — sufficient to cover
# the Phase 2b RPC surface.

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
        echo "Relay RPC evidence: $TMP" >&2
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
cli_b() { "$DINERO_CLI" -datadir="$HB" -rpcport="$RB" "$@"; }
sleep 6

# ─── Assertion 1: relay_hints.list empty-cache shape ────────────────────────
HINTS=$(cli_a relay_hints.list)

cli_a relay_hints.list | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d.get('rpc_schema') == 'din.rpc.v1',  f\"rpc_schema wrong: {d.get('rpc_schema')!r}\"
assert isinstance(d.get('total_targets'), int) and d['total_targets'] >= 0, \
    f\"total_targets bad: {d.get('total_targets')!r}\"
assert d.get('ttl_seconds') == 900, \
    f\"ttl_seconds wrong: {d.get('ttl_seconds')!r} (want 900 = kHintTtl 15min)\"
assert d.get('max_failures') == 3, \
    f\"max_failures wrong: {d.get('max_failures')!r} (want kHintMaxFailures=3)\"
assert isinstance(d.get('targets'), list), \
    f\"targets not a list: {type(d.get('targets'))}\"
print(f\"PASS: relay_hints.list shape ok — total_targets={d['total_targets']} ttl={d['ttl_seconds']}s max_failures={d['max_failures']}\")
"

# ─── Assertion 1b: relayhints.dial is registered + object-parameter safe ───
cli_a relayhints.dial '{"target_node_id_hex":"abc"}' | python3 -c "
import sys, json
payload = json.load(sys.stdin)
err = payload.get('error')
assert isinstance(err, dict), f'expected error object, got {payload!r}'
assert err.get('code') == -8, f'expected -8 invalid params, got {err!r}'
print('PASS: relayhints.dial invalid target returns JSON error shape')
"

cli_a relayhints.dial \
    '{"target_node_id_hex":"00112233445566778899aabbccddeeff00112233","dry_run":true}' \
    | python3 -c "
import sys, json
payload = json.load(sys.stdin)
assert payload.get('rpc_schema') == 'din.rpc.v1', f'rpc_schema wrong: {payload!r}'
assert payload.get('status') == 'no_hint', f'expected no_hint, got {payload!r}'
assert payload.get('submitted') is False, f'expected submitted=false, got {payload!r}'
assert payload.get('request_id') == 0, f'expected request_id=0, got {payload!r}'
print('PASS: relayhints.dial dry-run unknown target returns no_hint')
"

# ─── Assertion 2: getnetworkinfo.relay has 24h counter fields (Node A) ──────
cli_a getnetworkinfo | python3 -c "
import sys, json
d = json.load(sys.stdin)
relay = d.get('relay')
assert relay is not None, 'getnetworkinfo missing relay key'
assert 'blocks_served_24h' in relay, 'relay missing blocks_served_24h'
assert 'bytes_relayed_24h' in relay, 'relay missing bytes_relayed_24h'
bsrv = relay['blocks_served_24h']
brel = relay['bytes_relayed_24h']
assert isinstance(bsrv, int) and bsrv >= 0, f'blocks_served_24h invalid: {bsrv!r}'
assert isinstance(brel, int) and brel >= 0, f'bytes_relayed_24h invalid: {brel!r}'
print(f'PASS: getnetworkinfo.relay blocks_served_24h={bsrv} bytes_relayed_24h={brel}')
"

# ─── Assertion 3: symmetric — both fields present on non-relay node (B) ─────
cli_b getnetworkinfo | python3 -c "
import sys, json
d = json.load(sys.stdin)
relay = d.get('relay')
assert relay is not None, 'Node B: getnetworkinfo missing relay key'
assert 'blocks_served_24h' in relay, 'Node B: relay missing blocks_served_24h'
assert 'bytes_relayed_24h' in relay, 'Node B: relay missing bytes_relayed_24h'
print(f'PASS: Node B getnetworkinfo.relay symmetry ok — blocks_served_24h={relay[\"blocks_served_24h\"]} bytes_relayed_24h={relay[\"bytes_relayed_24h\"]}')
"

echo ""
echo "=== ALL ASSERTIONS PASS ==="
