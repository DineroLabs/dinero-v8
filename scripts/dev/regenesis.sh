#!/usr/bin/env bash
set -euo pipefail

# Regenesis development script
# Rebuilds, computes fresh genesis, and starts clean regtest daemon

echo "🔄 Building dinerod and genesis_print..."
cmake --build build-test -j4

echo "🧮 Computing regtest genesis constants..."
./build-test/genesis_print > /tmp/genesis.txt

echo "📋 Genesis constants:"
cat /tmp/genesis.txt

echo "🗑️  Cleaning test data..."
rm -rf ./test_data

echo "🚀 Starting fresh regtest daemon..."
echo "   RPC: http://127.0.0.1:20998/"
echo "   Health: http://127.0.0.1:20999/healthz"
echo "   Data: ./test_data/regtest/"
echo ""

# Use background + log + kill pattern for macOS (no timeout command)
./build-test/dinerod --datadir=./test_data --rpcport=20998 --regtest --printtoconsole > run.log 2>&1 &
PID=$!

echo "⏱️  Waiting 5 seconds for startup..."
sleep 5

echo "📊 Last 30 lines of startup log:"
tail -n 30 run.log

echo ""
echo "🔍 Checking for issues:"
if grep -q "Genesis hash mismatch" run.log; then
    echo "❌ Genesis hash mismatch still present"
else
    echo "✅ No genesis hash mismatch"
fi

if grep -q "segmentation fault\|abort" run.log; then
    echo "❌ Segmentation fault detected"
else
    echo "✅ No segmentation fault"
fi

if grep -q "RPC READY" run.log; then
    echo "✅ RPC server started successfully"
else
    echo "❌ RPC server failed to start"
fi

echo ""
echo "🎯 Daemon PID: $PID"
echo "📝 Full log: run.log"
echo "🛑 To stop: kill $PID"
echo ""
echo "🧪 Quick test commands:"
echo "   curl -s http://127.0.0.1:20999/healthz | jq ."
echo "   curl -s -X POST http://127.0.0.1:20998/ -H 'content-type: application/json' -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getnetworkinfo\",\"params\":[]}' | jq ."


