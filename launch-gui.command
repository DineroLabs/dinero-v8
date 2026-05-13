#!/bin/bash
# Dinero Complete Launcher - Auto-connects to testnet and launches GUI

echo "════════════════════════════════════════════════════════════"
echo "  🚀 DINERO WALLET LAUNCHER"
echo "════════════════════════════════════════════════════════════"
echo ""

cd "$(dirname "$0")"

# Step 1: Check if we have cookies for remote servers
COOKIE_DIR="$HOME/.dinero/testnet-cookies"
if [ ! -f "$COOKIE_DIR/server1.cookie" ] && [ ! -f "$COOKIE_DIR/server2.cookie" ]; then
    echo "⚠️  No testnet cookies found."
    echo ""
    echo "Choose how to connect:"
    echo "  1) Connect to remote testnet servers (recommended)"
    echo "  2) Start local daemon (requires blockchain sync)"
    echo ""
    read -p "Enter choice [1/2]: " choice
    
    if [ "$choice" = "1" ]; then
        echo ""
        echo "📡 Setting up connection to testnet servers..."
        echo ""
        
        # Setup SSH tunnels
        cd gui
        ./setup-tunnels.sh
        ./fetch-testnet-cookies.sh
        cd ..
        
        echo ""
        echo "✅ Ready to connect to testnet!"
        echo ""
    elif [ "$choice" = "2" ]; then
        echo ""
        echo "🚀 Starting local daemon..."
        echo ""
        
        # Check if daemon exists
        if [ ! -f "./build-clean/dinero-daemon" ]; then
            echo "❌ Daemon not found at ./build-clean/dinero-daemon"
            echo "Please build it first:"
            echo "  cmake --build build-clean --target dinero-daemon"
            exit 1
        fi
        
        # Start daemon in background
        ./build-clean/dinero-daemon -datadir="./data-main" -daemon
        
        echo "✅ Daemon started!"
        echo "⏳ Waiting 5 seconds for RPC server to be ready..."
        sleep 5
    else
        echo "Invalid choice. Exiting."
        exit 1
    fi
fi

# Step 2: Launch GUI
echo "🖥️  Launching Dinero Wallet GUI..."
echo ""

# Check if GUI exists
if [ ! -f "./gui/build/dinero-qt" ]; then
    echo "❌ GUI not found at ./gui/build/dinero-qt"
    echo "Please build it first:"
    echo "  cd gui/build && make"
    exit 1
fi

# Launch GUI
./gui/build/dinero-qt -datadir="$(pwd)/data-main" &

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  ✅ DINERO WALLET LAUNCHED"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "📌 WHAT'S RUNNING:"
echo "   • GUI: Wallet interface"
echo "   • RPC: Connected to daemon (local or remote)"
echo ""
echo "⛏️  TO START MINING:"
echo "   1. In GUI, go to 'Wallet' tab"
echo "   2. Unlock wallet and generate an address"
echo "   3. Go to 'Mining' tab"
echo "   4. Click 'Use Wallet Address'"
echo "   5. Click 'Start Mining'"
echo ""
echo "🔍 CONNECTION STATUS:"
echo "   • Check status bar in GUI (bottom)"
echo "   • Green = Connected to daemon"
echo "   • Orange/Red = Connection issue"
echo ""
echo "💡 TIP: Mining finds blocks on your Mac,"
echo "        but the daemon (local or remote) validates"
echo "        and broadcasts them to the network."
echo ""
echo "════════════════════════════════════════════════════════════"
