#!/bin/bash

###############################################################################
# DineroCoin ASERT Progression Examination Script
#
# This script examines the ASERT difficulty progression across different
# heights and phases, verifying that difficulty adjustments are smooth and
# predictable.
###############################################################################

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
CA_SERVER="172.93.160.131:20998"
VA_SERVER="173.249.195.59:20998"
RPC_USER="dinerouser"
RPC_PASS="Dinerosaur1447"

echo "================================================================"
echo "  DineroCoin ASERT Progression Examination"
echo "================================================================"
echo ""

# Function to query block at specific height
query_block_at_height() {
    local server=$1
    local height=$2

    # Get block hash
    local hash=$(curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$server" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getblockhash\",\"params\":[$height]}" | \
        python3 -c "import sys, json; print(json.load(sys.stdin).get('result', ''))" 2>&1)

    if [ -z "$hash" ] || [ "$hash" = "null" ]; then
        return 1
    fi

    # Get block details
    curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$server" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getblock\",\"params\":[\"$hash\"]}" | \
        python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    block = data.get('result', {})
    print(f\"{block.get('time', 'N/A')}|{block.get('difficulty', 'N/A')}|{block.get('bits', 'N/A')}\")
except:
    print('')
" 2>&1
}

# Function to get current height
get_current_height() {
    curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$1" \
        -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' | \
        python3 -c "import sys, json; print(json.load(sys.stdin).get('result', 0))" 2>&1
}

# Function to calculate theoretical ASERT difficulty
calculate_theoretical_difficulty() {
    python3 << 'EOF'
import math

def bits_to_target(bits):
    """Convert compact bits to target value"""
    exponent = (bits >> 24) & 0xFF
    mantissa = bits & 0xFFFFFF
    if exponent <= 3:
        mantissa = mantissa >> (8 * (3 - exponent))
        return mantissa
    else:
        return mantissa << (8 * (exponent - 3))

def difficulty_from_bits(current_bits, pow_limit_bits):
    """Calculate difficulty from bits"""
    current_target = bits_to_target(current_bits)
    limit_target = bits_to_target(pow_limit_bits)
    return limit_target / current_target

# DineroCoin ASERT parameters
pow_limit_bits = 0x1f00ffff
phase1_bits = 0x1d3fffff  # Heights 1-180,000
phase2_bits = 0x1d00ffff  # Heights 180,001+

phase1_diff = difficulty_from_bits(phase1_bits, pow_limit_bits)
phase2_diff = difficulty_from_bits(phase2_bits, pow_limit_bits)

print(f"Phase 1 (1-180,000): {phase1_diff:.4f}")
print(f"Phase 2 (180,001+):  {phase2_diff:.4f}")
print(f"Transition factor:   {phase2_diff/phase1_diff:.2f}x")
EOF
}

# Examine progression on California server
examine_server() {
    local server=$1
    local server_name=$2

    echo -e "${BLUE}=== $server_name Server ($server) ===${NC}"
    echo ""

    # Get current height
    local height=$(get_current_height "$server")

    if [ "$height" -eq 0 ]; then
        echo -e "${RED}✗ Failed to get blockchain height${NC}"
        return 1
    fi

    echo "Current Height: $height"
    echo ""

    # Examine blocks at different heights
    echo "Height | Time      | Difficulty    | Bits       | Phase"
    echo "-------|-----------|---------------|------------|-------"

    # Examine recent blocks
    local heights_to_check=(1 2 3 5 10)

    # Add current height if different
    if [ $height -gt 10 ]; then
        heights_to_check+=($((height - 2)))
        heights_to_check+=($((height - 1)))
        heights_to_check+=($height)
    fi

    for h in "${heights_to_check[@]}"; do
        if [ $h -le $height ]; then
            local info=$(query_block_at_height "$server" "$h")

            if [ -n "$info" ]; then
                IFS='|' read -r time difficulty bits <<< "$info"

                # Determine phase
                local phase="1"
                if [ $h -gt 180000 ]; then
                    phase="2"
                fi

                printf "%-6d | %-9s | %-13s | %-10s | %s\n" \
                    "$h" "$time" "$difficulty" "$bits" "$phase"
            fi
        fi
    done

    echo ""
}

# Main execution
main() {
    echo "Theoretical ASERT Difficulty Values:"
    echo "-----------------------------------"
    calculate_theoretical_difficulty
    echo ""
    echo "================================================================"
    echo ""

    # Examine California server
    examine_server "$CA_SERVER" "California"

    # Examine Virginia server
    examine_server "$VA_SERVER" "Virginia"

    echo "================================================================"
    echo -e "${GREEN}Analysis Complete${NC}"
    echo "================================================================"
    echo ""
    echo "Key Observations:"
    echo "- Phase 1 (heights 1-180,000): Difficulty should be ~1023.98"
    echo "- Phase 2 (heights 180,001+):  Difficulty should be ~65,536.0"
    echo "- Transition happens at height 180,001"
    echo "- ASERT adjusts per-block based on time delta and half-life"
    echo ""
    echo "Expected ASERT Behavior:"
    echo "- Smooth exponential adjustment"
    echo "- No difficulty jumps or oscillations"
    echo "- Half-life of 2 days (172,800 seconds)"
    echo "- Target block time: 5 minutes (300 seconds)"
}

# Run main
main
exit 0
