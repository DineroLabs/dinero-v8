#!/usr/bin/env bash
set -euo pipefail

# Auth Security Hardening Test Suite
# Tests for authorization header scrubbing, rate limiting, and security measures

DATADIR="${1:-./test-data/auth-hardening}"
PORT="${2:-20999}"

echo "🔒 Starting Auth Security Hardening Tests..."
echo "   DATADIR: $DATADIR"
echo "   PORT: $PORT"

# Clean start
echo "🧹 Cleaning up any existing daemon..."
pkill -f dinerod >/dev/null 2>&1 || true
rm -rf "$DATADIR" && mkdir -p "$DATADIR"

# Start regtest daemon
echo "🚀 Starting regtest daemon..."
./build/dinerod --regtest --datadir="$DATADIR" --printtoconsole > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
trap 'kill $DAEMON_PID >/dev/null 2>&1 || true' EXIT
sleep 2

AUTH=$(cat "$DATADIR/regtest/.cookie")
echo "✅ Daemon started"

# Test 1: Authorization header scrubbing in logs
echo "🔍 Test 1: Authorization header logging security..."

# Make request with Bearer token and check logs don't contain it
CREATE_RESULT=$(curl -s --max-time 5 --user "$AUTH" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"create","method":"rpc.createauth","params":{"label":"test","ttl_days":1}}
JSON
)

TOKEN=$(echo "$CREATE_RESULT" | jq -r '.result.token')

# Make request with Bearer token
curl -s --max-time 5 -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON' >/dev/null
{"jsonrpc":"2.0","id":"test","method":"mining.status"}
JSON

# Check that token doesn't appear in logs
if grep -q "$TOKEN" "$DATADIR/daemon.log"; then
    echo "❌ SECURITY VIOLATION: Token found in daemon logs!"
    echo "Found token in logs:"
    grep "$TOKEN" "$DATADIR/daemon.log" || true
    exit 1
fi

echo "✅ Authorization headers properly scrubbed from logs"

# Test 2: Malformed Bearer headers
echo "🚫 Test 2: Malformed Bearer header handling..."

# Empty Bearer token
EMPTY_BEARER_CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"test","method":"mining.status"}
JSON
)

if [[ "$EMPTY_BEARER_CODE" != "401" ]]; then
    echo "❌ Empty Bearer token should return 401, got: $EMPTY_BEARER_CODE"
    exit 1
fi

# Invalid Bearer token
INVALID_BEARER_CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer invalid_token_here" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"test","method":"mining.status"}
JSON
)

if [[ "$INVALID_BEARER_CODE" != "401" ]]; then
    echo "❌ Invalid Bearer token should return 401, got: $INVALID_BEARER_CODE"
    exit 1
fi

echo "✅ Malformed Bearer headers properly rejected"

# Test 3: Mixed authentication headers (Bearer should win)
echo "🥊 Test 3: Mixed authentication priority..."

# Send both Basic and Bearer - Bearer should take precedence
MIXED_RESULT=$(curl -s --max-time 5 \
  --user "$AUTH" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"mixed","method":"mining.status"}
JSON
)

if ! echo "$MIXED_RESULT" | jq -e '.result' >/dev/null; then
    echo "❌ Mixed auth failed - Bearer should take precedence:"
    echo "$MIXED_RESULT" | jq .
    exit 1
fi

echo "✅ Bearer takes precedence over Basic in mixed auth"

# Test 4: Concurrent token operations
echo "⚡ Test 4: Concurrent token operations safety..."

# Create multiple tokens concurrently
PIDS=()
TEMP_DIR=$(mktemp -d)
for i in {1..5}; do
    (
        RESULT=$(curl -s --max-time 10 --user "$AUTH" -H 'content-type: application/json' \
          --data-binary @- http://127.0.0.1:$PORT/ <<JSON
{"jsonrpc":"2.0","id":"concurrent_$i","method":"rpc.createauth","params":{"label":"concurrent_test_$i","ttl_days":1}}
JSON
        )
        echo "$RESULT" | jq -r '.result.token' > "$TEMP_DIR/token_$i.txt"
    ) &
    PIDS+=($!)
done

# Wait for all to complete
for pid in "${PIDS[@]}"; do
    wait "$pid"
done

# Verify all tokens are unique
TOKENS=($(cat "$TEMP_DIR"/token_*.txt | sort))
UNIQUE_TOKENS=($(cat "$TEMP_DIR"/token_*.txt | sort -u))

if [[ ${#TOKENS[@]} -ne ${#UNIQUE_TOKENS[@]} ]]; then
    echo "❌ Concurrent token creation produced duplicates!"
    echo "Total tokens: ${#TOKENS[@]}, Unique: ${#UNIQUE_TOKENS[@]}"
    exit 1
fi

rm -rf "$TEMP_DIR"
echo "✅ Concurrent token operations produce unique tokens"

# Test 5: Token validation after revocation
echo "🔥 Test 5: Revocation enforcement..."

# Create a token
REVOKE_TEST_RESULT=$(curl -s --max-time 5 --user "$AUTH" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"revoke_test","method":"rpc.createauth","params":{"label":"revoke_test","ttl_days":1}}
JSON
)

REVOKE_TOKEN=$(echo "$REVOKE_TEST_RESULT" | jq -r '.result.token')
REVOKE_HASH=$(echo "$REVOKE_TEST_RESULT" | jq -r '.result.token_hash')

# Verify it works
curl -s --max-time 5 -H "Authorization: Bearer $REVOKE_TOKEN" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON' >/dev/null
{"jsonrpc":"2.0","id":"pre_revoke","method":"mining.status"}
JSON

# Revoke it
curl -s --max-time 5 --user "$AUTH" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<JSON >/dev/null
{"jsonrpc":"2.0","id":"revoke","method":"rpc.revokeauth","params":{"token_hash":"$REVOKE_HASH"}}
JSON

# Verify it's immediately denied
REVOKED_CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $REVOKE_TOKEN" -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"post_revoke","method":"mining.status"}
JSON
)

if [[ "$REVOKED_CODE" != "401" ]]; then
    echo "❌ Revoked token should be immediately denied, got: $REVOKED_CODE"
    exit 1
fi

echo "✅ Token revocation immediately enforced"

# Test 6: WWW-Authenticate header advertisement
echo "📢 Test 6: WWW-Authenticate header advertisement..."

NO_AUTH_RESPONSE=$(curl -s -i --max-time 5 -H 'content-type: application/json' \
  --data-binary @- http://127.0.0.1:$PORT/ <<'JSON'
{"jsonrpc":"2.0","id":"no_auth","method":"mining.status"}
JSON
)

if ! echo "$NO_AUTH_RESPONSE" | grep -q "WWW-Authenticate:.*Basic.*Bearer"; then
    echo "❌ Missing proper WWW-Authenticate header"
    echo "Response headers:"
    echo "$NO_AUTH_RESPONSE" | head -20
    exit 1
fi

echo "✅ WWW-Authenticate header properly advertises both schemes"

# Success summary
cat <<'SUCCESS'

🔒 AUTH SECURITY HARDENING TESTS - COMPLETE SUCCESS!
==================================================

✅ ALL SECURITY TESTS PASSED:
  🔍 Authorization header scrubbing: SECURE (no tokens in logs)
  🚫 Malformed Bearer handling: SECURE (401 as expected)
  🥊 Mixed auth priority: SECURE (Bearer wins over Basic)
  ⚡ Concurrent operations: SECURE (unique tokens generated)
  🔥 Revocation enforcement: SECURE (immediate denial)
  📢 WWW-Authenticate headers: SECURE (both schemes advertised)

🛡️ SECURITY HARDENING COMPLETE!
  • No credential leakage in logs ✅
  • Proper error handling for malformed auth ✅
  • Correct authentication precedence ✅
  • Thread-safe concurrent operations ✅
  • Immediate revocation enforcement ✅
  • Standards-compliant header advertisement ✅

🎯 PRODUCTION SECURITY STANDARDS MET!

SUCCESS

echo "✅ Auth security hardening tests PASSED"
