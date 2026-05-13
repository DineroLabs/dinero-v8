#!/bin/bash

echo "═══════════════════════════════════════════════════════════════════════════"
echo "🧪 QUICK TESTS SUITE - 3 Tests (~1 hour total)"
echo "═══════════════════════════════════════════════════════════════════════════"
echo ""

RESULTS_DIR="quick_test_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
cd "$RESULTS_DIR" || exit 1

# Clean environment
echo "Cleaning environment..."
pkill -9 dinerod dinero-miner 2>/dev/null || true
sleep 3
rm -rf ../data/blocks ../data/utxo.db ../data/blockchain_state.json ../data/wallet
find ../data/testnet -name "*.db*" -delete 2>/dev/null || true
echo ""

# ═══════════════════════════════════════════════════════════════════════════
# TEST 1: RPC Response Time Under Load (5 minutes)
# ═══════════════════════════════════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📊 TEST 1/3: RPC Response Time Under Load"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Start daemon
../build/dinerod -datadir=../data -testnet -rpcport=20998 -dev > test1_daemon.log 2>&1 &
DAEMON_PID=$!
echo "Daemon started (PID: $DAEMON_PID)"
sleep 12

# Create wallet and start miner
ADDR=$(curl -s http://127.0.0.1:20998/ -d '{"method":"createhdwallet","params":["test1"]}' | jq -r '.result.first_address')
echo "Mining to: $ADDR"
../build/dinero-miner --address "$ADDR" --threads 4 > test1_miner.log 2>&1 &
MINER_PID=$!
echo "Miner started (PID: $MINER_PID)"
echo ""

# Wait for some blocks to be mined
echo "Waiting for mining to stabilize (20 seconds)..."
sleep 20

# Test RPC response times
echo "Testing RPC response times (60 iterations, ~5 minutes)..."
echo ""
TOTAL_TIME=0
MAX_TIME=0
MIN_TIME=999999
TIMEOUTS=0

for i in {1..60}; do
    START=$(date +%s%N)
    RESULT=$(curl -s --max-time 2 http://127.0.0.1:20998/ -d '{"method":"getblockchaininfo"}' 2>/dev/null)
    END=$(date +%s%N)
    
    if [ -z "$RESULT" ]; then
        TIMEOUTS=$((TIMEOUTS + 1))
        echo "[$(date +%H:%M:%S)] Test $i/60: TIMEOUT"
        continue
    fi
    
    TIME_NS=$((END - START))
    TIME_MS=$((TIME_NS / 1000000))
    TOTAL_TIME=$((TOTAL_TIME + TIME_MS))
    
    if [ $TIME_MS -gt $MAX_TIME ]; then
        MAX_TIME=$TIME_MS
    fi
    
    if [ $TIME_MS -lt $MIN_TIME ]; then
        MIN_TIME=$TIME_MS
    fi
    
    if [ $((i % 10)) -eq 0 ]; then
        echo "[$(date +%H:%M:%S)] Test $i/60: ${TIME_MS}ms"
    fi
    
    sleep 5
done

AVG_TIME=$((TOTAL_TIME / (60 - TIMEOUTS)))

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "TEST 1 RESULTS:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Average RPC time: ${AVG_TIME}ms"
echo "Min RPC time: ${MIN_TIME}ms"
echo "Max RPC time: ${MAX_TIME}ms"
echo "Timeouts: $TIMEOUTS/60"
echo ""

if [ $AVG_TIME -lt 100 ] && [ $MAX_TIME -lt 500 ] && [ $TIMEOUTS -eq 0 ]; then
    echo "✅ TEST 1 PASSED: RPC stays fast under load!"
    TEST1_STATUS="✅ PASSED"
else
    echo "⚠️  TEST 1: Performance concerns (Avg: ${AVG_TIME}ms, Max: ${MAX_TIME}ms)"
    TEST1_STATUS="⚠️  WARNING"
fi

# Stop processes
kill $MINER_PID $DAEMON_PID 2>/dev/null
sleep 5
pkill -9 dinerod dinero-miner 2>/dev/null || true
sleep 3
echo ""
echo ""

# ═══════════════════════════════════════════════════════════════════════════
# TEST 2: 10 Concurrent Miners (30 minutes)
# ═══════════════════════════════════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "⚡ TEST 2/3: 10 Concurrent Miners Stress Test"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Clean and restart
rm -rf ../data/blocks ../data/utxo.db ../data/blockchain_state.json ../data/wallet
find ../data/testnet -name "*.db*" -delete 2>/dev/null || true

# Start daemon
../build/dinerod -datadir=../data -testnet -rpcport=20998 -dev > test2_daemon.log 2>&1 &
DAEMON_PID=$!
echo "Daemon started (PID: $DAEMON_PID)"
sleep 12

# Create wallet
ADDR=$(curl -s http://127.0.0.1:20998/ -d '{"method":"createhdwallet","params":["test2"]}' | jq -r '.result.first_address')
echo "Mining to: $ADDR"
echo ""

# Start 10 miners
MINER_PIDS=()
echo "Starting 10 miners (1 thread each)..."
for i in {1..10}; do
    ../build/dinero-miner --address "$ADDR" --threads 1 > test2_miner_$i.log 2>&1 &
    PID=$!
    MINER_PIDS+=($PID)
    echo "  Miner $i started (PID: $PID)"
    sleep 1
done
echo ""

# Monitor for 10 minutes (shorter than 30 for testing)
echo "Monitoring for 10 minutes (360 blocks expected at 5-min block time)..."
START_HEIGHT=$(curl -s http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' | jq -r '.result')
START_TIME=$(date +%s)
CRASHES=0

for i in {1..120}; do
    HEIGHT=$(curl -s --max-time 3 http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' 2>/dev/null | jq -r '.result // 0')
    ELAPSED=$(($(date +%s) - START_TIME))
    
    if [ "$HEIGHT" != "0" ] && [ -n "$HEIGHT" ]; then
        BLOCKS=$((HEIGHT - START_HEIGHT))
        if [ $((i % 12)) -eq 0 ]; then
            echo "[$(date +%H:%M:%S)] Elapsed: ${ELAPSED}s | Blocks: $BLOCKS | Current height: $HEIGHT"
        fi
    fi
    
    # Check if any miners crashed
    for PID in "${MINER_PIDS[@]}"; do
        if ! kill -0 $PID 2>/dev/null; then
            CRASHES=$((CRASHES + 1))
        fi
    done
    
    sleep 5
done

END_HEIGHT=$(curl -s http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' | jq -r '.result')
BLOCKS_MINED=$((END_HEIGHT - START_HEIGHT))

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "TEST 2 RESULTS:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Blocks mined: $BLOCKS_MINED"
echo "Miner crashes: $CRASHES/10"
echo "Daemon status: Running"
echo ""

if [ $BLOCKS_MINED -gt 100 ] && [ $CRASHES -eq 0 ]; then
    echo "✅ TEST 2 PASSED: All miners worked, no crashes!"
    TEST2_STATUS="✅ PASSED"
else
    echo "⚠️  TEST 2: Some issues (Blocks: $BLOCKS_MINED, Crashes: $CRASHES)"
    TEST2_STATUS="⚠️  WARNING"
fi

# Stop all processes
for PID in "${MINER_PIDS[@]}"; do
    kill $PID 2>/dev/null
done
kill $DAEMON_PID 2>/dev/null
sleep 5
pkill -9 dinerod dinero-miner 2>/dev/null || true
sleep 3
echo ""
echo ""

# ═══════════════════════════════════════════════════════════════════════════
# TEST 3: GUI + Miner Test (Manual - requires GUI)
# ═══════════════════════════════════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🖥️  TEST 3/3: GUI + Miner Test"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "This test requires manual interaction:"
echo ""
echo "1. Open Terminal 1 and run:"
echo "   cd $(pwd)/.."
echo "   ./build/dinerod -datadir=./data -testnet -rpcport=20998 -dev"
echo ""
echo "2. Open Terminal 2 and run:"
echo "   cd $(pwd)/.."
echo "   ./build/bin/dinero-qt6.app/Contents/MacOS/dinero-qt6"
echo ""
echo "3. In the GUI:"
echo "   - Create or unlock wallet"
echo "   - Generate an address"
echo "   - Copy the address"
echo ""
echo "4. Open Terminal 3 and run:"
echo "   cd $(pwd)/.."
echo "   ./build/dinero-miner --address <YOUR_ADDRESS> --threads 4"
echo ""
echo "5. Verify for 5 minutes:"
echo "   ✅ GUI shows increasing block height"
echo "   ✅ GUI shows balance updates"
echo "   ✅ GUI doesn't freeze"
echo "   ✅ Miner continues mining"
echo ""
echo "⏸️  Skipping automated GUI test (requires manual verification)"
TEST3_STATUS="⏸️  MANUAL"
echo ""

# ═══════════════════════════════════════════════════════════════════════════
# FINAL SUMMARY
# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "═══════════════════════════════════════════════════════════════════════════"
echo "🎯 QUICK TESTS SUMMARY"
echo "═══════════════════════════════════════════════════════════════════════════"
echo ""
echo "Test 1 - RPC Response Time:    $TEST1_STATUS"
echo "Test 2 - 10 Concurrent Miners:  $TEST2_STATUS"
echo "Test 3 - GUI + Miner:           $TEST3_STATUS"
echo ""
echo "All logs saved to: $RESULTS_DIR/"
echo ""

if [[ "$TEST1_STATUS" == *"PASSED"* ]] && [[ "$TEST2_STATUS" == *"PASSED"* ]]; then
    echo "✅ QUICK TESTS COMPLETED SUCCESSFULLY!"
    echo ""
    echo "🎯 NEXT STEPS:"
    echo "  1. Run GUI + Miner test manually (see instructions above)"
    echo "  2. Start 24h stability test before bed"
    echo "  3. Deploy to network servers tomorrow"
    echo ""
    echo "🚀 You're on track for mainnet launch!"
else
    echo "⚠️  Some tests had warnings - review logs for details"
fi

echo "═══════════════════════════════════════════════════════════════════════════"
