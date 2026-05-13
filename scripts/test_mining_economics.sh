#!/bin/bash
# PHASE 3: Mining Economics Verification - 2000 Block Test
# Mines blocks in regtest and compares actual coinbase to expected rewards
# Exit codes: 0 = PASS, 1 = FAIL (mismatch detected)

set -e

DATADIR="/tmp/dinero-economics-test"
CLI="./build/bin/dinero-cli -datadir=$DATADIR"
DAEMON="./build/bin/dinerod"
LOGFILE="/tmp/mining-economics-test.log"
BLOCKS_TO_TEST=2000

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================" | tee "$LOGFILE"
echo "DINERO MINING ECONOMICS VERIFICATION" | tee -a "$LOGFILE"
echo "PHASE 3: 2000-Block Deterministic Test" | tee -a "$LOGFILE"
echo "========================================" | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"

# Clean start
echo "[1/6] Cleaning test environment..." | tee -a "$LOGFILE"
$CLI stop 2>/dev/null || true
sleep 1
rm -rf "$DATADIR"

# Start daemon
echo "[2/6] Starting regtest daemon..." | tee -a "$LOGFILE"
$DAEMON --regtest --datadir="$DATADIR" --txindex --daemon >/dev/null 2>&1
echo "Waiting for daemon to start..."
READY=0
for i in {1..30}; do
    if $CLI getblockchaininfo >/dev/null 2>&1; then
        echo "Daemon ready!"
        READY=1
        break
    fi
    sleep 1
done

if [ "$READY" -eq 0 ]; then
    echo "ERROR: Daemon failed to start within 30 seconds"
    exit 1
fi

# Generate mining address
echo "[3/6] Generating mining address..." | tee -a "$LOGFILE"
ADDRESS=$($CLI wallet.getnewaddress | jq -r '.address')
echo "Mining to: $ADDRESS" | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"

# Test counters
TOTAL_BLOCKS=0
MISMATCHES=0
TOTAL_SUPPLY_ACTUAL=0
TOTAL_SUPPLY_EXPECTED=0

echo "[4/6] Mining $BLOCKS_TO_TEST blocks..." | tee -a "$LOGFILE"
echo "Format: HEIGHT | ACTUAL (una) | EXPECTED (una) | STATUS" | tee -a "$LOGFILE"
echo "------------------------------------------------------------" | tee -a "$LOGFILE"

# Mine blocks and verify
for HEIGHT in $(seq 0 $((BLOCKS_TO_TEST - 1))); do
    # Mine 1 block
    $CLI generatetoaddress 1 "$ADDRESS" >/dev/null 2>&1

    # Get actual coinbase value from the block
    BLOCKHASH=$($CLI getblockhash $HEIGHT | jq -r '.')
    COINBASE_TXID=$($CLI getblock "$BLOCKHASH" 1 | jq -r '.tx[0]')

    # Get full transaction details using blockchain.gettransaction
    COINBASE_TX=$($CLI blockchain.gettransaction "$COINBASE_TXID")

    # Sum all outputs in coinbase
    ACTUAL_SUBSIDY=$(echo "$COINBASE_TX" | jq '.total_output_value_una' | awk '{print int($1)}')

    # Get expected subsidy from economics RPC
    EXPECTED_DATA=$($CLI economics.getblockreward $HEIGHT)
    EXPECTED_SUBSIDY=$(echo "$EXPECTED_DATA" | jq -r '.subsidy_una')
    SUBSIDY_TYPE=$(echo "$EXPECTED_DATA" | jq -r '.type')

    # Compare
    if [ "$ACTUAL_SUBSIDY" != "$EXPECTED_SUBSIDY" ]; then
        echo -e "${RED}MISMATCH${NC} | Height $HEIGHT | Actual: $ACTUAL_SUBSIDY | Expected: $EXPECTED_SUBSIDY | Type: $SUBSIDY_TYPE" | tee -a "$LOGFILE"
        MISMATCHES=$((MISMATCHES + 1))
    else
        # Only log milestones to reduce spam
        if [ $((HEIGHT % 100)) -eq 0 ] || [ "$HEIGHT" -lt 3 ]; then
            echo -e "${GREEN}OK${NC}       | Height $HEIGHT | Subsidy: $ACTUAL_SUBSIDY una | Type: $SUBSIDY_TYPE" | tee -a "$LOGFILE"
        fi
    fi

    # Accumulate supply
    TOTAL_SUPPLY_ACTUAL=$((TOTAL_SUPPLY_ACTUAL + ACTUAL_SUBSIDY))
    TOTAL_SUPPLY_EXPECTED=$((TOTAL_SUPPLY_EXPECTED + EXPECTED_SUBSIDY))
    TOTAL_BLOCKS=$((TOTAL_BLOCKS + 1))
done

echo "" | tee -a "$LOGFILE"
echo "[5/6] Test Summary:" | tee -a "$LOGFILE"
echo "  Blocks tested: $TOTAL_BLOCKS" | tee -a "$LOGFILE"
echo "  Mismatches: $MISMATCHES" | tee -a "$LOGFILE"
echo "  Total supply (actual): $TOTAL_SUPPLY_ACTUAL una" | tee -a "$LOGFILE"
echo "  Total supply (expected): $TOTAL_SUPPLY_EXPECTED una" | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"

# Cleanup
echo "[6/6] Cleaning up..." | tee -a "$LOGFILE"
$CLI stop >/dev/null 2>&1
sleep 1

# Verdict
echo "========================================" | tee -a "$LOGFILE"
if [ "$MISMATCHES" -eq 0 ] && [ "$TOTAL_SUPPLY_ACTUAL" -eq "$TOTAL_SUPPLY_EXPECTED" ]; then
    echo -e "${GREEN}VERDICT: PASS ✅${NC}" | tee -a "$LOGFILE"
    echo "All $TOTAL_BLOCKS blocks have correct subsidy amounts." | tee -a "$LOGFILE"
    echo "Cumulative supply matches expected: $(echo "scale=8; $TOTAL_SUPPLY_ACTUAL / 100000000" | bc) DIN" | tee -a "$LOGFILE"
    exit 0
else
    echo -e "${RED}VERDICT: FAIL ❌${NC}" | tee -a "$LOGFILE"
    echo "Found $MISMATCHES subsidy mismatches in $TOTAL_BLOCKS blocks." | tee -a "$LOGFILE"
    echo "Supply mismatch: Actual=$TOTAL_SUPPLY_ACTUAL, Expected=$TOTAL_SUPPLY_EXPECTED" | tee -a "$LOGFILE"
    exit 1
fi
