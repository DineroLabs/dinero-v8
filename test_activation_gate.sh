#!/bin/bash
# Descriptor Activation Gate Test Suite
# Tests Phase 2 policy enforcement

COOKIE=$(cat ~/.dinero/.cookie)
RPC="http://127.0.0.1:20998"

echo "========================================="
echo "Descriptor Activation Gate Test Suite"
echo "========================================="
echo ""
echo "Wallet Policy: BIP86"
echo "Master Fingerprint: f802fb0e"
echo ""

# Helper function to test descriptor import
test_import() {
    local test_name="$1"
    local desc="$2"
    local active="$3"
    local expected_result="$4"  # "pass" or "fail"

    echo "----------------------------------------"
    echo "TEST: $test_name"
    echo "Descriptor: ${desc:0:80}..."
    echo "Active: $active"
    echo -n "Expected: $expected_result ... "

    result=$(curl -s --user "$COOKIE" --data-binary "{
        \"jsonrpc\":\"2.0\",
        \"method\":\"wallet.importdescriptors\",
        \"params\":[{
            \"desc\":\"$desc\",
            \"active\":$active,
            \"internal\":false,
            \"range\":[0,2],
            \"timestamp\":\"now\",
            \"label\":\"test_$(date +%s)\"
        }],
        \"id\":1
    }" "$RPC")

    success=$(echo "$result" | grep -o '"success"[[:space:]]*:[[:space:]]*true')
    error=$(echo "$result" | grep -o '"error"')

    if [ "$expected_result" == "pass" ]; then
        if [ -n "$success" ]; then
            echo "✅ PASS"
            echo "$result" | python3 -m json.tool 2>/dev/null | grep -A 2 "result"
        else
            echo "❌ FAIL (expected pass, got failure)"
            echo "$result" | python3 -m json.tool 2>/dev/null | grep -A 5 "error"
        fi
    else
        if [ -n "$error" ] || [ -z "$success" ]; then
            echo "✅ PASS (correctly rejected)"
            echo "$result" | python3 -m json.tool 2>/dev/null | grep -A 5 '"message"'
        else
            echo "❌ FAIL (expected rejection, got success)"
            echo "$result" | python3 -m json.tool 2>/dev/null
        fi
    fi
    echo ""
}

# TEST 1: Valid BIP86 descriptor with different fingerprint (watch-only external monitoring)
test_import \
    "Valid BIP86 (tr) descriptor in BIP86 wallet (watch-only external)" \
    "tr([12345678/86h/0h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)#vrr75364" \
    "false" \
    "pass"

# TEST 2: Invalid - BIP84 (wpkh) descriptor in BIP86 wallet
test_import \
    "Invalid: BIP84 (wpkh) descriptor in BIP86 wallet" \
    "wpkh([f802fb0e/84h/1447h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)#l9tcra4t" \
    "true" \
    "fail"

# TEST 3: Invalid - BIP86 descriptor with wrong path (84h instead of 86h)
test_import \
    "Invalid: BIP86 descriptor with 84h path in BIP86 wallet" \
    "tr([f802fb0e/84h/1447h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)#p0cp894m" \
    "true" \
    "fail"

# TEST 4: Invalid - Wrong fingerprint (should reject for active=true internal)
test_import \
    "Invalid: Wrong fingerprint for active descriptor" \
    "tr([abcd1234/86h/1447h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)#phutwpwj" \
    "true" \
    "fail"

# TEST 5: Valid - Watch-only BIP84 descriptor (active=false bypasses policy)
test_import \
    "Valid: Watch-only BIP84 descriptor (active=false)" \
    "wpkh([ae6a04bd/84h/0h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/0/*)#49n7vhn8" \
    "false" \
    "pass"

# TEST 6: Valid - Correct BIP86 descriptor with external fingerprint (active=true, hardware wallet)
test_import \
    "Valid: BIP86 descriptor with external HW wallet fingerprint (active=true)" \
    "tr([ae6a04bd/86h/0h/0h]xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8/1/*)#nuflxj65" \
    "true" \
    "pass"

echo "========================================="
echo "Test Suite Complete"
echo "========================================="
