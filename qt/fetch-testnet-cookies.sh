#!/bin/bash

# Fetch testnet cookies from remote servers
# This script downloads the .cookie files needed for RPC authentication

COOKIE_DIR="$HOME/.dinero/testnet-cookies"
mkdir -p "$COOKIE_DIR"

echo "🔐 Fetching testnet cookies from remote servers..."
echo ""

# Server 1: 173.249.195.59
echo "📡 Server 1 (173.249.195.59)..."
if ssh root@173.249.195.59 "cat /root/.dinero-testnet/.cookie" 2>/dev/null > "$COOKIE_DIR/server1.cookie"; then
    echo "   ✅ Downloaded successfully"
else
    echo "   ❌ Failed - trying alternative path..."
    if ssh root@173.249.195.59 "cat /root/data-testnet/.cookie" 2>/dev/null > "$COOKIE_DIR/server1.cookie"; then
        echo "   ✅ Downloaded successfully"
    else
        echo "   ⚠️  Could not fetch cookie - you may need to copy it manually"
    fi
fi

# Server 2: 96.9.226.98
echo "📡 Server 2 (96.9.226.98)..."
if ssh root@96.9.226.98 "cat /root/.dinero-testnet/.cookie" 2>/dev/null > "$COOKIE_DIR/server2.cookie"; then
    echo "   ✅ Downloaded successfully"
else
    echo "   ❌ Failed - trying alternative path..."
    if ssh root@96.9.226.98 "cat /root/data-testnet/.cookie" 2>/dev/null > "$COOKIE_DIR/server2.cookie"; then
        echo "   ✅ Downloaded successfully"
    else
        echo "   ⚠️  Could not fetch cookie - you may need to copy it manually"
    fi
fi

echo ""
echo "📁 Cookies saved to: $COOKIE_DIR"
echo ""

# Display contents (first few characters only)
if [ -f "$COOKIE_DIR/server1.cookie" ]; then
    echo "Server 1 cookie: $(head -c 20 "$COOKIE_DIR/server1.cookie")..."
fi
if [ -f "$COOKIE_DIR/server2.cookie" ]; then
    echo "Server 2 cookie: $(head -c 20 "$COOKIE_DIR/server2.cookie")..."
fi

echo ""
echo "✅ Done! The GUI will now be able to connect to testnet servers."
echo ""
echo "📌 Manual method (if SSH fails):"
echo "   On each server, run: cat ~/.dinero-testnet/.cookie"
echo "   Then copy the contents to: $COOKIE_DIR/server1.cookie and server2.cookie"
