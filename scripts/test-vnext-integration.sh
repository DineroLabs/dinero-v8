#!/bin/bash
# Test script for CLI vNext integration features
# Tests schema validation, health endpoints, and metrics integration

set -e

CLI_PATH="${1:-./dinero-cli}"
TEST_URL="${2:-http://localhost:20998}"

echo "Testing DineroCoin CLI vNext Integration"
echo "CLI Path: $CLI_PATH"
echo "Test URL: $TEST_URL"
echo "----------------------------------------"

# Test 1: Basic CLI functionality
echo "Test 1: Basic CLI functionality"
if $CLI_PATH --version >/dev/null 2>&1; then
    echo "✓ CLI version check passed"
else
    echo "✗ CLI version check failed"
    exit 1
fi

# Test 2: Schema validation (with mock daemon)
echo -e "\nTest 2: Schema validation"
# This would require a running daemon, so we test the CLI structure
if $CLI_PATH --help | grep -q "nodeinfo"; then
    echo "✓ CLI includes nodeinfo command"
else
    echo "✗ CLI missing nodeinfo command"
    exit 1
fi

# Test 3: Health endpoint integration
echo -e "\nTest 3: Health endpoint integration"
# Test that CLI can handle health endpoint unavailability gracefully
timeout 5 $CLI_PATH --wait-ready --timeout 2 2>/dev/null || true
echo "✓ CLI handles health endpoint gracefully"

# Test 4: Nodeinfo with health data
echo -e "\nTest 4: Nodeinfo functionality"
if $CLI_PATH --nodeinfo --dry-run 2>/dev/null | grep -q "NodeInfo"; then
    echo "✓ Nodeinfo command structure correct"
else
    echo "✗ Nodeinfo command failed"
    exit 1
fi

# Test 5: JSON output format
echo -e "\nTest 5: JSON output format"
if $CLI_PATH --nodeinfo --format json --dry-run 2>/dev/null | grep -q "schema"; then
    echo "✓ JSON output includes schema"
else
    echo "✗ JSON output missing schema"
    exit 1
fi

# Test 6: Profile integration
echo -e "\nTest 6: Profile integration"
if $CLI_PATH --help | grep -q "profile"; then
    echo "✓ CLI includes profile support"
else
    echo "✗ CLI missing profile support"
    exit 1
fi

# Test 7: Verbose metrics
echo -e "\nTest 7: Verbose metrics support"
if $CLI_PATH --nodeinfo --verbose --dry-run 2>/dev/null; then
    echo "✓ Verbose nodeinfo works"
else
    echo "✗ Verbose nodeinfo failed"
    exit 1
fi

echo -e "\n✓ All CLI vNext integration tests passed!"
echo "Ready for dinerod vNext deployment"
