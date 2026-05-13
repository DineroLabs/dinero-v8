#!/usr/bin/env bash
# DineroCoin Smoke Test Script
# Tests basic daemon functionality in regtest mode

set -euo pipefail

PLATFORM=$(uname -s | tr A-Z a-z)
DINEROD="${DINEROD:-./build/dinerod}"
DINERO_CLI="${DINERO_CLI:-./build/dinero-cli}"
DATADIR=$(mktemp -d)

# Cleanup function
cleanup() {
  echo "🧹 Cleaning up..."
  $DINERO_CLI -regtest -datadir=$DATADIR stop 2>/dev/null || true
  sleep 2
  rm -rf $DATADIR
}
trap cleanup EXIT

# Test counter
TESTS_RUN=0
TESTS_PASSED=0

# Test helper
run_test() {
  local test_name=$1
  local test_command=$2

  TESTS_RUN=$((TESTS_RUN + 1))
  echo ""
  echo "▶️  Test $TESTS_RUN: $test_name"

  if eval "$test_command"; then
    echo "   ✅ PASSED"
    TESTS_PASSED=$((TESTS_PASSED + 1))
    return 0
  else
    echo "   ❌ FAILED"
    return 1
  fi
}

# ═══════════════════════════════════════════════════════════════════════════
# TEST SUITE
# ═══════════════════════════════════════════════════════════════════════════

echo "════════════════════════════════════════════════════════════════"
echo "DineroCoin Smoke Tests - $PLATFORM"
echo "════════════════════════════════════════════════════════════════"
echo "Daemon:   $DINEROD"
echo "CLI:      $DINERO_CLI"
echo "Data dir: $DATADIR"
echo "════════════════════════════════════════════════════════════════"

# Test 1: Daemon binary exists
run_test "Daemon binary exists" "test -f $DINEROD"

# Test 2: CLI binary exists
run_test "CLI binary exists" "test -f $DINERO_CLI"

# Test 3: Daemon is executable
run_test "Daemon is executable" "test -x $DINEROD"

# Test 4: Start daemon
run_test "Start daemon in regtest mode" \
  "$DINEROD -regtest -daemon -datadir=$DATADIR -printtoconsole=0"

# Wait for startup
echo "⏳ Waiting for daemon startup..."
sleep 5

# Test 5: Daemon process running
run_test "Daemon process is running" \
  "pgrep -f 'dinerod.*regtest' > /dev/null"

# Test 6: RPC connectivity
run_test "RPC responds to ping" \
  "$DINERO_CLI -regtest -datadir=$DATADIR ping > /dev/null"

# Test 7: Get blockchain info
run_test "Get blockchain info" \
  "$DINERO_CLI -regtest -datadir=$DATADIR getblockchaininfo | grep -q '\"chain\"'"

# Test 8: Generate address
run_test "Generate new address" \
  "ADDRESS=\$($DINERO_CLI -regtest -datadir=$DATADIR getnewaddress) && [ ! -z \"\$ADDRESS\" ]"

# Test 9: Check initial balance (should be 0)
run_test "Check initial balance is 0" \
  "$DINERO_CLI -regtest -datadir=$DATADIR getbalance | grep -q '^0'"

# Test 10: Generate blocks
run_test "Generate 101 blocks (for coinbase maturity)" \
  "$DINERO_CLI -regtest -datadir=$DATADIR generatetoaddress 101 \$($DINERO_CLI -regtest -datadir=$DATADIR getnewaddress) | grep -q '\"' "

# Wait for block processing
sleep 2

# Test 11: Check balance after mining
run_test "Check balance is non-zero after mining" \
  "test \$($DINERO_CLI -regtest -datadir=$DATADIR getbalance | sed 's/\\..*//') -gt 0"

# Test 12: List unspent outputs
run_test "List unspent outputs (should have coinbase UTXOs)" \
  "$DINERO_CLI -regtest -datadir=$DATADIR listunspent | grep -q 'txid'"

# Test 13: Get block count
run_test "Get block count (should be 101)" \
  "test \$($DINERO_CLI -regtest -datadir=$DATADIR getblockcount) -eq 101"

# Test 14: Get best block hash
run_test "Get best block hash" \
  "$DINERO_CLI -regtest -datadir=$DATADIR getbestblockhash | grep -qE '^[0-9a-f]{64}$'"

# Test 15: Stop daemon
run_test "Stop daemon cleanly" \
  "$DINERO_CLI -regtest -datadir=$DATADIR stop"

# Wait for shutdown
sleep 3

# Test 16: Daemon process terminated
run_test "Daemon process terminated" \
  "! pgrep -f 'dinerod.*regtest' > /dev/null"

# ═══════════════════════════════════════════════════════════════════════════
# PLATFORM-SPECIFIC TESTS
# ═══════════════════════════════════════════════════════════════════════════

if [ "$PLATFORM" = "darwin" ]; then
  echo ""
  echo "════════════════════════════════════════════════════════════════"
  echo "macOS Platform-Specific Tests"
  echo "════════════════════════════════════════════════════════════════"

  # Test: Binary architecture
  run_test "Verify binary architecture (Mach-O)" \
    "file $DINEROD | grep -q 'Mach-O'"

  # Test: No Homebrew dependencies
  run_test "No Homebrew dependencies" \
    "! otool -L $DINEROD | grep -q '/opt/homebrew'"

  # Test: GUI bundle structure (optional)
  if [ -d "build/gui/dinero-qt.app" ]; then
    run_test "GUI bundle exists" \
      "test -f build/gui/dinero-qt.app/Contents/MacOS/dinero-qt"
  fi

elif [ "$PLATFORM" = "linux" ]; then
  echo ""
  echo "════════════════════════════════════════════════════════════════"
  echo "Linux Platform-Specific Tests"
  echo "════════════════════════════════════════════════════════════════"

  # Test: ELF binary format
  run_test "Verify ELF binary" \
    "file $DINEROD | grep -q 'ELF.*LSB.*executable'"

  # Test: No gRPC dependencies
  run_test "No gRPC dependencies" \
    "! ldd $DINEROD | grep -iq 'grpc'"

  # Test: No protobuf dependencies
  run_test "No protobuf dependencies" \
    "! ldd $DINEROD | grep -iq 'protobuf'"

  # Test: No abseil dependencies
  run_test "No abseil dependencies" \
    "! ldd $DINEROD | grep -iq 'absl'"
fi

# ═══════════════════════════════════════════════════════════════════════════
# RESULTS
# ═══════════════════════════════════════════════════════════════════════════

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "SMOKE TEST RESULTS"
echo "════════════════════════════════════════════════════════════════"
echo "Tests run:    $TESTS_RUN"
echo "Tests passed: $TESTS_PASSED"
echo "Tests failed: $((TESTS_RUN - TESTS_PASSED))"
echo "════════════════════════════════════════════════════════════════"

if [ $TESTS_PASSED -eq $TESTS_RUN ]; then
  echo "✅ ALL SMOKE TESTS PASSED"
  exit 0
else
  echo "❌ SOME SMOKE TESTS FAILED"
  exit 1
fi
