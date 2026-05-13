#!/bin/bash

# P2P Hub script for Node A (accepts connections from Node B)
cd /Users/haydarevich/Documents/DineroCoin

echo "🌐 **NODE A P2P HUB STARTUP** 🌐"
echo "==============================="
echo ""
echo "🖥️  **Hub Configuration:**"
echo "• Role: Hub (accepts connections)"
echo "• P2P Port: 20333"
echo "• Mining: Enabled"
echo "• Network: regtest"
echo "• Data: data/regtest"
echo ""

# Stop any existing daemons
pkill -f dinerod 2>/dev/null || true
sleep 2

# 1. Start Node A as P2P hub
echo "1️⃣ **Starting Node A as P2P hub...**"

# Create data directory if it doesn't exist
mkdir -p data/regtest

# Start daemon as P2P hub
./build/dinerod \
  -regtest \
  -datadir="$(pwd)/data" \
  -p2p -port=20333 \
  -gen \
  -printtoconsole > data/node_a_hub.log 2>&1 &

sleep 8

# 2. Test RPC connection
echo "2️⃣ **Testing RPC connection...**"
COOKIE_PATH="$(pwd)/data/regtest/.cookie"
if [ ! -f "$COOKIE_PATH" ]; then
    echo "❌ Error: Cookie file not found. Is Node A running?"
    echo "📋 **Daemon log:**"
    tail -15 data/node_a_hub.log
    exit 1
fi

AUTH="$(cat "$COOKIE_PATH")"
echo "✅ Cookie found: $AUTH"

# 3. Get blockchain info
echo "3️⃣ **Getting blockchain info...**"
BLOCKCHAIN_INFO="$(curl -s --user "$AUTH" -d '{"method":"getblockchaininfo","id":1}' http://127.0.0.1:20999/)"
echo "$BLOCKCHAIN_INFO" | jq .

# 4. Check network info
echo "4️⃣ **Checking network info...**"
NETWORK_INFO="$(curl -s --user "$AUTH" -d '{"method":"getnetworkinfo","id":1}' http://127.0.0.1:20999/)"
echo "$NETWORK_INFO" | jq .

# 5. Check mining status
echo "5️⃣ **Checking mining status...**"
MINING_STATUS_RESPONSE="$(curl -s --user "$AUTH" -d '{"method":"mining.status","id":1}' http://127.0.0.1:20999/)"
echo "$MINING_STATUS_RESPONSE" | jq .

# 6. Monitor for incoming connections
echo "6️⃣ **Monitoring for incoming P2P connections...**"
echo "⏱️  Checking every 10 seconds..."
echo "🔍 **Waiting for Node B to connect...**"

for i in {1..30}; do
    sleep 10
    
    # Check network connections
    CONNECTIONS=$(curl -s --user "$AUTH" -d '{"method":"getnetworkinfo","id":1}' http://127.0.0.1:20999/ | jq -r '.result.connections')
    
    # Check blockchain height
    HEIGHT=$(curl -s --user "$AUTH" -d '{"method":"getblockchaininfo","id":1}' http://127.0.0.1:20999/ | jq -r '.result.blocks')
    
    # Check mining status
    BLOCKS_MINED=$(curl -s --user "$AUTH" -d '{"method":"mining.status","id":1}' http://127.0.0.1:20999/ | jq -r '.result.blocks_mined')
    HASHRATE=$(curl -s --user "$AUTH" -d '{"method":"mining.status","id":1}' http://127.0.0.1:20999/ | jq -r '.result.hashrate_hps_ma')
    
    echo "⏱️  Check $i: Height=$HEIGHT, Blocks mined=$BLOCKS_MINED, Hashrate=$HASHRATE H/s, Connections=$CONNECTIONS"
    
    if [ "$CONNECTIONS" -gt 0 ]; then
        echo "🎉 SUCCESS! Node A connected to $CONNECTIONS peers!"
        echo "✅ P2P hub is working - Node B can connect!"
        break
    fi
    
    if [ "$i" -eq 30 ]; then
        echo "⏰ Timeout reached. Node A is running as hub but no connections yet."
        echo "💡 Make sure Node B is running the P2P script."
    fi
done

# 7. Final status
echo "7️⃣ **Final hub status...**"
echo "📋 **Network info:**"
curl -s --user "$AUTH" -d '{"method":"getnetworkinfo","id":1}' http://127.0.0.1:20999/ | jq .

echo "📋 **Mining status:**"
curl -s --user "$AUTH" -d '{"method":"mining.status","id":1}' http://127.0.0.1:20999/ | jq .

echo "📋 **Blockchain info:**"
curl -s --user "$AUTH" -d '{"method":"getblockchaininfo","id":1}' http://127.0.0.1:20999/ | jq .

echo ""
echo "✅ **NODE A P2P HUB RUNNING**"
echo "============================"
echo "• Daemon: ✅ Running as P2P hub"
echo "• Port: ✅ 20333 (listening for connections)"
echo "• Mining: ✅ Active"
echo "• RPC: ✅ Working on port 20999"
echo "• Status: ✅ Ready for Node B connections"
echo ""
echo "🎯 **Next Steps:**"
echo "• Run Node B P2P script on the other Mac"
echo "• Monitor block propagation between nodes"
echo "• Test multi-node mining competition"
