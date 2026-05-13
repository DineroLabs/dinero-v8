#!/bin/bash
LOG_DIR="test_$(date +%H%M%S)"
mkdir -p "$LOG_DIR"

echo "🧪 Autonomous Deadlock Test - Mining 105 Blocks"
rm -rf ./data/blocks ./data/utxo.db ./data/blockchain_state.json ./data/testnet/*.db* ./data/wallet

# Start daemon
./build/dinerod -datadir=./data -testnet -rpcport=20998 -dev > "$LOG_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 10

# Get address from wallet creation
WALLET_DATA=$(curl -s http://127.0.0.1:20998/ -d '{"method":"createhdwallet","params":["test"]}')
ADDR=$(echo "$WALLET_DATA" | jq -r '.result.first_address')
echo "Mining to: $ADDR"

# Start miner
./build/dinero-miner -a "$ADDR" -t 4 > "$LOG_DIR/miner.log" 2>&1 &
MINER_PID=$!

echo "⛏️  Mining started... (this will take a while)"
START=$(date +%s)
LAST=0

while true; do
    H=$(curl -s --max-time 3 http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' | jq -r '.result // 0')
    
    if [ "$H" != "$LAST" ] && [ "$H" != "0" ]; then
        echo "[Block $H] Elapsed: $(($(date +%s) - START))s"
        LAST=$H
    fi
    
    if [ "$H" -ge 105 ]; then
        echo "✅ SUCCESS! 105+ blocks mined - NO DEADLOCKS!"
        kill $MINER_PID $DAEMON_PID 2>/dev/null
        exit 0
    fi
    
    sleep 3
done
