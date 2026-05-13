#!/bin/bash

###############################################################################
# DineroCoin ASERT Difficulty Verification Script
#
# This script verifies that ASERT difficulty calculations are working correctly
# by querying both production servers and checking against known values.
###############################################################################

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Server configuration
CA_SERVER="172.93.160.131:20998"
VA_SERVER="173.249.195.59:20998"
RPC_USER="dinerouser"
RPC_PASS="Dinerosaur1447"

# Expected values for Phase 1
EXPECTED_PHASE1_DIFFICULTY=1023.98
EXPECTED_PHASE1_BITS="0x1d3fffff"
TOLERANCE=1.0  # Allow 1.0 difficulty variation

echo "================================================================"
echo "  DineroCoin ASERT Difficulty Verification"
echo "================================================================"
echo ""

# Function to query difficulty via RPC
query_difficulty() {
    local server=$1
    local server_name=$2

    echo "Querying $server_name ($server)..."

    local response=$(curl -s -u "$RPC_USER:$RPC_PASS" -X POST "http://$server" \
        -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo","params":[]}' 2>&1)

    # Extract difficulty
    local difficulty=$(echo "$response" | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    print(data.get('result', {}).get('difficulty', 'ERROR'))
except:
    print('ERROR')
" 2>&1)

    # Extract height
    local height=$(echo "$response" | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    print(data.get('result', {}).get('blocks', 'ERROR'))
except:
    print('ERROR')
" 2>&1)

    # Extract best block hash
    local hash=$(echo "$response" | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    print(data.get('result', {}).get('bestblockhash', 'ERROR'))
except:
    print('ERROR')
" 2>&1)

    if [ "$difficulty" = "ERROR" ] || [ -z "$difficulty" ]; then
        echo -e "  ${RED}✗ Failed to query difficulty${NC}"
        return 1
    fi

    echo "  Height: $height"
    echo "  Difficulty: $difficulty"
    echo "  Hash: ${hash:0:16}..."

    # Validate difficulty is in expected range
    local diff_check=$(python3 -c "
diff = float('$difficulty')
expected = float('$EXPECTED_PHASE1_DIFFICULTY')
tolerance = float('$TOLERANCE')
if abs(diff - expected) < tolerance:
    print('PASS')
else:
    print('FAIL')
")

    if [ "$diff_check" = "PASS" ]; then
        echo -e "  ${GREEN}✓ Difficulty is correct (expected ~${EXPECTED_PHASE1_DIFFICULTY})${NC}"
    else:
        echo -e "  ${RED}✗ Difficulty is incorrect (expected ~${EXPECTED_PHASE1_DIFFICULTY}, got ${difficulty})${NC}"
        return 1
    fi

    echo ""
    return 0
}

# Function to verify raw bits calculation
verify_bits_calculation() {
    echo "Verifying bits-to-difficulty calculation..."

    local result=$(python3 << 'EOF'
# Calculate difficulty from compact bits
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
    """Calculate difficulty from current bits and pow limit"""
    current_target = bits_to_target(current_bits)
    limit_target = bits_to_target(pow_limit_bits)
    return limit_target / current_target

# Test Phase 1 difficulty
phase1_bits = 0x1d3fffff
pow_limit = 0x1f00ffff

difficulty = difficulty_from_bits(phase1_bits, pow_limit)
print(f"Phase 1 Difficulty: {difficulty:.2f}")

# Verify it's close to expected
if abs(difficulty - 1023.98) < 1.0:
    print("✓ Calculation is correct")
else:
    print(f"✗ Calculation is incorrect (expected ~1023.98, got {difficulty:.2f})")
EOF
)

    echo "$result"

    if echo "$result" | grep -q "✓ Calculation is correct"; then
        echo -e "${GREEN}✓ Bits calculation verified${NC}"
        echo ""
        return 0
    else
        echo -e "${RED}✗ Bits calculation failed${NC}"
        echo ""
        return 1
    fi
}

# Main execution
main() {
    local exit_code=0

    # Verify mathematical calculation
    verify_bits_calculation || exit_code=1

    # Query California server
    query_difficulty "$CA_SERVER" "California" || exit_code=1

    # Query Virginia server
    query_difficulty "$VA_SERVER" "Virginia" || exit_code=1

    # Final summary
    echo "================================================================"
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}✓ All ASERT verification checks passed!${NC}"
    else
        echo -e "${RED}✗ Some ASERT verification checks failed${NC}"
    fi
    echo "================================================================"

    return $exit_code
}

# Run main function
main
exit $?
