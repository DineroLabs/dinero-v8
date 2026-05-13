#!/usr/bin/env bash
set -euo pipefail

DIN_DAEMON="${DIN_DAEMON:-./build/bin/dinerod}"

NETWORK="${NETWORK:-regtest}"   # regtest|testnet|mainnet
RPC_PORT="${RPC_PORT:-26660}"
WS_PORT="${WS_PORT:-26662}"
P2P_PORT="${P2P_PORT:-0}"
DATADIR="${DATADIR:-$(mktemp -d -t din.XXXXXX)}"

# Map NETWORK -> daemon flag + subdir name
case "$NETWORK" in
  regtest) NETFLAG="-regtest";  CHAIN="regtest" ;;
  testnet) NETFLAG="-testnet";  CHAIN="testnet" ;;
  mainnet|"") NETFLAG="";       CHAIN="mainnet" ;;
  *) echo "Unknown NETWORK=$NETWORK"; exit 2 ;;
esac

echo "🚀 Starting daemon smoke test..."
echo "📁 Test directory: $DATADIR"
echo "🔌 Requested: RPC=$RPC_PORT WS=$WS_PORT  🌐 Network=$NETWORK"

# Start daemon with CORRECT flags
"$DIN_DAEMON" \
  -daemon \
  ${NETFLAG} \
  -datadir="$DATADIR" \
  -rpcport="$RPC_PORT" \
  -wsport="$WS_PORT" \
  -port="$P2P_PORT" \
  >"$DATADIR/daemon.log" 2>&1 || {
    echo "❌ Failed to launch dinerod"
    exit 1
  }

# Wait for banners
COOKIE_PATH=""
EFFECTIVE_HTTP=""

# Try to parse "RPC READY on http://host:port/"
READY_URL="$(grep -Eo 'RPC READY on http://[^ ]+' "$DATADIR/daemon.log" | awk '{print $5}' | tail -n1 || true)"
if [[ -n "$READY_URL" ]]; then
  EFFECTIVE_HTTP="$READY_URL"
fi

# If not found, parse "Separate ports mode: RPC=X HTTP=Y"
if [[ -z "$EFFECTIVE_HTTP" ]]; then
  HTTP_PORT_LINE="$(grep -E 'Separate ports mode: RPC=[0-9]+ HTTP=[0-9]+' "$DATADIR/daemon.log" | tail -n1 || true)"
  if [[ -n "$HTTP_PORT_LINE" ]]; then
    HTTP_PORT="$(sed -E 's/.*HTTP=([0-9]+).*/\1/' <<<"$HTTP_PORT_LINE")"
    EFFECTIVE_HTTP="http://127.0.0.1:${HTTP_PORT}"
  fi
fi

# Final fallback: use requested RPC_PORT (works in unified mode)
[[ -z "$EFFECTIVE_HTTP" ]] && EFFECTIVE_HTTP="http://127.0.0.1:${RPC_PORT}"

# Cookie path (prefer explicit banner if present)
COOKIE_PATH="$(grep -Eo 'RPC cookie generated: .*\.cookie' "$DATADIR/daemon.log" | sed -E 's/.*: //; s/\r$//' | tail -n1 || true)"
[[ -z "$COOKIE_PATH" ]] && COOKIE_PATH="$DATADIR/$CHAIN/.cookie"

# Wait for cookie and HTTP to be ready
for _ in {1..100}; do [[ -f "$COOKIE_PATH" ]] && break; sleep 0.05; done
if [[ ! -f "$COOKIE_PATH" ]]; then
  echo "❌ Cookie file not found: $COOKIE_PATH"
  tail -n 60 "$DATADIR/daemon.log" || true
  exit 1
fi

# Wait for the HTTP port to accept connections
HTTP_PORT_TO_CHECK="$(sed -E 's#.*/##; s/.*:([0-9]+).*/\1/' <<<"$EFFECTIVE_HTTP")"
for _ in {1..200}; do
  (exec 3<>/dev/tcp/127.0.0.1/"$HTTP_PORT_TO_CHECK") >/dev/null 2>&1 && { exec 3>&-; break; }
  sleep 0.05
done

echo "✅ Cookie: $COOKIE_PATH"
echo "🔍 HTTP JSON-RPC: $EFFECTIVE_HTTP"

AUTH_RAW="$(cat "$COOKIE_PATH")"
# cookie can be "user:token" or just "token"
if [[ "$AUTH_RAW" == *:* ]]; then CURL_AUTH="$AUTH_RAW"; else CURL_AUTH="$AUTH_RAW:x"; fi

# Simple RPC probe
curl -sS -u "$CURL_AUTH" \
  -H "Content-Type: application/json" \
  --data '{"jsonrpc":"2.0","id":1,"method":"getnetworkinfo","params":[]}' \
  "$EFFECTIVE_HTTP" | jq . || {
    echo "❌ RPC request failed"
    tail -n 80 "$DATADIR/daemon.log" || true
    exit 1
  }

# Shutdown & cleanup
# Try a graceful stop if implemented; otherwise kill
curl -sS -u "$CURL_AUTH" -H "Content-Type: application/json" \
  --data '{"jsonrpc":"2.0","id":2,"method":"stop","params":[]}' \
  "$EFFECTIVE_HTTP" >/dev/null 2>&1 || true

sleep 0.5
pkill -f "$(basename "$DIN_DAEMON")" >/dev/null 2>&1 || true
rm -rf "$DATADIR"
echo "🧹 Cleaning up..."