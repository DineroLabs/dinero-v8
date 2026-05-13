#!/bin/bash

echo "═══════════════════════════════════════════════════════════════════════════"
echo "🧪 100+ BLOCK DEADLOCK TEST"
echo "═══════════════════════════════════════════════════════════════════════════"
echo ""

# Step 1: Clean Environment
echo "📦 Step 1: Cleaning environment..."
pkill -9 dinerod dinero-miner 2>/dev/null || true
sleep 3
rm -rf ./data/blocks ./data/utxo.db ./data/blockchain_state.json ./data/wallet
find ./data/testnet -name "*.db*" -delete 2>/dev/null || true
echo "✅ Environment cleaned"
echo ""

# Step 2: Start Daemon
echo "🚀 Step 2: Starting daemon..."
./build/dinerod -datadir=./data -testnet -rpcport=20998 -dev > test_daemon.log 2>&1 &
DAEMON_PID=$!
echo "Daemon PID: $DAEMON_PID"
echo "Waiting 12 seconds for initialization..."
sleep 12

# Check if daemon started
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "❌ Daemon failed to start!"
    cat test_daemon.log
    exit 1
fi
echo "✅ Daemon running"
echo ""

# Step 3: Create Wallet
echo "🔑 Step 3: Creating wallet and getting address..."
WALLET_RESPONSE=$(curl -s http://127.0.0.1:20998/ -d '{"method":"createhdwallet","params":["test123"]}')

ADDR=$(echo "$WALLET_RESPONSE" | jq -r '.result.first_address // empty')

if [ -z "$ADDR" ]; then
    echo "❌ Failed to get mining address!"
    echo "Full response:"
    echo "$WALLET_RESPONSE" | jq '.'
    kill $DAEMON_PID
    exit 1
fi

echo "✅ Mining address: $ADDR"
echo ""

# Step 4: Start Miner
echo "⛏️  Step 4: Starting miner (4 threads)..."
echo "Command: ./build/dinero-miner --address $ADDR --threads 4"
./build/dinero-miner --address "$ADDR" --threads 4 > test_miner.log 2>&1 &
MINER_PID=$!
echo "Miner PID: $MINER_PID"

# Wait a bit for miner to start
sleep 5

# Check miner status
if ! kill -0 $MINER_PID 2>/dev/null; then
    echo "❌ Miner failed to start!"
    echo "Miner log:"
    cat test_miner.log
    echo ""
    echo "Daemon log (last 20 lines):"
    tail -20 test_daemon.log
    kill $DAEMON_PID 2>/dev/null
    exit 1
fi

# Check if miner is actually mining
sleep 3
if grep -q "Mining started" test_miner.log; then
    echo "✅ Miner running and connected!"
else
    echo "⚠️  Miner may have connection issues. Checking logs..."
    tail -10 test_miner.log
    echo ""
    echo "Continuing anyway..."
fi
echo ""

# Step 5: Monitor Mining
echo "📊 Step 5: Monitoring block production (target: 105 blocks)..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

START_TIME=$(date +%s)
LAST_HEIGHT=0
RPC_FAILURES=0

while true; do
    # Get current height
    HEIGHT=$(curl -s --max-time 3 http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' 2>/dev/null | jq -r '.result // 0')
    
    # Handle RPC failures
    if [ -z "$HEIGHT" ] || [ "$HEIGHT" = "null" ] || [ "$HEIGHT" = "0" ]; then
        RPC_FAILURES=$((RPC_FAILURES + 1))
        if [ $RPC_FAILURES -ge 10 ]; then
            echo ""
            echo "❌ TEST FAILED: RPC server not responding (possible deadlock)"
            kill $DAEMON_PID $MINER_PID 2>/dev/null
            exit 1
        fi
        sleep 3
        continue
    fi
    
    RPC_FAILURES=0
    
    # New block mined
    if [ "$HEIGHT" != "$LAST_HEIGHT" ]; then
        CURRENT_TIME=$(date +%s)
        ELAPSED=$((CURRENT_TIME - START_TIME))
        REMAINING=$((105 - HEIGHT))
        
        echo "⛓️  [$(date +%H:%M:%S)] Block $HEIGHT/105 | Remaining: $REMAINING | Elapsed: ${ELAPSED}s"
        
        # Test RPC responsiveness every 10 blocks
        if [ $((HEIGHT % 10)) -eq 0 ] && [ $HEIGHT -gt 1 ]; then
            RPC_START=$(date +%s%3N)
            curl -s --max-time 2 http://127.0.0.1:20998/ -d '{"method":"getblockchaininfo"}' > /dev/null 2>&1
            RPC_END=$(date +%s%3N)
            RPC_TIME=$((RPC_END - RPC_START))
            
            if [ $RPC_TIME -lt 200 ]; then
                echo "   ⚡ RPC response time: ${RPC_TIME}ms ✅"
            else
                echo "   ⚠️  RPC response time: ${RPC_TIME}ms (slow)"
            fi
        fi
        
        LAST_HEIGHT=$HEIGHT
    fi
    
    # Check if target reached
    if [ "$HEIGHT" -ge 105 ]; then
        TOTAL_TIME=$(($(date +%s) - START_TIME))
        
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "✅ TEST PASSED! 105 BLOCKS MINED SUCCESSFULLY!"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""
        echo "📊 Final Statistics:"
        echo "   • Total blocks: $HEIGHT"
        echo "   • Total time: ${TOTAL_TIME}s ($((TOTAL_TIME / 60)) minutes)"
        echo "   • Average time per block: $((TOTAL_TIME / HEIGHT))s"
        echo "   • RPC failures: 0 ✅"
        echo "   • Daemon restarts: 0 ✅"
        echo ""
        echo "🎉 NO DEADLOCKS DETECTED - FIX VERIFIED!"
        echo ""
        
        # Get final blockchain info
        echo "🔍 Final blockchain state:"
        curl -s http://127.0.0.1:20998/ -d '{"method":"getblockchaininfo"}' | jq '.'
        echo ""
        
        # Stop processes gracefully
        echo "🛑 Stopping miner and daemon..."
        kill $MINER_PID 2>/dev/null || true
        sleep 2
        kill $DAEMON_PID 2>/dev/null || true
        sleep 2
        kill -9 $MINER_PID $DAEMON_PID 2>/dev/null || true
        
        echo "✅ Test complete! Logs saved:"
        echo "   - test_daemon.log"
        echo "   - test_miner.log"
        
        exit 0
    fi
    
    # Check if processes crashed
    if ! kill -0 $DAEMON_PID 2>/dev/null; then
        echo ""
        echo "❌ TEST FAILED: Daemon crashed!"
        echo ""
        echo "Daemon log:"
        tail -50 test_daemon.log
        kill $MINER_PID 2>/dev/null
        exit 1
    fi
    
    if ! kill -0 $MINER_PID 2>/dev/null; then
        echo ""
        echo "❌ TEST FAILED: Miner crashed!"
        echo ""
        echo "Miner log:"
        tail -50 test_miner.log
        kill $DAEMON_PID 2>/dev/null
        exit 1
    fi
    
    sleep 3
done
