#!/bin/bash
# Dinero Production Node Health Check
# Auto-restarts daemon if peer count drops below 1

DINERO_CLI="/root/DineroCoin/build/dinero-cli"
DATA_DIR="/root/.dinero"
LOG_FILE="/var/log/dinero-health.log"

# Ensure log file exists with proper permissions
touch "$LOG_FILE" 2>/dev/null || LOG_FILE="/tmp/dinero-health.log"

# Get metrics
BLOCKS=$($DINERO_CLI -datadir=$DATA_DIR getblockcount 2>/dev/null || echo "ERROR")
PEERS=$($DINERO_CLI -datadir=$DATA_DIR getconnectioncount 2>/dev/null || echo "0")

# Health check logic
if [ "$PEERS" = "ERROR" ] || [ "$PEERS" -lt 1 ]; then
    echo "$(date '+%Y-%m-%d %H:%M:%S'): ⚠️  No peers detected (peers=$PEERS) — restarting dinerod" >> "$LOG_FILE"
    systemctl restart dinerod
    sleep 5
    # Verify restart
    NEW_PEERS=$($DINERO_CLI -datadir=$DATA_DIR getconnectioncount 2>/dev/null || echo "0")
    echo "$(date '+%Y-%m-%d %H:%M:%S'): ✅ Restarted (new peers=$NEW_PEERS)" >> "$LOG_FILE"
else
    echo "$(date '+%Y-%m-%d %H:%M:%S'): ✅ ok peers=$PEERS blocks=$BLOCKS" >> "$LOG_FILE"
fi
