#!/bin/bash
set -euo pipefail

# Negative test script for witness mining verification
# This script tests that the verification script properly catches failures

echo "🧪 **NEGATIVE TEST: Testing Witness Mining Verification Failure Detection**"
echo "================================================================"

# Test 1: Test with non-existent log file (should fail)
echo ""
echo "🔍 **Test 1: Non-existent log file**"
echo "   Expected: Script should fail with clear error message and exit code 1"
echo ""

# Run the script and capture both output and exit code
./verify-witness-mining.sh --log /tmp/nonexistent.log > /tmp/test1-output.txt 2>&1
EXIT_CODE=$?
OUTPUT=$(cat /tmp/test1-output.txt)

if echo "$OUTPUT" | grep -q "Log file not found" && [ $EXIT_CODE -eq 1 ]; then
    echo "✅ PASS: Script correctly failed with 'Log file not found' error and exit code 1"
else
    echo "❌ FAIL: Script should have failed with 'Log file not found' error and exit code 1"
    echo "   Output: $OUTPUT"
    echo "   Exit code: $EXIT_CODE"
    exit 1
fi

# Test 2: Test with log containing Bech32 errors (should fail in strict mode)
echo ""
echo "🔍 **Test 2: Log with Bech32 errors in strict mode**"
echo "   Expected: Script should fail due to Bech32 errors"
echo ""

# Create a temporary log file with Bech32 errors
TEMP_LOG="/tmp/test-bech32-errors.log"
cat > "$TEMP_LOG" << 'EOF'
[2025-08-23 01:00:00] [INFO] Starting daemon
[2025-08-23 01:00:01] [ERROR] Failed to decode Bech32 address: rdin1invalid
[2025-08-23 01:00:02] [INFO] Daemon started
EOF

# Run the script and capture both output and exit code
OUTPUT=$(./verify-witness-mining.sh --log "$TEMP_LOG" --strict 2>&1)
EXIT_CODE=$?

if echo "$OUTPUT" | grep -q "Bech32 decode errors found" && [ $EXIT_CODE -eq 1 ]; then
    echo "✅ PASS: Script correctly detected Bech32 errors in strict mode and exited with code 1"
else
    echo "❌ FAIL: Script should have detected Bech32 errors in strict mode and exited with code 1"
    echo "   Output: $OUTPUT"
    echo "   Exit code: $EXIT_CODE"
    rm -f "$TEMP_LOG"
    exit 1
fi

# Cleanup
rm -f "$TEMP_LOG"

# Test 3: Test with log containing HRP drift (should fail in strict mode)
echo ""
echo "🔍 **Test 3: Log with HRP drift in strict mode**"
echo "   Expected: Script should fail due to HRP drift (din on regtest)"
echo ""

# Create a temporary log file with HRP drift
TEMP_LOG="/tmp/test-hrp-drift.log"
cat > "$TEMP_LOG" << 'EOF'
[2025-08-23 01:00:00] [INFO] Starting daemon
[2025-08-23 01:00:01] [INFO] RPC server initializing on port 22998 for network with HRP=din
[2025-08-23 01:00:02] [INFO] Using data directory: /tmp/test-dir
[2025-08-23 01:00:03] [INFO] Daemon started
EOF

# Run the script and capture both output and exit code
OUTPUT=$(./verify-witness-mining.sh --log "$TEMP_LOG" --strict 2>&1)
EXIT_CODE=$?

if echo "$OUTPUT" | grep -q "HRP drift detected" && [ $EXIT_CODE -eq 1 ]; then
    echo "✅ PASS: Script correctly detected HRP drift in strict mode and exited with code 1"
else
    echo "❌ FAIL: Script should have detected HRP drift in strict mode and exited with code 1"
    echo "   Output: $OUTPUT"
    echo "   Exit code: $EXIT_CODE"
    rm -f "$TEMP_LOG"
    exit 1
fi

# Cleanup
rm -f "$TEMP_LOG"

# Test 4: Test with log containing no witness-based mining (should fail in strict mode)
echo ""
echo "🔍 **Test 4: Log with no witness-based mining in strict mode**"
echo "   Expected: Script should fail due to no witness-based mining found"
echo ""

# Create a temporary log file with no witness-based mining
TEMP_LOG="/tmp/test-no-witness.log"
cat > "$TEMP_LOG" << 'EOF'
[2025-08-23 01:00:00] [INFO] Starting daemon
[2025-08-23 01:00:01] [INFO] RPC server initializing on port 22998 for network with HRP=rdin
[2025-08-23 01:00:02] [INFO] Using data directory: /tmp/test-dir
[2025-08-23 01:00:03] [INFO] Daemon started
[2025-08-23 01:00:04] [INFO] Mining started
EOF

# Run the script and capture both output and exit code
OUTPUT=$(./verify-witness-mining.sh --log "$TEMP_LOG" --strict 2>&1)
EXIT_CODE=$?

if echo "$OUTPUT" | grep -q "No witness-direct coinbase creation found" && [ $EXIT_CODE -eq 1 ]; then
    echo "✅ PASS: Script correctly detected no witness-based mining in strict mode and exited with code 1"
else
    echo "❌ FAIL: Script should have detected no witness-based mining in strict mode and exited with code 1"
    echo "   Output: $OUTPUT"
    echo "   Exit code: $EXIT_CODE"
    rm -f "$TEMP_LOG"
    exit 1
fi

# Cleanup
rm -f "$TEMP_LOG"

echo ""
echo "🎉 **All negative tests passed!**"
echo "   The verification script correctly detects failures when it should."
echo "   This proves the guardrails are working properly."
echo ""
echo "📋 **Test Summary:**"
echo "   ✅ Non-existent log file - properly fails"
echo "   ✅ Bech32 errors - properly detected in strict mode"
echo "   ✅ HRP drift - properly detected in strict mode"
echo "   ✅ No witness mining - properly detected in strict mode"
echo ""
echo "🚀 **The verification script is robust and reliable!**"
