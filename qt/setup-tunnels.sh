#!/bin/bash
# Setup SSH tunnels to access remote testnet RPC servers
# This creates local ports that tunnel to the remote servers

echo "🔐 Setting up SSH tunnels to testnet servers..."
echo ""

# Kill existing tunnels if any
pkill -f "ssh.*173.249.195.59.*20998" 2>/dev/null
pkill -f "ssh.*96.9.226.98.*20998" 2>/dev/null

# Server 1: Create tunnel from local 20991 -> remote 20998
echo "📡 Tunneling to Server 1 (173.249.195.59)..."
ssh -f -N -L 20991:127.0.0.1:20998 root@173.249.195.59
if [ $? -eq 0 ]; then
    echo "   ✅ Tunnel active: localhost:20991 -> 173.249.195.59:20998"
else
    echo "   ❌ Failed to create tunnel"
fi

# Server 2: Create tunnel from local 20992 -> remote 20998
echo "📡 Tunneling to Server 2 (96.9.226.98)..."
ssh -f -N -L 20992:127.0.0.1:20998 root@96.9.226.98
if [ $? -eq 0 ]; then
    echo "   ✅ Tunnel active: localhost:20992 -> 96.9.226.98:20998"
else
    echo "   ❌ Failed to create tunnel"
fi

echo ""
echo "✅ SSH Tunnels established!"
echo ""
echo "📌 Local ports mapped:"
echo "   • localhost:20991 → Server 1 (173.249.195.59)"
echo "   • localhost:20992 → Server 2 (96.9.226.98)"
echo ""
echo "🔍 To check tunnels: ps aux | grep ssh"
echo "🛑 To close tunnels: ./close-tunnels.sh"
echo ""
echo "🚀 Now fetch cookies and launch GUI:"
echo "   ./fetch-testnet-cookies.sh"
echo "   cd .. && open launch-gui.command"
