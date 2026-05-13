#!/bin/bash
# Simplified autonomous test - mines 100+ blocks and monitors for deadlocks

LOG_DIR="test_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

echo "🧪 Starting Autonomous Deadlock Test"
echo "Log directory: $LOG_DIR"

# Clean start
rm -rf ./data/blocks ./data/utxo.db ./data/blockchain_state.json ./data/testnet/*.db*

# Start daemon (dev mode for simplicity)
./build/dinerod -datadir=./data -testnet -rpcport=20998 -port=20999 -dev > "$LOG_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "Daemon PID: $DAEMON_PID"
sleep 10

# Create wallet
echo "Creating wallet..."
curl -s http://127.0.0.1:20998/ -d '{"method":"createhdwallet","params":["test"]}' > /dev/null

# Get mining address
ADDR=$(curl -s http://127.0.0.1:20998/ -d '{"method":"getnewaddress"}' | jq -r '.result.address // .result')
echo "Mining address: $ADDR"

# Start miner
./build/dinero-miner -a "$ADDR" -t 4 > "$LOG_DIR/miner.log" 2>&1 &
MINER_PID=$!
echo "Miner PID: $MINER_PID"
echo ""
echo "⛏️  Mining 105 blocks..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Monitor
START=$(date +%s)
LAST_HEIGHT=0
RPC_FAILURES=0

while true; do
    HEIGHT=$(curl -s --max-time 5 http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' 2>/dev/null | jq -r '.result // 0')
    
    if [ -z "$HEIGHT" ] || [ "$HEIGHT" = "null" ]; then
        RPC_FAILURES=$((RPC_FAILURES + 1))
        if [ $RPC_FAILURES -ge 10 ]; then
            echo "❌ FAILED: RPC deadlock detected!"
            kill $DAEMON_PID $MINER_PID 2>/dev/null
            exit 1
        fi
        sleep 5
        continue
    fi
    
    RPC_FAILURES=0
    
    if [ "$HEIGHT" != "$LAST_HEIGHT" ]; then
        ELAPSED=$(($(date +%s) - START))
        echo "[$(date +%H:%M:%S)] Block $HEIGHT | Elapsed: ${ELAPSED}s"
        LAST_HEIGHT=$HEIGHT
        
        # Test RPC every 10 blocks
        if [ $((HEIGHT % 10)) -eq 0 ] && [ $HEIGHT -gt 0 ]; then
            RPC_START=$(date +%s%3N)
            curl -s http://127.0.0.1:20998/ -d '{"method":"getblockchaininfo"}' > /dev/null
            RPC_TIME=$(($(date +%s%3N) - RPC_START))
            echo "   ⚡ RPC time: ${RPC_TIME}ms"
        fi
    fi
    
    if [ "$HEIGHT" -ge 105 ]; then
        ELAPSED=$(($(date +%s) - START))
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "✅ TEST PASSED! $HEIGHT blocks mined in ${ELAPSED}s"
        echo "   Average: $((ELAPSED / HEIGHT))s per block"
        echo "   No deadlocks detected!"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        kill $MINER_PID $DAEMON_PID 2>/dev/null
        exit 0
    fi
    
    if ! kill -0 $DAEMON_PID 2>/dev/null || ! kill -0 $MINER_PID 2>/dev/null; then
        echo "❌ FAILED: Process crashed"
        exit 1
    fi
    
    sleep 3
done
