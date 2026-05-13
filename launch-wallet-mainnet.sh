#!/bin/bash
set -e

DATADIR="$HOME/.dinero"
COOKIE_FILE="$DATADIR/.cookie"
RPC_URL="http://127.0.0.1:20998/"

rpc_ready() {
    [ -f "$COOKIE_FILE" ] || return 1
    local cookie
    cookie=$(cat "$COOKIE_FILE")
    curl -sf --user "$cookie" \
        --data-binary '{"jsonrpc":"1.0","id":"launch-wallet","method":"getblockcount","params":[]}' \
        -H 'content-type:text/plain;' \
        "$RPC_URL" >/dev/null
}

echo "🚀 Launching Dinero Qt Wallet (MAINNET)..."
echo "📂 Data directory: $DATADIR"
echo "🔐 Cookie file: $COOKIE_FILE"
echo "🌐 RPC Server: $RPC_URL"
echo ""

# Make sure mainnet daemon is running
if ! rpc_ready; then
    echo "⚠️  Mainnet daemon not running. Starting it now..."
    ./build/dinerod -datadir="$DATADIR" -daemon -rpcport=20998 -port=20999
    echo "⏳ Waiting 5 seconds for daemon to initialize..."
    sleep 5
fi

# Launch GUI with mainnet settings
./gui/build/dinero-qt \
    -datadir="$DATADIR" \
    -rpcconnect=127.0.0.1 \
    -rpcport=20998 \
    "$@"
