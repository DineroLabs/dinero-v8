#!/bin/bash
# DineroCoin Mainnet Bundle Notarization Command
# Execute this when mainnet daemon is running with funds

BUNDLE_HASH="7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c"
NOTARIZATION_ADDRESS="din1q3a90d70ffae6a40e2e10b4b4a09e568e5f7c1c91"
DATA_DIR="${1:-/root/.dinero}"  # Default to mainnet data dir
RPC_PORT="${2:-20998}"

echo "🔐 DineroCoin Mainnet Bundle Notarization"
echo "=========================================="
echo ""
echo "Bundle Hash: $BUNDLE_HASH"
echo "Address: $NOTARIZATION_ADDRESS"
echo "Data Dir: $DATA_DIR"
echo "RPC Port: $RPC_PORT"
echo ""

# Check daemon
if ! ./build/dinero-cli -datadir="$DATA_DIR" -rpcport=$RPC_PORT getblockcount &>/dev/null; then
    echo "❌ Daemon not responding"
    echo "   Start daemon: ./build/dinerod --datadir=$DATA_DIR --rpcport=$RPC_PORT"
    exit 1
fi

# Check balance
BALANCE=$(./build/dinero-cli -datadir="$DATA_DIR" -rpcport=$RPC_PORT getbalance 2>/dev/null | python3 -c "import sys, json; print(json.load(sys.stdin).get('total', 0))" 2>/dev/null || echo "0")
echo "Balance: $BALANCE DIN"

if [ "$(echo "$BALANCE < 0.00000001" | bc 2>/dev/null || echo 1)" -eq 1 ]; then
    echo "⚠️  Insufficient funds (need 0.00000001 DIN minimum)"
    exit 1
fi

# Send notarization
echo ""
echo "📝 Sending notarization transaction..."
TXID=$(./build/dinero-cli -datadir="$DATA_DIR" -rpcport=$RPC_PORT sendtoaddress \
    "$NOTARIZATION_ADDRESS" 0.00000001 "" \
    "Mainnet Launch Kit Notarization: $BUNDLE_HASH" 2>&1)

if [ $? -eq 0 ] && [ ${#TXID} -eq 64 ]; then
    echo "✅ NOTARIZATION SUCCESSFUL!"
    echo ""
    echo "Transaction ID: $TXID"
    echo "Bundle Hash: $BUNDLE_HASH"
    echo ""
    echo "✅ Bundle hash is now permanently recorded on-chain"
    echo ""
    echo "🔍 Verify transaction:"
    echo "   ./build/dinero-cli -datadir=$DATA_DIR -rpcport=$RPC_PORT gettransaction \"$TXID\""
else
    echo "❌ Failed to send transaction"
    echo "Error: $TXID"
    echo ""
    echo "Common issues:"
    echo "  - Wallet not unlocked (use: walletpassphrase)"
    echo "  - Insufficient funds"
    echo "  - Invalid address"
    exit 1
fi
