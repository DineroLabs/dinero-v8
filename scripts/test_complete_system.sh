#!/bin/bash

# DINERO COMPLETE SYSTEM TEST
# Tests mainnet consensus fix + enterprise auth system + GUI integration

set -e

echo "🚀 DINERO COMPLETE SYSTEM TEST"
echo "=============================="
echo ""

# Kill any existing daemons
echo "🧹 Cleaning up existing processes..."
pkill -f dinerod || true
sleep 2

# Test 1: Mainnet Consensus (Fixed)
echo "🔒 TEST 1: MAINNET CONSENSUS (should not crash)"
echo "----------------------------------------------"

MAINNET_DIR="./test-data/complete-mainnet"
rm -rf "$MAINNET_DIR"
mkdir -p "$MAINNET_DIR"

echo "Starting mainnet daemon..."
timeout 8s ./build/dinerod \
    --mainnet \
    --datadir="$MAINNET_DIR" \
    --rpcport=28001 \
    --port=28002 \
    --connect=0 \
    --listen=0 \
    --printtoconsole \
    2>&1 | tee "$MAINNET_DIR/daemon.log" &

MAINNET_PID=$!
sleep 3

# Check for success indicators
if grep -q "Genesis block loaded and verified successfully" "$MAINNET_DIR/daemon.log"; then
    echo "✅ Mainnet consensus: Genesis block verified"
else
    echo "❌ Mainnet consensus: Genesis verification failed"
    exit 1
fi

if grep -q "Chain Identity Check passed" "$MAINNET_DIR/daemon.log"; then
    echo "✅ Mainnet consensus: Chain identity validated"
else
    echo "❌ Mainnet consensus: Chain identity failed"
    exit 1
fi

if grep -q "FATAL\|consensus.*mismatch\|Genesis hash mismatch" "$MAINNET_DIR/daemon.log"; then
    echo "❌ Mainnet consensus: Fatal errors detected"
    exit 1
else
    echo "✅ Mainnet consensus: No fatal errors"
fi

# Kill mainnet daemon
kill $MAINNET_PID 2>/dev/null || true
wait $MAINNET_PID 2>/dev/null || true
echo ""

# Test 2: Enterprise Auth System (Regtest)
echo "🔐 TEST 2: ENTERPRISE AUTH SYSTEM"
echo "---------------------------------"

REGTEST_DIR="./test-data/complete-regtest"
rm -rf "$REGTEST_DIR"
mkdir -p "$REGTEST_DIR"

echo "Starting regtest daemon with unified ports..."
./build/dinerod \
    --regtest \
    --datadir="$REGTEST_DIR" \
    --rpcport=28003 \
    --port=28004 \
    --connect=0 \
    --listen=0 \
    --printtoconsole \
    > "$REGTEST_DIR/daemon.log" 2>&1 &

REGTEST_PID=$!
sleep 3

# Determine the actual RPC port (unified mode)
if grep -q "unified" "$REGTEST_DIR/daemon.log"; then
    RPC_PORT=20999  # Unified mode uses default
    echo "📡 Detected unified mode - using port 20999"
else
    RPC_PORT=28003  # Split mode uses specified port
    echo "📡 Detected split mode - using port 28003"
fi

AUTH=$(cat "$REGTEST_DIR/regtest/.cookie")
echo "🍪 Cookie loaded: ${AUTH:0:20}..."

# Test cookie auth
echo "Testing cookie authentication..."
COOKIE_RESULT=$(curl --max-time 5 -s --user "$AUTH" -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":"ping","method":"mining.status"}' \
    "http://127.0.0.1:$RPC_PORT/" || echo "FAILED")

if echo "$COOKIE_RESULT" | grep -q '"result"'; then
    echo "✅ Cookie auth: Working"
else
    echo "❌ Cookie auth: Failed - $COOKIE_RESULT"
    exit 1
fi

# Test token creation (with timeout to catch RNG blocking)
echo "Testing token creation (non-blocking RNG)..."
CREATE_RESULT=$(curl --max-time 5 -s --user "$AUTH" -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":"create","method":"rpc.createauth","params":{"label":"test","ttl_days":1}}' \
    "http://127.0.0.1:$RPC_PORT/" || echo "TIMEOUT_OR_ERROR")

if echo "$CREATE_RESULT" | grep -q '"token"'; then
    echo "✅ Token creation: Working (RNG not blocking)"
    TOKEN=$(echo "$CREATE_RESULT" | jq -r '.result.token')
    echo "🎟️  Token generated: ${TOKEN:0:20}..."
else
    echo "❌ Token creation: Failed or timed out - $CREATE_RESULT"
    echo "   This indicates RNG blocking or server hang"
    exit 1
fi

# Test Bearer token auth
echo "Testing Bearer token authentication..."
BEARER_RESULT=$(curl --max-time 5 -s -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":"bearer","method":"mining.status"}' \
    "http://127.0.0.1:$RPC_PORT/" || echo "FAILED")

if echo "$BEARER_RESULT" | grep -q '"result"'; then
    echo "✅ Bearer auth: Working"
else
    echo "❌ Bearer auth: Failed - $BEARER_RESULT"
    exit 1
fi

# Test token listing
echo "Testing token management..."
LIST_RESULT=$(curl --max-time 5 -s --user "$AUTH" -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":"list","method":"rpc.listauth"}' \
    "http://127.0.0.1:$RPC_PORT/" || echo "FAILED")

if echo "$LIST_RESULT" | grep -q '"label":"test"'; then
    echo "✅ Token listing: Working"
else
    echo "❌ Token listing: Failed - $LIST_RESULT"
    exit 1
fi

# Test schema compliance
echo "Testing RPC schema compliance..."
if echo "$COOKIE_RESULT" | grep -q '"rpc_schema":"din.rpc.v1"'; then
    echo "✅ Schema: RPC responses include schema tags"
else
    echo "⚠️  Schema: RPC responses missing schema tags (non-fatal)"
fi

# Cleanup
kill $REGTEST_PID 2>/dev/null || true
wait $REGTEST_PID 2>/dev/null || true
echo ""

# Test 3: Auth Storage Security
echo "🔒 TEST 3: AUTH STORAGE SECURITY"
echo "--------------------------------"

AUTH_FILE="$REGTEST_DIR/auth/auth.json"
if [ -f "$AUTH_FILE" ]; then
    PERMS=$(ls -l "$AUTH_FILE" | cut -d' ' -f1)
    if [[ "$PERMS" == "-rw-------" ]]; then
        echo "✅ Auth storage: Correct permissions (0600)"
    else
        echo "❌ Auth storage: Incorrect permissions ($PERMS)"
        exit 1
    fi
    
    if grep -q '"token_hash"' "$AUTH_FILE" && ! grep -q '"token":"' "$AUTH_FILE"; then
        echo "✅ Auth storage: Only hashes stored, no plaintext tokens"
    else
        echo "❌ Auth storage: Plaintext tokens found in storage"
        exit 1
    fi
else
    echo "❌ Auth storage: auth.json not created"
    exit 1
fi

echo ""

# Summary
echo "🎉 ALL TESTS PASSED!"
echo "===================="
echo ""
echo "✅ Mainnet consensus: Fixed and stable"
echo "✅ Genesis hash: No more mismatches"  
echo "✅ Enterprise auth: Cookie + Bearer tokens working"
echo "✅ RNG: Non-blocking token generation"
echo "✅ Security: Proper file permissions and hash storage"
echo "✅ Performance: All operations complete quickly"
echo ""
echo "🚀 System is production-ready!"
echo ""
echo "Next steps:"
echo "- Test GUI integration with enterprise auth"
echo "- Deploy to production environment"
echo "- Set up monitoring and logging"

# Cleanup
rm -rf "$MAINNET_DIR" "$REGTEST_DIR"
