#!/bin/bash
# Start Dinero in TESTNET mode locally (Mac)

set -e

echo "🧪 STARTING DINERO TESTNET (Local Mac)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Check if daemon exists
if [ ! -f "build/bin/dinerod" ]; then
    echo "❌ dinerod not found. Building..."
    cmake --build build -j8
fi

# Create testnet data directory
mkdir -p data/testnet
mkdir -p logs

# Stop existing daemon if running
pkill -f dinerod || true
sleep 2

echo "Starting testnet daemon..."
echo ""
echo "Genesis:"
echo "  Hash: 10992d751621c536a49998d1d007a97f270a1db3eddb3ef60f2c9946398d927e"
echo "  Economics: 97.85M DIN (99 burned + 1M premine)"
echo ""
echo "Network:"
echo "  Mode: TESTNET"
echo "  RPC: http://localhost:19998"
echo "  P2P: localhost:19999"
echo ""

# Start daemon in testnet mode
./build/bin/dinerod \
  --testnet \
  --datadir=./data/testnet \
  --rpcport=19998 \
  --port=19999 \
  2>&1 | tee logs/testnet.log &

DAEMON_PID=$!

echo "✅ Testnet daemon started (PID: $DAEMON_PID)"
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "📊 MONITORING"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Check logs:"
echo "  tail -f logs/testnet.log"
echo ""
echo "Check status:"
echo "  curl -X POST http://localhost:19998 -d '{\"method\":\"getblockchaininfo\"}'"
echo ""
echo "Stop daemon:"
echo "  kill $DAEMON_PID"
echo "  # or: pkill -f dinerod"
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "🧪 TESTNET TESTING CHECKLIST"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Test these over 1-2 weeks:"
echo "  [ ] Genesis loads correctly"
echo "  [ ] Mining works (CPU-friendly)"
echo "  [ ] Wallet creates addresses"
echo "  [ ] Transactions work"
echo "  [ ] P2P sync works"
echo "  [ ] GUI connects"
echo "  [ ] Block rewards correct"
echo "  [ ] Consensus rules work"
echo ""
echo "When all tests pass → Launch MAINNET!"
echo ""

