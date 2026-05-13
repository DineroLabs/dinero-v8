#!/usr/bin/env bash
# Start a local Dinero testnet daemon with cookie auth (no username/password).
# - Writes $HOME/.dinero/testnet/dinero.conf by default
# - Starts dinerod and waits for the cookie
# - Prints connection details for Qt wallet/miner
set -euo pipefail

# ---- Defaults (override with flags) ----
BIN="./build/bin/dinerod"
DATADIR="$HOME/.dinero/testnet"
PORT="20998"  # Default testnet RPC port
NETWORK="testnet"   # "testnet" or "mainnet"

usage() {
  cat <<USAGE
Usage: $0 [-b /path/to/dinerod] [-d /custom/datadir] [-p rpcport] [--mainnet|--testnet]
  Defaults:
    -b $BIN
    -d "$DATADIR"
    -p $PORT
    --testnet (use --mainnet to switch)
USAGE
}

# Parse flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--bin) BIN="$2"; shift 2;;
    -d|--datadir) DATADIR="$2"; shift 2;;
    -p|--port) PORT="$2"; shift 2;;
    --mainnet) NETWORK="mainnet"; shift;;
    --testnet) NETWORK="testnet"; shift;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1"; usage; exit 2;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "dinerod binary not found or not executable: $BIN" >&2
  exit 3
fi

echo "=== Dinero local node ==="
echo "Binary : $BIN"
echo "Datadir: $DATADIR"
echo "Network: $NETWORK"
echo "RPC    : 127.0.0.1:$PORT (cookie auth, daemon will use default port)"
echo

# Stop any running dinerod
pkill -f "$BIN" >/dev/null 2>&1 || true
pkill -f dinerod >/dev/null 2>&1 || true
sleep 1

# Clean stale RocksDB locks if no process holds them
LOCK_ROOT="$DATADIR"
[[ "$NETWORK" == "testnet" ]] && LOCK_ROOT="$DATADIR/testnet"
for sub in chainstate blocks index wallet; do
  lock="$LOCK_ROOT/blockchain_data/$sub/LOCK"
  if [[ -f "$lock" ]]; then
    if command -v lsof >/dev/null 2>&1 && lsof "$lock" >/dev/null 2>&1; then
      echo "LOCK in use: $lock (another dinerod running?)" >&2
      exit 4
    else
      rm -f "$lock" || true
    fi
  fi
done

mkdir -p "$DATADIR"

# Write minimal config (no rpcuser/rpcpassword -> cookie auth)
CONF="$DATADIR/dinero.conf"
{
  [[ "$NETWORK" == "testnet" ]] && echo "testnet=1" || true
  echo "server=1"
  echo "rpcbind=127.0.0.1"
  echo "rpcallowip=127.0.0.1"
  # Let daemon use default ports: testnet=20998, mainnet=20998
} > "$CONF"

echo "Wrote config: $CONF"
echo

# Start daemon in background and stream logs to a file
LOGDIR="$DATADIR/logs"; mkdir -p "$LOGDIR"
LOGFILE="$LOGDIR/dinerod-$(date +%Y%m%d-%H%M%S).log"

# Some builds support -daemon; we don't rely on it.
"$BIN" -conf="$CONF" -datadir="$DATADIR" -printtoconsole > "$LOGFILE" 2>&1 &
PID=$!
echo "Started dinerod (pid=$PID). Logs: $LOGFILE"
echo "Waiting for cookie..."

# Wait for cookie file
COOKIE_PATH="$DATADIR/.cookie"
[[ "$NETWORK" == "testnet" ]] && COOKIE_PATH="$DATADIR/testnet/.cookie"

for i in {1..60}; do
  if [[ -f "$COOKIE_PATH" ]]; then
    break
  fi
  sleep 0.5
done

if [[ ! -f "$COOKIE_PATH" ]]; then
  echo "❌ Cookie not found after 30s. Tail of log:"
  tail -n 50 "$LOGFILE" || true
  exit 5
fi

COOKIE="$(cat "$COOKIE_PATH")"
AUTH_HEADER="Basic $(printf %s "$COOKIE" | base64)"
echo "✅ Cookie: $COOKIE"
echo

echo "=== Ready ==="
echo "RPC URL     : http://127.0.0.1:$PORT/"
echo "Auth (curl) : curl --user \"$COOKIE\" -H 'content-type: application/json' \\"
echo "  --data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"c\",\"method\":\"getblockchaininfo\",\"params\":[]}' \\"
echo "  http://127.0.0.1:$PORT/"
echo
echo "Qt wallet/miner:"
echo "  • Host   = 127.0.0.1"
echo "  • Port   = $PORT"
echo "  • Auth   = COOKIE"
echo "    - Username = __cookie__"
echo "    - Password = (read from $COOKIE_PATH)"
echo
echo "Tip: your app can auto-read the cookie whenever it changes (daemon restart rotates it)."
