#!/bin/bash

echo "========================================="
echo "Dinero Full Test Suite - November 7, 2025"
echo "========================================="
echo ""

PASSED=0
FAILED=0
SKIPPED=0

tests=(
    "test_bech32_validator"
    "test_bip39"
    "test_bip84_addr_check"
    "test_change_addresses"
    "test_codebase_verification"
    "test_daa_phase_transition"
    "test_hd_wallet"
    "test_mining_comprehensive"
    "test_mining_smoke"
    "test_psbt_comprehensive"
    "test_wallet_comprehensive"
    "test_wallet_integration"
    "test_wallet_recovery"
)

for test in "${tests[@]}"; do
    if [ -f "build/$test" ]; then
        echo "Running $test..."
        if timeout 30 "build/$test" > "/tmp/${test}.log" 2>&1; then
            echo "  ✅ PASS"
            ((PASSED++))
        else
            echo "  ❌ FAIL (see /tmp/${test}.log)"
            ((FAILED++))
        fi
    else
        echo "  ⚠️  SKIP (not built)"
        ((SKIPPED++))
    fi
done

echo ""
echo "========================================="
echo "Test Suite Summary"
echo "========================================="
echo "Passed:  $PASSED"
echo "Failed:  $FAILED"  
echo "Skipped: $SKIPPED"
echo "Total:   $((PASSED + FAILED + SKIPPED))"
echo "========================================="

if [ $FAILED -eq 0 ]; then
    echo "✅ All tests PASSED!"
    exit 0
else
    echo "❌ Some tests FAILED"
    exit 1
fi
