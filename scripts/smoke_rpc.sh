#!/usr/bin/env bash
set -euo pipefail

# Test RPC authentication with both cookie formats
echo "🧪 Testing RPC authentication..."

# Defaults (if not provided by env/CTest)
: "${NETWORK:=regtest}"
: "${RPC_PORT:=26660}"
: "${DIN_DAEMON:=./build/bin/dinerod}"

# Isolated temp datadir
DATADIR="$(mktemp -d "/tmp/din.rpc.XXXXXX")"

# Map NETWORK -> chain flag
NET_FLAG=""
CHAIN_SUBDIR=""
case "$NETWORK" in
  regtest) NET_FLAG="-regtest"; CHAIN_SUBDIR="regtest" ;;
  testnet) NET_FLAG="-testnet"; CHAIN_SUBDIR="testnet" ;;
  *)       NET_FLAG="";          CHAIN_SUBDIR="mainnet" ;;
esac

echo "📁 Test directory: $DATADIR"
echo "🔌 RPC Port: $RPC_PORT"
echo "🌐 Network: $NETWORK"

LOG="$DATADIR/daemon.log"
trap 'kill ${DAEMON_PID:-0} >/dev/null 2>&1 || true; rm -rf "$DATADIR"' EXIT

# Launch daemon with explicit network + datadir + rpcport
"$DIN_DAEMON" $NET_FLAG -datadir="$DATADIR" -rpcport="$RPC_PORT" -daemon -printtoconsole >>"$LOG" 2>&1 &
DAEMON_PID=$!
echo "   PID: $DAEMON_PID"

# Wait for cookie in the *known* datadir
COOKIE="$DATADIR/$CHAIN_SUBDIR/.cookie"
for i in {1..50}; do
  [ -f "$COOKIE" ] && break
  sleep 0.1
done
[ -f "$COOKIE" ] || { echo "❌ Cookie file not found: $COOKIE"; tail -n 40 "$LOG"; exit 1; }

echo "✅ Cookie found: $COOKIE"

# 1) Derive the actual HTTP JSON-RPC port from the log
HTTP_PORT=""
if grep -q "Separate ports mode:" "$LOG"; then
  HTTP_PORT="$(grep "Separate ports mode:" "$LOG" | tail -1 | sed -n 's/.*HTTP=\([0-9]*\).*/\1/p')"
fi
# Fallback: some builds print a "RPC READY" line with a full URL
if [ -z "$HTTP_PORT" ]; then
  HTTP_PORT="$(grep -Eo 'RPC READY on http://127\.0\.0\.1:[0-9]+' "$LOG" | tail -1 | sed -E 's/.*:([0-9]+)/\1/')"
fi
# Final fallback to the known default
: "${HTTP_PORT:=20999}"

echo "🌐 HTTP JSON-RPC: http://127.0.0.1:${HTTP_PORT}"

# 2) Wait until the HTTP port is accepting connections
ready=0
for i in {1..50}; do
  code="$(curl -sS --connect-timeout 1 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${HTTP_PORT}/")"
  if [ "$code" != "000" ]; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  echo "❌ HTTP JSON-RPC not reachable on ${HTTP_PORT}"
  tail -n 40 "$LOG"
  exit 1
fi

# 3) Do an authenticated JSON-RPC call using the cookie
USERPASS="$(cat "$COOKIE")"
curl -sS --fail --user "$USERPASS" \
  -H 'content-type: text/plain' \
  --data-binary '{"jsonrpc":"2.0","id":"smoke","method":"getnetworkinfo","params":[]}' \
  "http://127.0.0.1:${HTTP_PORT}/" >/dev/null \
  && echo "✅ RPC auth OK" || { echo "❌ RPC call failed"; exit 1; }