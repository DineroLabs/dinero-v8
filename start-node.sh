#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

: "${NET:=mainnet}"
case "${NET}" in
  mainnet)
    DEFAULT_RPCPORT=20998
    DEFAULT_P2P_PORT=20999
    DEFAULT_DATADIR="$HOME/.dinero"
    ;;
  testnet)
    DEFAULT_RPCPORT=20998
    DEFAULT_P2P_PORT=21000
    DEFAULT_DATADIR="$HOME/.dinero/testnet"
    ;;
  regtest)
    DEFAULT_RPCPORT=20996
    DEFAULT_P2P_PORT=21001
    DEFAULT_DATADIR="$HOME/.dinero/regtest"
    ;;
  *) echo "Unknown NET='$NET'"; exit 2 ;;
esac

: "${RPCPORT:=${DEFAULT_RPCPORT}}"
: "${P2P_PORT:=${DEFAULT_P2P_PORT}}"
: "${DATADIR:=${DINERO_DATADIR:-${DEFAULT_DATADIR}}}"
BIN="${DINEROD_BIN:-$PWD/build/dinerod}"
LOG="$DATADIR/logs/dinerod.log"
PIDFILE="$DATADIR/run/dstart.pid"

mkdir -p "$DATADIR" "$DATADIR/logs" "$DATADIR/run"
ulimit -n 4096 || true

if [ ! -x "$BIN" ] && [ -x "$PWD/build-clean/dinerod" ]; then
  BIN="$PWD/build-clean/dinerod"
fi

if [ ! -x "$BIN" ]; then
  echo "❌ Cannot find dinerod binary at $PWD/build/dinerod or $PWD/build-clean/dinerod"
  exit 1
fi

rpc_ready() {
  local cookie_file="$DATADIR/.cookie"
  [ -f "$cookie_file" ] || return 1
  local cookie
  cookie=$(cat "$cookie_file")
  curl -sf --user "$cookie" \
    --data-binary '{"jsonrpc":"1.0","id":"start-node","method":"getblockcount","params":[]}' \
    -H 'content-type:text/plain;' \
    "http://127.0.0.1:$RPCPORT/" >/dev/null
}

# already running?
if rpc_ready; then
  echo "✅ dinerod already responding on the canonical datadir"
  exit 0
fi

if [ -f "$PIDFILE" ] && ps -p "$(cat "$PIDFILE")" -o comm= 2>/dev/null | grep -q dinerod; then
  echo "✅ dinerod already running (pid $(cat "$PIDFILE"))"
  exit 0
fi

# don't leave any stray old process with THIS datadir
if pgrep -f "dinerod.*${DATADIR}" >/dev/null 2>&1; then
  echo "🧹 Cleaning up old process..."
  pkill -f "dinerod.*${DATADIR}"
  sleep 2
fi

NETFLAGS=()
[[ "$NET" == "testnet" ]] && NETFLAGS+=( -testnet=1 )
[[ "$NET" == "regtest" ]] && NETFLAGS+=( -regtest=1 )

ADDNODE_FLAGS=()
if [[ "$NET" == "mainnet" ]]; then
  ADDNODE_FLAGS=(
    -addnode=96.9.226.98:20999
    -addnode=173.249.195.59:20999
    -addnode=172.93.160.131:20999
  )
fi

echo "🚀 Starting Dinero node..."
echo "   Data dir: $DATADIR"
echo "   Binary: $BIN"
echo "   RPC: http://127.0.0.1:$RPCPORT/"
echo "   P2P: 127.0.0.1:$P2P_PORT"

# start and detach; do NOT set any trap that kills children
nohup "$BIN" -datadir="$DATADIR" \
  -listen=1 -rpcbind=127.0.0.1 -rpcport="$RPCPORT" -port="$P2P_PORT" \
  "${NETFLAGS[@]}" \
  "${ADDNODE_FLAGS[@]}" \
  >>"$LOG" 2>&1 &

PID=$!
echo $PID > "$PIDFILE"
sleep 3

if ps -p "$PID" >/dev/null 2>&1; then
  echo "✅ Node started successfully (PID $PID)"
  echo "   RPC: http://127.0.0.1:$RPCPORT/"
  echo "   Cookie: $DATADIR/.cookie"
  echo "   Logs: tail -f $LOG"
else
  echo "❌ Node failed to start. Last 20 lines:"
  tail -20 "$LOG"
  rm -f "$PIDFILE"
  exit 1
fi
