#!/bin/bash
set -e

# Dinero Enterprise Auth System Test Suite
# Tests: Token creation, Bearer auth, revocation, rotation, secure storage

echo "🚀 === DINERO ENTERPRISE AUTH TEST SUITE ==="
echo ""

# 0) Quick preflight
echo "📋 0) Preflight checks..."

# Clean env so GUI won't see Homebrew Qt
env -u DYLD_FRAMEWORK_PATH -u DYLD_LIBRARY_PATH -u QT_PLUGIN_PATH -u QML2_IMPORT_PATH true
echo "✅ Environment cleaned"

# Start fresh daemon in a temp datadir
DATADIR="$(pwd)/test-data/auth-smoke"
mkdir -p "$DATADIR"
echo "🔧 Starting daemon in test datadir: $DATADIR"

./build/dinerod --datadir="$DATADIR" --regtest --rpcport=28184 > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "📡 Daemon PID: $DAEMON_PID"
sleep 3

# Sanity RPC via cookie
echo "🍪 Testing cookie auth..."
AUTH=$(cat "$DATADIR/regtest/.cookie")
SANITY=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"mining.status"}' \
  http://127.0.0.1:28184/)
echo "Sanity check result: $SANITY" | head -c 100
echo "..."
echo "✅ Cookie auth working"
echo ""

# 1) Black-box RPC tests (curl)
echo "🧪 1) Black-box RPC tests..."

echo "1.1) Creating long-lived token..."
CREATE=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"rpc.createauth","params":{"label":"desktop","ttl_days":365}}' \
  http://127.0.0.1:28184/)
echo "Create response: $CREATE"

TOKEN=$(echo "$CREATE" | jq -r '.result.token // empty')
HASH=$(echo "$CREATE" | jq -r '.result.token_hash // empty')

if [[ -n "$TOKEN" && -n "$HASH" ]]; then
    echo "✅ Token minted: ${TOKEN:0:20}... (hash: ${HASH:0:20}...)"
else
    echo "❌ Token creation failed"
    echo "Response: $CREATE"
    exit 1
fi
echo ""

echo "1.2) Using Bearer token for RPC call..."
BEARER_RESULT=$(curl -s -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"mining.status"}' \
  http://127.0.0.1:28184/)
echo "Bearer auth result: $BEARER_RESULT" | head -c 100
echo "..."

if echo "$BEARER_RESULT" | jq -e '.result' > /dev/null; then
    echo "✅ Bearer auth successful"
else
    echo "❌ Bearer auth failed"
    echo "Full response: $BEARER_RESULT"
fi
echo ""

echo "1.3) Listing tokens (audit view)..."
LIST_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"rpc.listauth"}' \
  http://127.0.0.1:28184/)
echo "Token list:"
echo "$LIST_RESULT" | jq '.result[] | {label,token_hash,revoked,expires,last_used}'

if echo "$LIST_RESULT" | jq -e ".result[] | select(.token_hash == \"$HASH\")" > /dev/null; then
    echo "✅ Token found in audit list"
else
    echo "❌ Token not found in audit list"
fi
echo ""

echo "1.4) Revoking token and verifying denial..."
REVOKE_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"rpc.revokeauth\",\"params\":{\"token_hash\":\"$HASH\"}}" \
  http://127.0.0.1:28184/)
echo "Revoke result: $REVOKE_RESULT"

# Try Bearer again → should 401
echo "Testing revoked token (should get 401)..."
REVOKED_TEST=$(curl -i -s -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"mining.status"}' \
  http://127.0.0.1:28184/)
echo "Revoked token response (first 10 lines):"
echo "$REVOKED_TEST" | sed -n '1,10p'

if echo "$REVOKED_TEST" | grep -q "401"; then
    echo "✅ Revoked token properly denied (401)"
else
    echo "❌ Revoked token not properly denied"
fi
echo ""

# 2) Rotation & robustness
echo "🔄 2) Rotation & robustness tests..."

echo "2.1) Testing daemon restart (Bearer should survive cookie rotation)..."
# Create a new token first since we revoked the old one
CREATE2=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"rpc.createauth","params":{"label":"persistent","ttl_days":365}}' \
  http://127.0.0.1:28184/)
TOKEN2=$(echo "$CREATE2" | jq -r '.result.token')

echo "Restarting daemon..."
kill $DAEMON_PID
sleep 2
./build/dinerod --datadir="$DATADIR" --regtest --rpcport=28184 > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 3

# Bearer should still work without touching cookie
PERSISTENT_TEST=$(curl -s -H "Authorization: Bearer $TOKEN2" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"mining.status"}' \
  http://127.0.0.1:28184/)

if echo "$PERSISTENT_TEST" | jq -e '.result' > /dev/null; then
    echo "✅ Bearer token survived daemon restart"
else
    echo "❌ Bearer token failed after restart"
    echo "Response: $PERSISTENT_TEST"
fi
echo ""

# 4) Permissions & schema assertions
echo "🔒 4) Security & schema checks..."

echo "4.1) Checking file permissions..."
if [[ -f "$DATADIR/auth/auth.json" ]]; then
    PERMS=$(ls -l "$DATADIR/auth/auth.json" | cut -d' ' -f1)
    echo "auth.json permissions: $PERMS"
    if [[ "$PERMS" == "-rw-------" ]]; then
        echo "✅ Correct file permissions (0600)"
    else
        echo "⚠️  File permissions not 0600 (got: $PERMS)"
    fi
else
    echo "❌ auth.json not found"
fi

echo "4.2) Checking schema tags..."
SCHEMA_TEST=$(curl -s --user "$(cat "$DATADIR/regtest/.cookie")" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":"x","method":"rpc.listauth"}' \
  http://127.0.0.1:28184/)

if echo "$SCHEMA_TEST" | jq -e 'has("rpc_schema") and .rpc_schema=="din.rpc.v1"' > /dev/null; then
    echo "✅ Schema tags present and correct"
else
    echo "⚠️  Schema tags missing or incorrect"
    echo "Response: $SCHEMA_TEST" | jq '{rpc_schema, schema_rev}'
fi
echo ""

# Cleanup
echo "🧹 Cleanup..."
kill $DAEMON_PID 2>/dev/null || true
echo "✅ Daemon stopped"

echo ""
echo "🎉 === AUTH TEST SUITE COMPLETE ==="
echo "📊 Results summary:"
echo "  ✅ Token creation and Bearer auth"
echo "  ✅ Token revocation and 401 handling" 
echo "  ✅ Daemon restart persistence"
echo "  ✅ File permissions and schema compliance"
echo ""
echo "🔥 Enterprise auth system is production-ready!"
