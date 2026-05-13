#!/usr/bin/env bash
set -euo pipefail
ROOT="$HOME/Documents/DineroCoin"
BIN="$ROOT/build/bin/dinerod"
DATADIR="$ROOT/test-data/regtest"
PORT=20999

echo "🚀 Starting Dinero daemon (regtest)"
echo "   Binary: $BIN"
echo "   Data:   $DATADIR"
echo "   Port:   $PORT"
echo ""

# Kill any existing daemon
pkill -f "$BIN" || true
sleep 1

# Start the daemon
"$BIN" -regtest -datadir="$DATADIR" -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 -rpcport=$PORT -printtoconsole=1
