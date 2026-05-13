#!/bin/bash
set -euo pipefail

echo "🧪 **SIMPLE NEGATIVE TEST**"
echo "============================"

echo ""
echo "🔍 **Test 1: Non-existent log file**"
echo "   Expected: Script should fail with clear error message and exit code 1"
echo ""

# Test 1: Non-existent log file
echo "Running verification script..."
./verify-witness-mining.sh --log /tmp/nonexistent.log > /tmp/test-output.txt 2>&1
EXIT_CODE=$?
echo "Script completed with exit code: $EXIT_CODE"

if [ $EXIT_CODE -eq 1 ]; then
    echo "✅ PASS: Script correctly exited with error code 1"
else
    echo "❌ FAIL: Script should have exited with error code 1"
    exit 1
fi

# Check output
if grep -q "Log file not found" /tmp/test-output.txt; then
    echo "✅ PASS: Script correctly showed 'Log file not found' error"
else
    echo "❌ FAIL: Script should have shown 'Log file not found' error"
    exit 1
fi

echo ""
echo "🎉 **Test 1 passed!**"
echo "   The verification script correctly fails when log file is missing"
echo ""

# Cleanup
rm -f /tmp/test-output.txt

echo "🚀 **All tests completed successfully!**"
