#!/usr/bin/env bash
# Regression test for path double-nesting bug
set -euo pipefail

echo "=== Path Regression Test ==="

# Clean up any existing test
rm -rf ./t
pkill -f "dinerod.*--datadir=./t" || true

# Start daemon
echo "Starting daemon..."
./build/dinerod --datadir=./t --regtest --printtoconsole >/dev/null 2>&1 &
PID=$!
sleep 2

# Test nodeinfo.json at root
if test -f ./t/nodeinfo.json; then
    echo "✅ OK_root: nodeinfo.json found at ./t/nodeinfo.json"
else
    echo "❌ FAIL_root: nodeinfo.json not found at root"
    EXIT_CODE=1
fi

# Test cookie in network subdir
if test -f ./t/regtest/.cookie; then
    echo "✅ OK_cookie: .cookie found at ./t/regtest/.cookie"
else
    echo "❌ FAIL_cookie: .cookie not found in network subdir"
    EXIT_CODE=1
fi

# Check for double-nesting (should NOT exist)
if test -f ./t/regtest/t/nodeinfo.json; then
    echo "❌ FAIL_nesting: Found double-nested nodeinfo.json (regression!)"
    EXIT_CODE=1
else
    echo "✅ OK_nesting: No double-nesting detected"
fi

# Cleanup
kill -TERM $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
rm -rf ./t

echo "=== Path Regression Test Complete ==="
exit ${EXIT_CODE:-0}
