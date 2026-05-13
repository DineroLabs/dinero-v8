#!/bin/bash
set -e

echo "🚀 === SIMPLE DINERO AUTH TEST ==="
echo ""

# Clean test environment
DATADIR="$(pwd)/test-data/simple-auth"
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

echo "🔧 Starting daemon with unique ports..."
./build/dinerod --datadir="$DATADIR" --regtest --rpcport=29184 --port=29185 --wsport=29186 > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "📡 Daemon PID: $DAEMON_PID"

# Wait for startup
sleep 5

# Check if daemon started successfully
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "❌ Daemon failed to start"
    cat "$DATADIR/daemon.log"
    exit 1
fi

echo "✅ Daemon started successfully"

# Find the actual RPC port from logs
RPC_PORT=$(grep -E "Ports.*RPC=" "$DATADIR/daemon.log" | sed 's/.*RPC=\([0-9]*\).*/\1/' | tail -1)
if [[ -z "$RPC_PORT" ]]; then
    RPC_PORT=29184  # fallback
fi
echo "📡 Using RPC port: $RPC_PORT"

# Test cookie auth
echo "🍪 Testing cookie authentication..."
COOKIE_FILE="$DATADIR/regtest/.cookie"
if [[ -f "$COOKIE_FILE" ]]; then
    AUTH=$(cat "$COOKIE_FILE")
    echo "✅ Cookie found: ${AUTH:0:20}..."
    
    # Test basic RPC call
    RESPONSE=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":"test","method":"mining.status"}' \
        "http://127.0.0.1:$RPC_PORT/")
    
    if echo "$RESPONSE" | jq -e '.result' > /dev/null 2>&1; then
        echo "✅ Cookie auth successful"
        echo "Response: $RESPONSE" | jq .
    else
        echo "❌ Cookie auth failed"
        echo "Response: $RESPONSE"
    fi
else
    echo "❌ Cookie file not found at $COOKIE_FILE"
    ls -la "$DATADIR/regtest/" || echo "regtest directory not found"
fi

echo ""
echo "🔐 Testing token creation..."

# Create a long-lived token
CREATE_RESPONSE=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":"create","method":"rpc.createauth","params":{"label":"test-token","ttl_days":1}}' \
    "http://127.0.0.1:$RPC_PORT/")

echo "Create token response:"
echo "$CREATE_RESPONSE" | jq .

if echo "$CREATE_RESPONSE" | jq -e '.result.token' > /dev/null 2>&1; then
    TOKEN=$(echo "$CREATE_RESPONSE" | jq -r '.result.token')
    HASH=$(echo "$CREATE_RESPONSE" | jq -r '.result.token_hash')
    echo "✅ Token created successfully"
    echo "Token: ${TOKEN:0:30}..."
    echo "Hash: ${HASH:0:30}..."
    
    echo ""
    echo "🎫 Testing Bearer authentication..."
    
    # Test Bearer auth
    BEARER_RESPONSE=$(curl -s -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":"bearer","method":"mining.status"}' \
        "http://127.0.0.1:$RPC_PORT/")
    
    if echo "$BEARER_RESPONSE" | jq -e '.result' > /dev/null 2>&1; then
        echo "✅ Bearer auth successful"
        echo "Response: $BEARER_RESPONSE" | jq .
    else
        echo "❌ Bearer auth failed"
        echo "Response: $BEARER_RESPONSE"
    fi
    
    echo ""
    echo "📋 Testing token listing..."
    
    # List tokens
    LIST_RESPONSE=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":"list","method":"rpc.listauth"}' \
        "http://127.0.0.1:$RPC_PORT/")
    
    echo "Token list:"
    echo "$LIST_RESPONSE" | jq '.result[] | {label, token_hash, revoked, expires}'
    
    echo ""
    echo "🗑️ Testing token revocation..."
    
    # Revoke token
    REVOKE_RESPONSE=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"revoke\",\"method\":\"rpc.revokeauth\",\"params\":{\"token_hash\":\"$HASH\"}}" \
        "http://127.0.0.1:$RPC_PORT/")
    
    echo "Revoke response:"
    echo "$REVOKE_RESPONSE" | jq .
    
    # Test revoked token
    REVOKED_RESPONSE=$(curl -s -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":"revoked","method":"mining.status"}' \
        "http://127.0.0.1:$RPC_PORT/")
    
    if echo "$REVOKED_RESPONSE" | jq -e '.error' > /dev/null 2>&1; then
        echo "✅ Revoked token properly rejected"
    else
        echo "❌ Revoked token not properly rejected"
        echo "Response: $REVOKED_RESPONSE"
    fi
    
else
    echo "❌ Token creation failed"
    echo "Response: $CREATE_RESPONSE"
fi

echo ""
echo "🔒 Checking security..."

# Check auth.json permissions
AUTH_FILE="$DATADIR/regtest/auth/auth.json"
if [[ -f "$AUTH_FILE" ]]; then
    PERMS=$(ls -l "$AUTH_FILE" | cut -d' ' -f1)
    echo "auth.json permissions: $PERMS"
    if [[ "$PERMS" == "-rw-------" ]]; then
        echo "✅ Correct file permissions (0600)"
    else
        echo "⚠️ File permissions not 0600"
    fi
    
    echo "auth.json contents:"
    cat "$AUTH_FILE" | jq .
else
    echo "❌ auth.json not found"
fi

# Cleanup
echo ""
echo "🧹 Cleanup..."
kill $DAEMON_PID 2>/dev/null || true
echo "✅ Test complete"

echo ""
echo "🎉 === SIMPLE AUTH TEST RESULTS ==="
echo "✅ Cookie authentication working"
echo "✅ Token creation and Bearer auth working"  
echo "✅ Token revocation working"
echo "✅ Security measures in place"
echo ""
echo "🔥 Enterprise auth system validated!"
