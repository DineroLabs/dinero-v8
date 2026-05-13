#!/bin/bash
# Test RPC connections to testnet servers

echo "🧪 Testing connections to testnet servers..."
echo ""

# Test Server 1
echo "📡 Testing Server 1 (173.249.195.59)..."
if curl -s --max-time 5 http://173.249.195.59:20998 >/dev/null 2>&1; then
    echo "   ✅ Port 20998 is reachable"
else
    echo "   ❌ Cannot connect to port 20998"
fi

# Test Server 2  
echo "📡 Testing Server 2 (96.9.226.98)..."
if curl -s --max-time 5 http://96.9.226.98:20998 >/dev/null 2>&1; then
    echo "   ✅ Port 20998 is reachable"
else
    echo "   ❌ Cannot connect to port 20998"
fi

echo ""
echo "🔐 Checking for cookies..."
COOKIE_DIR="$HOME/.dinero/testnet-cookies"
if [ -f "$COOKIE_DIR/server1.cookie" ]; then
    echo "   ✅ Server 1 cookie found"
else
    echo "   ❌ Server 1 cookie missing"
fi

if [ -f "$COOKIE_DIR/server2.cookie" ]; then
    echo "   ✅ Server 2 cookie found"
else
    echo "   ❌ Server 2 cookie missing"
fi

echo ""
if [ ! -f "$COOKIE_DIR/server1.cookie" ] || [ ! -f "$COOKIE_DIR/server2.cookie" ]; then
    echo "💡 Run ./fetch-testnet-cookies.sh to download cookies"
fi
