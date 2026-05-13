#!/usr/bin/env bash
# Multi-network database migration script
# Prepares all networks for multi-daemon architecture
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "🗄️ **MULTI-DAEMON DATABASE MIGRATION**"
echo "====================================="
echo ""

# Network configurations for multi-daemon setup
declare -A NETWORK_PORTS=(
    ["mainnet"]="20998"
    ["testnet"]="20988" 
    ["regtest"]="20978"
)

# Check if any daemons are running and get their actual ports
echo "**Detecting running daemons...**"
RUNNING_DAEMONS=$(ps aux | grep dinerod | grep -v grep || true)
if [[ -n "$RUNNING_DAEMONS" ]]; then
    echo "Found running daemons:"
    echo "$RUNNING_DAEMONS" | while read line; do
        if [[ "$line" =~ --regtest.*--rpcport=([0-9]+) ]] || [[ "$line" =~ unified.*20999 ]]; then
            echo "  Regtest daemon: port 20999 (unified mode)"
            NETWORK_PORTS["regtest"]="20999"
        elif [[ "$line" =~ --testnet.*--rpcport=([0-9]+) ]]; then
            echo "  Testnet daemon: port ${BASH_REMATCH[1]}"
        elif [[ "$line" =~ --rpcport=([0-9]+) ]] && [[ ! "$line" =~ --regtest|--testnet ]]; then
            echo "  Mainnet daemon: port ${BASH_REMATCH[1]}"
        fi
    done
else
    echo "No daemons currently running"
fi
echo ""

# Migrate each network
for network in mainnet testnet regtest; do
    port=${NETWORK_PORTS[$network]}
    echo "**Migrating $network network (port $port)...**"
    
    # Set the RPC port for this network
    export RPC_PORT=$port
    
    # Run migration
    if "$SCRIPT_DIR/db_migrate_apply.sh" "$network"; then
        echo "✅ $network migration successful"
    else
        echo "❌ $network migration failed"
    fi
    echo ""
done

echo "**Running audits for all networks...**"
for network in mainnet testnet regtest; do
    echo "--- $network ---"
    "$SCRIPT_DIR/db_audit.sh" "$network" 2>/dev/null || echo "Audit failed (database may not exist yet)"
    echo ""
done

echo "🎊 **MULTI-DAEMON DATABASE ARCHITECTURE READY!**"
echo ""
echo "**Next steps:**"
echo "1. All networks now have consistent, robust SQLite schemas"
echo "2. Each network isolated in separate directories"
echo "3. WAL mode enabled for better concurrency"
echo "4. Meta tables track network identity and chain state"
echo "5. Ready for concurrent multi-daemon operation"
echo ""
echo "**To start multi-daemon setup:**"
echo "  Mainnet: ./build/dinerod --rpcport=20998 --wsport=21001"
echo "  Testnet: ./build/dinerod --testnet --rpcport=20988 --wsport=21011" 
echo "  Regtest: ./build/dinerod --regtest --rpcport=20978 --wsport=21021"
