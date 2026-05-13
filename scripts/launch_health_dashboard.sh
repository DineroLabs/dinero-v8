#!/bin/bash
# Launch Dinero Health Dashboard

echo "🚀 Launching Dinero Health Dashboard..."
echo ""

# Check if daemon is running
if ! curl -s --user "$(tr -d '\r\n' < /Users/haydarevich/Documents/DineroCoin/data/.cookie)" "http://127.0.0.1:20998" > /dev/null 2>&1; then
    echo "❌ Dinero daemon is not running!"
    echo "Please start the daemon first:"
    echo "  ./build/bin/dinerod -datadir=/Users/haydarevich/Documents/DineroCoin/data -rpcport=20998 -port=20999"
    echo ""
    exit 1
fi

echo "✅ Dinero daemon is running"
echo "🎯 Starting Health Dashboard..."
echo ""

# Launch the dashboard
exec ./build/bin/dinero-health-dashboard
