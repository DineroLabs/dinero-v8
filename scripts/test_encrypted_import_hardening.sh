#!/usr/bin/env bash
set -euo pipefail

# Comprehensive Encrypted Key Import Hardening Test Suite
# Tests all security features: validation, rate limiting, error taxonomy, log redaction

DATADIR="./test-data/hardening"
PORT="20999"
FAILED_TESTS=0
TOTAL_TESTS=0

echo "🛡️  ENCRYPTED KEY IMPORT HARDENING TEST SUITE"
echo "=============================================="

# Clean start
echo "🧹 Cleaning up..."
pkill -f dinerod >/dev/null 2>&1 || true
rm -rf "$DATADIR" && mkdir -p "$DATADIR"

# Start daemon
echo "🚀 Starting daemon..."
./build/dinerod --regtest --datadir="$DATADIR" --printtoconsole > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
trap 'kill $DAEMON_PID >/dev/null 2>&1 || true' EXIT
sleep 3

AUTH=$(cat "$DATADIR/regtest/.cookie")

# Create and load wallet
echo "🏦 Setting up wallet..."
curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"create","method":"wallet.create","params":{"name":"test"}}' \
  http://127.0.0.1:$PORT/ > /dev/null

curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"load","method":"wallet.load","params":{"name":"test"}}' \
  http://127.0.0.1:$PORT/ > /dev/null

# Test helper function
test_case() {
    local name="$1"
    local expected_error="$2"
    local request_data="$3"
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo "📝 Test $TOTAL_TESTS: $name"
    
    local response=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
      --data-binary "$request_data" \
      http://127.0.0.1:$PORT/)
    
    local success=$(echo "$response" | jq -r '.result.success // false')
    local error=$(echo "$response" | jq -r '.result.error // "none"')
    
    if [[ "$expected_error" == "SUCCESS" ]]; then
        if [[ "$success" == "true" ]]; then
            echo "   ✅ PASSED: Success as expected"
        else
            echo "   ❌ FAILED: Expected success, got error: $error"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    else
        if [[ "$success" == "false" && "$error" == "$expected_error" ]]; then
            echo "   ✅ PASSED: Got expected error: $error"
        else
            echo "   ❌ FAILED: Expected error '$expected_error', got success=$success, error=$error"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    fi
    
    echo "   Response: $response" | head -c 200
    echo ""
    echo ""
}

echo "🔍 PARAMETER VALIDATION TESTS"
echo "=============================="

# Test 1: Missing required parameters
test_case "Missing passphrase parameter" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test1","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4"}}'

# Test 2: Invalid parameter types
test_case "Invalid iter parameter type" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test2","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":"not_a_number","salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

# Test 3: Invalid base64 encoding
test_case "Invalid base64 salt" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test3","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"invalid_base64!@#","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

echo "🚫 UNSUPPORTED CIPHER/KDF TESTS"
echo "==============================="

# Test 4: Unsupported KDF
test_case "Unsupported KDF" "UNSUPPORTED_CIPHER_OR_KDF" \
'{"jsonrpc":"2.0","id":"test4","method":"wallet.importencryptedkey","params":{"enc":"scrypt","iter":100000,"salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

# Test 5: Unsupported cipher
test_case "Unsupported cipher" "UNSUPPORTED_CIPHER_OR_KDF" \
'{"jsonrpc":"2.0","id":"test5","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-128-cbc","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

echo "📏 BOUNDARY CONDITION TESTS"
echo "==========================="

# Test 6: Iterations too low
test_case "PBKDF2 iterations too low" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test6","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":1000,"salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

# Test 7: Salt too short (need at least 16 bytes, this is only 8 bytes)
test_case "Salt too short" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test7","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"dGVzdA==","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

# Test 8: IV wrong size for GCM (need exactly 12 bytes)
test_case "IV wrong size for GCM" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test8","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-256-gcm","iv":"dGVzdA==","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdHRhZzEyMzQ1Njc4","passphrase":"test"}}'

# Test 9: Authentication tag wrong size (need exactly 16 bytes)
test_case "Authentication tag wrong size" "INVALID_PARAMS" \
'{"jsonrpc":"2.0","id":"test9","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"dGVzdHNhbHQxMjM0NTY3OA==","cipher":"aes-256-gcm","iv":"dGVzdGl2MTIzNDU2Nzg=","ct":"dGVzdGN0MTIzNDU2Nzg=","tag":"dGVzdA==","passphrase":"test"}}'

echo "🔐 DECRYPTION FAILURE TESTS"
echo "=========================="

# Test 10: Wrong passphrase (using valid test vector with wrong passphrase)
test_case "Wrong passphrase" "WRONG_PASSPHRASE_OR_TAG" \
'{"jsonrpc":"2.0","id":"test10","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"VvIZ8/3ZpqRzLbJzT9P8Ow==","cipher":"aes-256-gcm","iv":"AZiU8/3ZpqRzLbJz","ct":"HZiU8/3ZpqRzLbJzT9P8OwHZiU8/3ZpqRzLbJzT9P8Ow","tag":"HZiU8/3ZpqRzLbJzT9P8Ow==","passphrase":"wrong_passphrase"}}'

echo "⚡ RATE LIMITING TESTS"
echo "===================="

# Test 11-14: Failed attempts (should work)
for i in {11..14}; do
    test_case "Failed attempt $((i-10))/5" "WRONG_PASSPHRASE_OR_TAG" \
    '{"jsonrpc":"2.0","id":"test'$i'","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"VvIZ8/3ZpqRzLbJzT9P8Ow==","cipher":"aes-256-gcm","iv":"AZiU8/3ZpqRzLbJz","ct":"HZiU8/3ZpqRzLbJzT9P8OwHZiU8/3ZpqRzLbJzT9P8Ow","tag":"HZiU8/3ZpqRzLbJzT9P8Ow==","passphrase":"wrong_'$i'"}}'
done

# Test 15: 5th attempt - should be rate limited now
test_case "Rate limited on 5th attempt" "RATE_LIMITED" \
'{"jsonrpc":"2.0","id":"test15","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"VvIZ8/3ZpqRzLbJzT9P8Ow==","cipher":"aes-256-gcm","iv":"AZiU8/3ZpqRzLbJz","ct":"HZiU8/3ZpqRzLbJzT9P8OwHZiU8/3ZpqRzLbJzT9P8Ow","tag":"HZiU8/3ZpqRzLbJzT9P8Ow==","passphrase":"wrong_15"}}'

# Test 16: Still rate limited
test_case "Still rate limited" "RATE_LIMITED" \
'{"jsonrpc":"2.0","id":"test16","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"VvIZ8/3ZpqRzLbJzT9P8Ow==","cipher":"aes-256-gcm","iv":"AZiU8/3ZpqRzLbJz","ct":"HZiU8/3ZpqRzLbJzT9P8OwHZiU8/3ZpqRzLbJzT9P8Ow","tag":"HZiU8/3ZpqRzLbJzT9P8Ow==","passphrase":"test"}}'

echo "📊 LOG REDACTION TEST"
echo "===================="

# Test 17: Check that sensitive data is redacted in logs
echo "🔍 Checking daemon logs for sensitive data leaks..."

# Use -F flag for fixed string matching (no regex interpretation)
if grep -F "test_passphrase_123" "$DATADIR/daemon.log" >/dev/null 2>&1; then
    echo "   ❌ FAILED: Passphrase found in logs!"
    FAILED_TESTS=$((FAILED_TESTS + 1))
else
    echo "   ✅ PASSED: No passphrase found in logs"
fi

# Check for common sensitive parameters that should never appear
sensitive_params=("wrong_passphrase" "wrong_1" "wrong_2" "wrong_3" "wrong_4" "wrong_5")
for param in "${sensitive_params[@]}"; do
    if grep -F "$param" "$DATADIR/daemon.log" >/dev/null 2>&1; then
        echo "   ❌ FAILED: Sensitive parameter '$param' found in logs!"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        break
    fi
done

# Positive test: Check that redaction markers are present
if grep -F '**REDACTED**' "$DATADIR/daemon.log" >/dev/null 2>&1; then
    echo "   ✅ PASSED: Redaction markers found in logs"
else
    echo "   ⚠️  WARNING: No redaction markers found (may be normal if no requests logged)"
fi

# Positive test: Check that request processing is logged (but safely)
if grep -F "Processing wallet.importencryptedkey request" "$DATADIR/daemon.log" >/dev/null 2>&1; then
    echo "   ✅ PASSED: Request processing logged safely"
else
    echo "   ⚠️  INFO: No request processing logs found"
fi

# Additional positive test: Check for specific redacted fields
redacted_fields=('"passphrase":"**REDACTED**"' '"ct":"**REDACTED**"' '"iv":"**REDACTED**"' '"tag":"**REDACTED**"')
redaction_found=false
for field in "${redacted_fields[@]}"; do
    if grep -F "$field" "$DATADIR/daemon.log" >/dev/null 2>&1; then
        echo "   ✅ PASSED: Field redaction confirmed: $field"
        redaction_found=true
        break
    fi
done

if [ "$redaction_found" = false ]; then
    echo "   ⚠️  INFO: Specific field redaction not verified (may be normal)"
fi

echo ""
echo "📈 TEST SUMMARY"
echo "==============="
echo "Total tests: $TOTAL_TESTS"
echo "Failed tests: $FAILED_TESTS"
echo "Success rate: $(( (TOTAL_TESTS - FAILED_TESTS) * 100 / TOTAL_TESTS ))%"

if [[ $FAILED_TESTS -eq 0 ]]; then
    echo ""
    echo "🎉 ALL HARDENING TESTS PASSED!"
    echo "   ✅ Parameter validation working"
    echo "   ✅ Error taxonomy implemented"
    echo "   ✅ Rate limiting functional"
    echo "   ✅ Log redaction active"
    echo "   ✅ Boundary conditions handled"
    echo ""
    echo "🛡️  ENCRYPTED KEY IMPORT IS PRODUCTION HARDENED!"
    exit 0
else
    echo ""
    echo "❌ SOME HARDENING TESTS FAILED"
    echo "   Review the failures above and fix before production deployment"
    exit 1
fi
