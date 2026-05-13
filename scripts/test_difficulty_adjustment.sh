#!/bin/bash

###############################################################################
# DineroCoin Difficulty Adjustment Testing Script
#
# This script tests ASERT difficulty adjustment by monitoring blocks as they
# are mined and verifying difficulty changes over time.
###############################################################################

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuration
SERVER="172.93.160.131:20998"
RPC_USER="dinerouser"
RPC_PASS="Dinerosaur1447"
BLOCKS_TO_MONITOR=10
CHECK_INTERVAL=30  # seconds

echo "================================================================"
echo "  DineroCoin Difficulty Adjustment Test"
echo "================================================================"
echo ""
echo "Monitoring: $BLOCKS_TO_MONITOR blocks"
echo "Check interval: ${CHECK_INTERVAL}s"
echo ""

# Function to get current block info
get_block_info() {
    local height=$1

    # Get block hash
    local hash=$(curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$SERVER" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getblockhash\",\"params\":[$height]}" | \
        python3 -c "import sys, json; data = json.load(sys.stdin); print(data.get('result', ''))" 2>&1)

    if [ -z "$hash" ] || [ "$hash" = "null" ]; then
        echo "ERROR"
        return 1
    fi

    # Get block details
    local block=$(curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$SERVER" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getblock\",\"params\":[\"$hash\"]}" | \
        python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    block = data.get('result', {})
    print(f\"{block.get('time', 'N/A')}|{block.get('difficulty', 'N/A')}|{block.get('bits', 'N/A')}|{block.get('nonce', 'N/A')}\")
except:
    print('ERROR')
" 2>&1)

    echo "$block"
}

# Function to get current blockchain height
get_height() {
    curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$SERVER" \
        -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' | \
        python3 -c "import sys, json; print(json.load(sys.stdin).get('result', 0))" 2>&1
}

# Main monitoring loop
main() {
    echo "Getting starting height..."
    local start_height=$(get_height)

    if [ "$start_height" -eq 0 ]; then
        echo -e "${RED}✗ Failed to get blockchain height${NC}"
        exit 1
    fi

    echo "Starting height: $start_height"
    echo ""
    echo "Block | Time      | Difficulty  | Bits       | Nonce"
    echo "------|-----------|-------------|------------|------------"

    local blocks_seen=0
    local last_height=$start_height

    # Monitor existing blocks first
    for (( h=$start_height; h>$start_height-5 && h>=1; h-- )); do
        local info=$(get_block_info $h)
        if [ "$info" != "ERROR" ]; then
            IFS='|' read -r time difficulty bits nonce <<< "$info"
            printf "%-5d | %-9s | %-11s | %-10s | %s\n" "$h" "$time" "$difficulty" "$bits" "$nonce"
        fi
    done

    echo "------|-----------|-------------|------------|------------"
    echo "Waiting for new blocks..."
    echo ""

    # Monitor for new blocks
    while [ $blocks_seen -lt $BLOCKS_TO_MONITOR ]; do
        local current_height=$(get_height)

        if [ "$current_height" -gt "$last_height" ]; then
            # New block found
            local info=$(get_block_info $current_height)
            if [ "$info" != "ERROR" ]; then
                IFS='|' read -r time difficulty bits nonce <<< "$info"
                printf "${GREEN}%-5d | %-9s | %-11s | %-10s | %s${NC}\n" \
                    "$current_height" "$time" "$difficulty" "$bits" "$nonce"

                blocks_seen=$((blocks_seen + 1))
                last_height=$current_height
            fi
        fi

        if [ $blocks_seen -lt $BLOCKS_TO_MONITOR ]; then
            sleep $CHECK_INTERVAL
        fi
    done

    echo ""
    echo "================================================================"
    echo -e "${GREEN}✓ Monitored $BLOCKS_TO_MONITOR blocks successfully${NC}"
    echo "================================================================"
    echo ""
    echo "Analysis:"
    echo "- All blocks should show difficulty ~1023.98 (Phase 1)"
    echo "- Bits should be 0x1d3fffff for heights 1-180,000"
    echo "- Block times should average ~5 minutes (300 seconds)"
}

# Run main
main
exit 0
