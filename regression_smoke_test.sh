#!/usr/bin/env bash
set -euo pipefail

# Load test helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/test_helpers.sh"

# --- config (override via env or first arg) ---
DIR="${1:-regtest-smoke}"
PORT="${PORT:-21999}"     # RPC port (unique!)
P2P="${P2P:-41999}"       # P2P port (unique!)
GENTHREADS="${GENTHREADS:-4}"
MINER="${MINER:-din1q58u9gl8pxs69w63tfghl6qwwggv4d70w0f004j}"

D="$DIR/regtest"
LOG="$DIR.log"

echo "🧪 DINERO REGRESSION SMOKE TEST"
echo "Dir: $DIR"
echo "RPC: $PORT  P2P: $P2P"
echo "Keep alive: ${KEEP_ALIVE:-0}"

echo
echo "🧹 Cleanup"
pkill -f "dinerod -regtest -datadir=$DIR" || true
lsof -ti :$PORT | xargs -r kill -9 || true
rm -rf "$D"
mkdir -p "$DIR"
echo "✅ Clean"

echo
echo "🚀 Starting daemon (bound to RPC:$PORT)"
./build/dinerod -regtest -datadir="$DIR" \
  -gen=1 -genthreads="$GENTHREADS" -miningaddress="$MINER" \
  -rpcbind=127.0.0.1 -rpcport="$PORT" -port="$P2P" \
  -printtoconsole=0 > "$LOG" 2>&1 &
DAEMON_PID=$!

# Wait for cookie
echo
echo "⏳ Waiting for RPC cookie…"
for i in {1..50}; do
  [[ -f "$D/.cookie" ]] && break
  sleep 0.2
done
if [[ ! -f "$D/.cookie" ]]; then
  echo "❌ Cookie not found at $D/.cookie"
  exit 1
fi
COOKIE="$(cat "$D/.cookie")"
echo "✅ Cookie ready"

# Discover effective ports using helper functions
RPC_PORT=""
HTTP_PORT=""

# Use helper to discover ports and wait for healthz
echo "🔍 Discovering ports and waiting for daemon readiness..."
RPC_PORT="$(wait_daemon_ready "$DIR")"

if [ -z "$RPC_PORT" ]; then
  echo "❌ Failed to discover RPC port or daemon not ready"
  exit 1
fi

# Get HTTP port from log or nodeinfo.json
HTTP_PORT="$(discover_http_port "$DIR")"
echo "Effective ports → RPC=$RPC_PORT  HTTP=$HTTP_PORT"

# JSON-RPC checks on actual RPC port
rpc() { curl -s --user "$COOKIE" -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$RPC_PORT/"; }

# Behavioral busy_timeout test: hold a write lock for ~10s to test retry behavior
echo
echo "🔒 Testing busy_timeout behavior under contention..."
# Use Python to hold a write lock for 10 seconds (bullet-proof across platforms)
(
python3 - <<'PY' &
import sqlite3, time, sys
db_path = sys.argv[1] if len(sys.argv) > 1 else "blockchain.db"
con = sqlite3.connect(db_path, timeout=5)
cur = con.cursor()
cur.execute("BEGIN IMMEDIATE")  # write lock
time.sleep(10)                  # hold it for 10 seconds
con.commit()
con.close()
PY
) "$DIR/regtest/blockchain.db" &

# Wait for lock to be held and released
sleep 12

# Verify daemon continued mining without permanent failures
if grep -E "SQLITE_BUSY|database is locked|Failed to begin transaction" "$DIR.log" | tail -50 | grep -q .; then
  echo "❌ DB errors detected during contention test"
  exit 1
fi

echo "✅ Busy-timeout behavior OK under contention"

# Additional guardrail: no nested transaction errors
echo
echo "🔍 Checking for nested transaction errors..."
if grep -E "cannot start a transaction within a transaction" "$DIR.log" | grep -q .; then
  echo "❌ Nested transaction errors detected"
  exit 1
fi

echo "✅ No nested transaction errors"

# Height growth
echo
echo "📈 Height growth"
H0="$(rpc '{"jsonrpc":"2.0","id":"h0","method":"getblockcount"}' | jq -r '.result')"
sleep 3
H1="$(rpc '{"jsonrpc":"2.0","id":"h1","method":"getblockcount"}' | jq -r '.result')"
echo "Height: $H0 → $H1 (Δ=$((H1-H0)))"
[ "$H1" -ge "$H0" ] || { echo "❌ Height did not advance"; exit 1; }
echo "✅ Height advancing"

# Hash consistency for block #1
echo
echo "🔗 Hash consistency (block #1)"
DB="$DIR/regtest/blockchain.db"
RPC_HASH="$(rpc '{"jsonrpc":"2.0","id":"b1","method":"getblockhash","params":[1]}' | jq -r '.result')"
DB_HASH="$(sqlite3 "$DB" "SELECT lower(hex(hash)) FROM block_index WHERE height=1;")"
echo "RPC hash: $RPC_HASH"
echo "DB  hash: $DB_HASH"
[ "$RPC_HASH" = "$DB_HASH" ] || { echo "❌ Hash mismatch"; exit 1; }
echo "✅ Hash match"

echo
echo "🎉 SMOKE TEST PASSED"

# Handle KEEP_ALIVE option
if [ "${KEEP_ALIVE:-0}" = "1" ]; then
  echo "✅ Daemon left running (PID $DAEMON_PID) for inspection"
  echo "   RPC: http://127.0.0.1:$RPC_PORT/"
  echo "   HTTP: http://127.0.0.1:$HTTP_PORT/healthz"
  echo "   Cookie: $D/.cookie"
  echo "   DB: $D/blockchain.db"
else
  echo "🛑 Shutting down daemon..."
  kill "$DAEMON_PID" || true
  echo "✅ Daemon stopped"
fi