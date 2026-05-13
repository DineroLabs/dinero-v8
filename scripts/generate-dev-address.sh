#!/bin/bash
# Generate a secure developer fund address for Dinero mainnet

set -e

echo "🔐 Generating Dinero Developer Fund Address..."

# Create secure temporary directory
SECURE_DIR="/tmp/dinero-dev-fund-$(date +%s)"
mkdir -p "$SECURE_DIR"
chmod 700 "$SECURE_DIR"

echo "📁 Secure directory: $SECURE_DIR"

# Start daemon with secure wallet
echo "🚀 Starting secure wallet daemon..."
./build/bin/dinerod -datadir="$SECURE_DIR" -rpcport=25998 -port=25999 -daemon

# Wait for startup
sleep 5

# Get RPC credentials
AUTH=$(tr -d '\r\n' < "$SECURE_DIR/.cookie")
RPC_URL="http://127.0.0.1:25998"

# Helper function for RPC calls
rpc_call() {
    curl -s --user "$AUTH" -H 'content-type: application/json' \
         --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" \
         "$RPC_URL" | jq -r '.result'
}

echo "💰 Generating developer fund address..."

# Generate new address
DEV_ADDRESS=$(rpc_call "getnewaddress" '["developer_fund"]')
echo "✅ Developer fund address: $DEV_ADDRESS"

# Get private key (CRITICAL: Store securely!)
PRIVATE_KEY=$(rpc_call "dumpprivkey" "[\"$DEV_ADDRESS\"]")

# Get address info for hash160
ADDRESS_INFO=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
               --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"validateaddress\",\"params\":[\"$DEV_ADDRESS\"]}" \
               "$RPC_URL" | jq '.result')

echo ""
echo "🔑 CRITICAL SECURITY INFORMATION:"
echo "=================================="
echo ""
echo "Developer Fund Address: $DEV_ADDRESS"
echo ""
echo "Private Key (STORE OFFLINE!):"
echo "$PRIVATE_KEY"
echo ""
echo "Address Details:"
echo "$ADDRESS_INFO" | jq '.'
echo ""

# Extract hash160 for chain_facts.hpp
echo "📝 Update src/consensus/chain_facts.hpp with this address:"
echo ""
echo "// Replace DEV_FUND_P2WPKH with your real hash160"
echo "// Address: $DEV_ADDRESS"
echo "static constexpr uint8_t DEV_FUND_P2WPKH[22] = {"
echo "    0x00, 0x14,"
echo "    // TODO: Extract 20-byte hash160 from validateaddress output"
echo "    // and format as hex bytes here"
echo "};"
echo ""

# Cleanup daemon
echo "🧹 Cleaning up daemon..."
pkill -f "dinerod.*25998" || true
sleep 2

echo "⚠️  SECURITY REMINDERS:"
echo "1. Store the private key on an air-gapped machine"
echo "2. Create encrypted backups of the private key"
echo "3. Never commit the private key to version control"
echo "4. Test on testnet before mainnet deployment"
echo "5. Use hardware wallet for additional security"
echo ""
echo "📁 Secure files are in: $SECURE_DIR"
echo "🗑️  Delete this directory after backing up keys securely!"
echo ""
echo "rm -rf $SECURE_DIR"
