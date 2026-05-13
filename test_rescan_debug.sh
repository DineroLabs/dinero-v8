#!/bin/bash
# Debug script to test rescan with full diagnostic output

set -e

DATADIR="/tmp/test_rescan_debug"
RPC_PORT=19999
P2P_PORT=19998

# Clean up
pkill -9 dinerod 2>/dev/null || true
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

# Start daemon in background (without --daemon flag so we get stderr)
echo "Starting daemon..."
/Users/haydarevich/Documents/DineroCoin/build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=$RPC_PORT --port=$P2P_PORT 2>&1 > /tmp/daemon_stderr.log &
DAEMON_PID=$!

echo "Daemon PID: $DAEMON_PID"
sleep 8

# Wait for cookie file
for i in {1..20}; do
    if [ -f "$DATADIR/.cookie" ]; then
        break
    fi
    sleep 1
done

if [ ! -f "$DATADIR/.cookie" ]; then
    echo "Cookie file not created"
    kill $DAEMON_PID 2>/dev/null || true
    exit 1
fi

COOKIE=$(cat "$DATADIR/.cookie" | cut -d: -f2)

echo "Creating wallet..."
CREATE_RESULT=$(curl -s -X POST http://127.0.0.1:$RPC_PORT -u "__cookie__:$COOKIE" \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["test"],"id":1}')

ADDR=$(echo "$CREATE_RESULT" | jq -r '.result.first_address')
echo "Mining address: $ADDR"

echo "Mining 10 blocks..."
curl -s -X POST http://127.0.0.1:$RPC_PORT -u "__cookie__:$COOKIE" \
    -H "Content-Type: application/json" \
    -d "{\"jsonrpc\":\"2.0\",\"method\":\"generatetoaddress\",\"params\":[10,\"$ADDR\"],\"id\":2}" > /dev/null

echo "Triggering rescan..."
curl -s -X POST http://127.0.0.1:$RPC_PORT -u "__cookie__:$COOKIE" \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","method":"wallet.rescanblockchain","params":[],"id":3}' | jq .

sleep 2

# Check daemon stderr output
echo ""
echo "=== Daemon diagnostic output ==="
grep -E "ChainDB|CF\[|forEachUTXO" /tmp/daemon_stderr.log || echo "No diagnostic messages found"

# Kill daemon
kill $DAEMON_PID 2>/dev/null || true
sleep 1
