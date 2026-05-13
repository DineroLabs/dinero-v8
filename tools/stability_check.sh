#!/bin/bash
# Daily Stability Monitoring Script for 7-Day Test
# Usage: ./tools/stability_check.sh [--alert-only]
#
# Records daily metrics and alerts on issues
# Results saved to: ~/stability_test_log.txt

set -euo pipefail

# Configuration
LOG_FILE="$HOME/stability_test_log.txt"
ALERT_ONLY=false

if [[ "${1:-}" == "--alert-only" ]]; then
    ALERT_ONLY=true
fi

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Get current timestamp
NOW=$(date '+%Y-%m-%d %H:%M:%S')
DAY_NUM=$(date '+%s')

# Helper function to print section headers
print_header() {
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "\n${BLUE}${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${BLUE}${BOLD}  $1${NC}"
        echo -e "${BLUE}${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
    fi
}

# Check if service exists
SERVICE_EXISTS=$(systemctl list-unit-files 2>/dev/null | grep -c "dinerod.service" || true)
if [[ "$SERVICE_EXISTS" -eq 0 ]]; then
    echo -e "${RED}❌ CRITICAL: dinerod.service not found!${NC}"
    exit 1
fi

# Initialize log file if it doesn't exist
if [[ ! -f "$LOG_FILE" ]]; then
    cat > "$LOG_FILE" << EOF
# DineroCoin 7-Day Stability Test Log
# Test Started: $(date '+%Y-%m-%d %H:%M:%S')
# Target: 7 days continuous operation with zero restarts
#
# Format: TIMESTAMP | STATUS | UPTIME | RESTARTS | MEMORY_MB | RPC_OK
# ========================================================================
EOF
fi

# Start collecting metrics
ISSUES=()

# ═══════════════════════════════════════════════════════════════════════
# 1. Service Status Check
# ═══════════════════════════════════════════════════════════════════════
print_header "1. Service Status"

ACTIVE_STATE=$(systemctl show dinerod --property=ActiveState --value)
SUB_STATE=$(systemctl show dinerod --property=SubState --value)

if [[ "$ACTIVE_STATE" != "active" ]] || [[ "$SUB_STATE" != "running" ]]; then
    ISSUES+=("❌ CRITICAL: Service not running! State: $ACTIVE_STATE/$SUB_STATE")
    echo -e "${RED}❌ CRITICAL: Service not running!${NC}"
    echo -e "   State: ${ACTIVE_STATE}/${SUB_STATE}"
    STATUS="DOWN"
else
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "${GREEN}✓${NC} Service is active and running"
    fi
    STATUS="UP"
fi

# ═══════════════════════════════════════════════════════════════════════
# 2. Uptime Check
# ═══════════════════════════════════════════════════════════════════════
print_header "2. Uptime"

ACTIVE_ENTER=$(systemctl show dinerod --property=ActiveEnterTimestamp --value)
if [[ -n "$ACTIVE_ENTER" ]]; then
    START_EPOCH=$(date -d "$ACTIVE_ENTER" +%s 2>/dev/null || echo 0)
    CURRENT_EPOCH=$(date +%s)
    UPTIME_SECONDS=$((CURRENT_EPOCH - START_EPOCH))
    UPTIME_DAYS=$((UPTIME_SECONDS / 86400))
    UPTIME_HOURS=$(((UPTIME_SECONDS % 86400) / 3600))
    UPTIME_MINS=$(((UPTIME_SECONDS % 3600) / 60))

    UPTIME_STR="${UPTIME_DAYS}d ${UPTIME_HOURS}h ${UPTIME_MINS}m"

    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "Started: ${ACTIVE_ENTER}"
        echo -e "Uptime:  ${GREEN}${UPTIME_STR}${NC} (${UPTIME_SECONDS}s)"

        # Progress to 7 days
        PERCENT=$((UPTIME_SECONDS * 100 / 604800))
        echo -e "Progress: ${PERCENT}% to 7-day target"
    fi

    # Alert if uptime is less than expected
    if [[ $UPTIME_DAYS -lt 1 ]] && [[ -f "$LOG_FILE" ]] && [[ $(wc -l < "$LOG_FILE") -gt 10 ]]; then
        ISSUES+=("⚠️  WARNING: Uptime less than 1 day - possible restart")
    fi
else
    UPTIME_STR="unknown"
    UPTIME_SECONDS=0
    ISSUES+=("❌ ERROR: Cannot determine uptime")
fi

# ═══════════════════════════════════════════════════════════════════════
# 3. Restart Count (CRITICAL METRIC)
# ═══════════════════════════════════════════════════════════════════════
print_header "3. Restart Count"

NRESTARTS=$(systemctl show dinerod --property=NRestarts --value)

if [[ "$NRESTARTS" -gt 0 ]]; then
    ISSUES+=("❌ CRITICAL: Service has restarted ${NRESTARTS} times!")
    echo -e "${RED}❌ CRITICAL: ${NRESTARTS} restarts detected!${NC}"
    echo -e "   ${RED}7-day stability test FAILED${NC}"
else
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "${GREEN}✓${NC} Zero restarts (target achieved)"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════
# 4. Memory Usage
# ═══════════════════════════════════════════════════════════════════════
print_header "4. Memory Usage"

MEMORY_CURRENT=$(systemctl show dinerod --property=MemoryCurrent --value)
MEMORY_PEAK=$(systemctl show dinerod --property=MemoryPeak --value)

if [[ "$MEMORY_CURRENT" != "[not set]" ]] && [[ "$MEMORY_CURRENT" -gt 0 ]]; then
    MEMORY_MB=$((MEMORY_CURRENT / 1024 / 1024))
    MEMORY_PEAK_MB=$((MEMORY_PEAK / 1024 / 1024))

    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "Current: ${MEMORY_MB} MB"
        echo -e "Peak:    ${MEMORY_PEAK_MB} MB"
    fi

    # Alert if memory is unusually high (> 500MB suggests leak)
    if [[ $MEMORY_MB -gt 500 ]]; then
        ISSUES+=("⚠️  WARNING: High memory usage: ${MEMORY_MB}MB (possible leak)")
        echo -e "${YELLOW}⚠️  WARNING: High memory usage: ${MEMORY_MB}MB${NC}"
    fi

    # Check for memory growth
    if [[ -f "$LOG_FILE" ]]; then
        PREV_MEMORY=$(grep -v '^#' "$LOG_FILE" | tail -1 | awk -F'|' '{print $5}' | tr -d ' ' || echo "0")
        if [[ -n "$PREV_MEMORY" ]] && [[ "$PREV_MEMORY" != "N/A" ]] && [[ $PREV_MEMORY -gt 0 ]]; then
            MEMORY_GROWTH=$((MEMORY_MB - PREV_MEMORY))
            if [[ $MEMORY_GROWTH -gt 50 ]]; then
                ISSUES+=("⚠️  WARNING: Memory grew by ${MEMORY_GROWTH}MB since last check")
                echo -e "${YELLOW}⚠️  Memory growth: +${MEMORY_GROWTH}MB since last check${NC}"
            fi
        fi
    fi
else
    MEMORY_MB="N/A"
fi

# ═══════════════════════════════════════════════════════════════════════
# 5. CPU Usage
# ═══════════════════════════════════════════════════════════════════════
print_header "5. CPU Usage"

CPU_NS=$(systemctl show dinerod --property=CPUUsageNSec --value)
if [[ "$CPU_NS" != "[not set]" ]] && [[ "$CPU_NS" -gt 0 ]]; then
    CPU_SECONDS=$((CPU_NS / 1000000000))
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "Total CPU time: ${CPU_SECONDS}s"
        if [[ $UPTIME_SECONDS -gt 0 ]]; then
            CPU_PERCENT=$((CPU_SECONDS * 100 / UPTIME_SECONDS))
            echo -e "Average CPU: ${CPU_PERCENT}%"
        fi
    fi
fi

# ═══════════════════════════════════════════════════════════════════════
# 6. RPC Connectivity
# ═══════════════════════════════════════════════════════════════════════
print_header "6. RPC Connectivity"

RPC_OK="NO"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI_PATH="$SCRIPT_DIR/../build/dinero-cli"

if [[ -f "$CLI_PATH" ]]; then
    if timeout 5 "$CLI_PATH" blockchain.getblockcount &>/dev/null; then
        RPC_OK="YES"
        if [[ "$ALERT_ONLY" == false ]]; then
            BLOCK_COUNT=$("$CLI_PATH" blockchain.getblockcount 2>/dev/null || echo "?")
            PEER_COUNT=$("$CLI_PATH" getconnectioncount 2>/dev/null || echo "?")
            echo -e "${GREEN}✓${NC} RPC responding"
            echo -e "  Block height: ${BLOCK_COUNT}"
            echo -e "  Peer count: ${PEER_COUNT}"
        fi
    else
        ISSUES+=("❌ ERROR: RPC not responding")
        echo -e "${RED}❌ RPC not responding!${NC}"
    fi
else
    ISSUES+=("⚠️  WARNING: dinero-cli not found")
    echo -e "${YELLOW}⚠️  dinero-cli not found${NC}"
fi

# ═══════════════════════════════════════════════════════════════════════
# 7. Recent Errors in Journal
# ═══════════════════════════════════════════════════════════════════════
print_header "7. Recent Errors"

ERROR_COUNT=$(journalctl -u dinerod --since "24 hours ago" -p err --no-pager 2>/dev/null | grep -v "^--" | grep -c "^" || echo "0")
ERROR_COUNT=$(echo "$ERROR_COUNT" | tr -d '\n' | tr -d ' ')

if [[ "$ERROR_COUNT" -gt 0 ]]; then
    ISSUES+=("⚠️  WARNING: ${ERROR_COUNT} errors in journal (last 24h)")
    echo -e "${YELLOW}⚠️  ${ERROR_COUNT} errors found in last 24 hours${NC}"
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "\nRecent errors:"
        journalctl -u dinerod --since "24 hours ago" -p err --no-pager 2>/dev/null | tail -5
    fi
else
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "${GREEN}✓${NC} No errors in last 24 hours"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════
# 8. Disk Space
# ═══════════════════════════════════════════════════════════════════════
print_header "8. Disk Space"

DATADIR_SIZE=$(du -sh ~/.dinero 2>/dev/null | awk '{print $1}' || echo "unknown")
DISK_AVAIL=$(df -h ~/.dinero | tail -1 | awk '{print $4}')
DISK_USE_PCT=$(df -h ~/.dinero | tail -1 | awk '{print $5}' | tr -d '%')

if [[ "$ALERT_ONLY" == false ]]; then
    echo -e "Data directory: ${DATADIR_SIZE}"
    echo -e "Available: ${DISK_AVAIL}"
    echo -e "Usage: ${DISK_USE_PCT}%"
fi

if [[ $DISK_USE_PCT -gt 90 ]]; then
    ISSUES+=("⚠️  WARNING: Disk usage high: ${DISK_USE_PCT}%")
    echo -e "${YELLOW}⚠️  Disk usage high: ${DISK_USE_PCT}%${NC}"
fi

# ═══════════════════════════════════════════════════════════════════════
# Write to Log File
# ═══════════════════════════════════════════════════════════════════════
LOG_ENTRY="$NOW | $STATUS | $UPTIME_STR | $NRESTARTS | $MEMORY_MB | $RPC_OK"
echo "$LOG_ENTRY" >> "$LOG_FILE"

# ═══════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════
print_header "Summary"

if [[ ${#ISSUES[@]} -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}✅ ALL CHECKS PASSED${NC}"
    echo -e ""
    if [[ "$ALERT_ONLY" == false ]]; then
        echo -e "Daily metrics logged to: ${LOG_FILE}"
        echo -e ""
        echo -e "${BOLD}Stability Test Status:${NC}"
        echo -e "  Days elapsed: ${UPTIME_DAYS} of 7"
        echo -e "  Restarts: ${GREEN}${NRESTARTS}${NC} (target: 0)"
        echo -e "  Status: ${GREEN}ON TRACK${NC}"
    fi
else
    echo -e "${RED}${BOLD}❌ ISSUES DETECTED (${#ISSUES[@]})${NC}\n"
    for issue in "${ISSUES[@]}"; do
        echo -e "  $issue"
    done
    echo -e ""
    echo -e "${YELLOW}Review logs: sudo journalctl -u dinerod -f${NC}"
    exit 1
fi

# ═══════════════════════════════════════════════════════════════════════
# Show Progress Chart (if not alert-only)
# ═══════════════════════════════════════════════════════════════════════
if [[ "$ALERT_ONLY" == false ]] && [[ -f "$LOG_FILE" ]]; then
    print_header "Progress Over Time"

    echo -e "${BOLD}Last 7 Days:${NC}"
    echo -e "Date                | Status | Uptime      | Restarts | Memory  | RPC"
    echo -e "--------------------+--------+-------------+----------+---------+-----"
    grep -v '^#' "$LOG_FILE" | tail -7 | while IFS='|' read -r timestamp status uptime restarts memory rpc; do
        # Color code status
        if [[ "$status" == *"UP"* ]]; then
            status_colored="${GREEN}UP  ${NC}"
        else
            status_colored="${RED}DOWN${NC}"
        fi

        # Color code restarts
        if [[ "$restarts" == *"0"* ]]; then
            restarts_colored="${GREEN}$restarts${NC}"
        else
            restarts_colored="${RED}$restarts${NC}"
        fi

        echo -e "$timestamp | $status_colored | $uptime | $restarts_colored | $memory | $rpc"
    done

    echo -e ""
    echo -e "Full log: ${LOG_FILE}"
fi

echo -e ""
echo -e "${BOLD}Next check: Run this script again in 24 hours${NC}"
echo -e ""
