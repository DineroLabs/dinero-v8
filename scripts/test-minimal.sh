#!/bin/bash
set -euo pipefail

echo "Starting minimal test..."

# Test 1: Check if jq is available
echo "Testing jq availability..."
if command -v jq >/dev/null 2>&1; then
    echo "✅ jq is available"
else
    echo "❌ jq is not available"
    exit 1
fi

# Test 2: Check if curl is available
echo "Testing curl availability..."
if command -v curl >/dev/null 2>&1; then
    echo "✅ curl is available"
else
    echo "❌ curl is not available"
    exit 1
fi

# Test 3: Check if log file exists
echo "Testing log file check..."
LOG="/tmp/nonexistent.log"
if [ ! -f "$LOG" ]; then
    echo "✅ Log file correctly identified as missing"
else
    echo "❌ Log file incorrectly identified as existing"
    exit 1
fi

echo "All tests passed!"
