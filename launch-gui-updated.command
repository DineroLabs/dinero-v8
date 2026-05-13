#!/bin/bash

# Set working directory
cd "$(dirname "$0")"

echo "════════════════════════════════════════════════════════════"
echo "         🚀 DINERO WALLET LAUNCHER (Updated)"
echo "════════════════════════════════════════════════════════════"
echo ""

# Set miner path for GUI
export DINERO_MINER_PATH="$(pwd)/build/dinero-miner"

if [ ! -f "$DINERO_MINER_PATH" ]; then
    echo "⚠️  Miner binary not found at: $DINERO_MINER_PATH"
    echo "   Building miner now..."
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    cmake --build . --target dinero-miner -j$(sysctl -n hw.ncpu) > /dev/null 2>&1
    cd ..
    if [ -f "$DINERO_MINER_PATH" ]; then
        echo "✅ Miner built successfully"
    else
        echo "❌ Failed to build miner"
    fi
    echo ""
fi

echo "🖥️  Launching Dinero Wallet GUI..."
echo "⛏️  Miner: $DINERO_MINER_PATH"
echo ""

# Launch GUI
./gui/build/dinero-qt &

echo "════════════════════════════════════════════════════════════"
echo "                  ✅ DINERO WALLET LAUNCHED"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "📌 NEW FEATURES:"
echo "   • 🔐 Encrypt Wallet button (top toolbar)"
echo "   • ⛏️  Mining fully integrated"
echo ""
echo "💡 TO ENCRYPT YOUR WALLET:"
echo "   1. Click 🔐 'Encrypt Wallet' button (top toolbar)"
echo "   2. Enter a strong password"
echo "   3. Wallet will lock - use 🔓 'Unlock' to spend"
echo ""
echo "⛏️  TO START MINING:"
echo "   1. Unlock wallet (if encrypted)"
echo "   2. Go to 'Mining' tab"
echo "   3. Click 'Use Wallet Address'"
echo "   4. Click 'Start Mining'"
echo ""
echo "════════════════════════════════════════════════════════════"

