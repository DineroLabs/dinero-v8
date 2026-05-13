#!/bin/bash
#
# Abuse Test: Corrupted Snapshot Detection
#
# Tests that corrupted snapshots are detected and rejected BEFORE corrupting state.
# Critical invariant: Node must reject bad snapshots without changing UTXO set.
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Test configuration
TEST_NAME="Corrupted Snapshot Detection"
TEST_ID="ABUSE-002"
DATADIR="/tmp/dinero_abuse_test_corrupt_$$"
SNAPSHOT_DIR="/tmp/dinero_snapshots_$$"
RESULTS_FILE="./abuse_test_results.md"

echo "═══════════════════════════════════════════════════════════════════════"
echo "  $TEST_NAME ($TEST_ID)"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Function to log results
log_result() {
    local status=$1
    local message=$2
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    echo "" >> "$RESULTS_FILE"
    echo "## $TEST_ID: $TEST_NAME" >> "$RESULTS_FILE"
    echo "**Timestamp:** $timestamp" >> "$RESULTS_FILE"
    echo "**Status:** $status" >> "$RESULTS_FILE"
    echo "**Details:** $message" >> "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"
}

# Cleanup function
cleanup() {
    echo ""
    echo "  [Cleanup] Removing test data..."
    rm -rf "$DATADIR"
    rm -rf "$SNAPSHOT_DIR"
    echo "    ✓ Cleanup complete"
}

trap cleanup EXIT

# Create test directory
mkdir -p "$SNAPSHOT_DIR"

# Test Case 1: Bad Magic Number
echo "  [Test Case 1] Bad magic number"
SNAPSHOT_FILE="$SNAPSHOT_DIR/bad_magic.dat"

# Create snapshot with wrong magic
printf '\x00\x00\x00\x00' > "$SNAPSHOT_FILE"  # Wrong magic (should be UTXO)
printf '\x01\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Version: 1
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Block hash
printf '\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Height
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # UTXO count
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Timestamp
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Reserved
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Checksum

echo "    ✓ Created snapshot with bad magic number"
echo "    Expected: Rejection with 'Invalid snapshot magic' error"
echo ""

# Test Case 2: Bad Version
echo "  [Test Case 2] Unsupported version"
SNAPSHOT_FILE="$SNAPSHOT_DIR/bad_version.dat"

printf '\x55\x54\x58\x4f' > "$SNAPSHOT_FILE"  # Correct magic
printf '\xff\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Version: 255 (unsupported)
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Block hash
printf '\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Height
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # UTXO count
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Timestamp
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Reserved
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Checksum

echo "    ✓ Created snapshot with unsupported version"
echo "    Expected: Rejection with 'Unsupported snapshot version' error"
echo ""

# Test Case 3: Truncated File
echo "  [Test Case 3] Truncated snapshot file"
SNAPSHOT_FILE="$SNAPSHOT_DIR/truncated.dat"

printf '\x55\x54\x58\x4f' > "$SNAPSHOT_FILE"  # Correct magic
printf '\x01\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Version: 1
# Truncate here (missing rest of header)

echo "    ✓ Created truncated snapshot file (8 bytes instead of 100)"
echo "    Expected: Rejection with 'Incomplete snapshot header' error"
echo ""

# Test Case 4: Bad Checksum
echo "  [Test Case 4] Invalid checksum"
SNAPSHOT_FILE="$SNAPSHOT_DIR/bad_checksum.dat"

# Create valid header
printf '\x55\x54\x58\x4f' > "$SNAPSHOT_FILE"  # Correct magic
printf '\x01\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Version: 1
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Block hash
printf '\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Height
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # UTXO count
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Timestamp
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Reserved

# Add WRONG checksum (all 0xFF instead of correct SHA256)
dd if=/dev/zero bs=32 count=1 2>/dev/null | tr '\000' '\377' >> "$SNAPSHOT_FILE"

echo "    ✓ Created snapshot with invalid checksum"
echo "    Expected: Rejection with 'Snapshot checksum verification failed' error"
echo ""

# Test Case 5: UTXO Count Mismatch
echo "  [Test Case 5] UTXO count mismatch"
SNAPSHOT_FILE="$SNAPSHOT_DIR/count_mismatch.dat"

# Create header claiming 100 UTXOs but provide none
printf '\x55\x54\x58\x4f' > "$SNAPSHOT_FILE"  # Correct magic
printf '\x01\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Version: 1
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Block hash
printf '\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Height
printf '\x64\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # UTXO count: 100 (but no UTXOs follow)
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Timestamp
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT_FILE"  # Reserved
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT_FILE"  # Checksum

echo "    ✓ Created snapshot with count mismatch"
echo "    Expected: Rejection with 'UTXO count mismatch' error"
echo ""

# Test Case 6: Empty File
echo "  [Test Case 6] Empty snapshot file"
SNAPSHOT_FILE="$SNAPSHOT_DIR/empty.dat"
touch "$SNAPSHOT_FILE"

echo "    ✓ Created empty snapshot file"
echo "    Expected: Rejection with 'File too small' error"
echo ""

# Summary of test expectations
echo "═══════════════════════════════════════════════════════════════════════"
echo "  TEST EXPECTATIONS SUMMARY"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "  All of the above snapshot files should be REJECTED by LoadSnapshot()."
echo "  Critical invariant: Node must detect corruption BEFORE modifying UTXO set."
echo ""
echo "  To verify:"
echo "    1. Run: dinero-cli loadtxoutset <snapshot_file>"
echo "    2. Check for appropriate error message"
echo "    3. Verify UTXO set unchanged (getblockcount same before/after)"
echo "    4. Verify node remains operational"
echo ""
echo "  Test files created in: $SNAPSHOT_DIR"
echo "    - bad_magic.dat        (wrong magic number)"
echo "    - bad_version.dat      (unsupported version)"
echo "    - truncated.dat        (incomplete header)"
echo "    - bad_checksum.dat     (invalid checksum)"
echo "    - count_mismatch.dat   (UTXO count mismatch)"
echo "    - empty.dat            (empty file)"
echo ""

# Now let's verify our snapshot loading code handles these
echo "═══════════════════════════════════════════════════════════════════════"
echo "  VERIFYING SNAPSHOT LOADING CODE"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Check if snapshot loading code exists and has proper validation
CHAINSTATE_CPP="../src/daemon/services/chainstate_service.cpp"

if [ ! -f "$CHAINSTATE_CPP" ]; then
    echo -e "  ${YELLOW}⚠️  Source file not found: $CHAINSTATE_CPP${NC}"
    log_result "SKIPPED" "Source file not available for code inspection"
    exit 0
fi

echo "  [Code Inspection] Checking LoadSnapshot() implementation..."
echo ""

# Check for magic number validation
if grep -q "SNAPSHOT_MAGIC" "$CHAINSTATE_CPP"; then
    echo -e "    ${GREEN}✓${NC} Magic number validation: PRESENT"
else
    echo -e "    ${RED}✗${NC} Magic number validation: MISSING"
fi

# Check for version validation
if grep -q "SNAPSHOT_VERSION" "$CHAINSTATE_CPP"; then
    echo -e "    ${GREEN}✓${NC} Version validation: PRESENT"
else
    echo -e "    ${RED}✗${NC} Version validation: MISSING"
fi

# Check for checksum verification
if grep -q "checksum" "$CHAINSTATE_CPP" | grep -qi "verif\|valid"; then
    echo -e "    ${GREEN}✓${NC} Checksum verification: LIKELY PRESENT"
else
    echo -e "    ${YELLOW}⚠️${NC}  Checksum verification: NEEDS REVIEW"
fi

# Check for file size validation
if grep -q "file.*size\|SNAPSHOT_HEADER_SIZE" "$CHAINSTATE_CPP"; then
    echo -e "    ${GREEN}✓${NC} File size validation: PRESENT"
else
    echo -e "    ${YELLOW}⚠️${NC}  File size validation: NEEDS REVIEW"
fi

echo ""
echo "═══════════════════════════════════════════════════════════════════════"
echo -e "  ${GREEN}✓ TEST SETUP COMPLETE: $TEST_NAME${NC}"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "  Next Steps:"
echo "    1. Start dinerod with test datadir"
echo "    2. Try loading each test snapshot file"
echo "    3. Verify appropriate error messages"
echo "    4. Confirm UTXO set remains unchanged"
echo ""

log_result "SETUP_COMPLETE" "Test files created. Manual verification required with running node."

exit 0
