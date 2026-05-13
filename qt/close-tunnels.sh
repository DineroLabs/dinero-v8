#!/bin/bash
# Close SSH tunnels to testnet servers

echo "🛑 Closing SSH tunnels..."

pkill -f "ssh.*173.249.195.59.*20998"
pkill -f "ssh.*96.9.226.98.*20998"

echo "✅ Tunnels closed"
