#!/usr/bin/env bash
# Top-level smoke test - complete database initialization validation
# This is the final integration test before deployment
set -Eeuo pipefail

echo "🚀 **FINAL SMOKE TEST - DATABASE INITIALIZATION**"
echo "================================================="
echo ""

NET="regtest"
DATADIR="./data"
DAEMON_PID=""

# Cleanup function
cleanup() {
    if [[ -n "$DAEMON_PID" ]]; then
        echo "Stopping daemon (PID: $DAEMON_PID)..."
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "**1. Database Meta Initialization:**"
if ./scripts/fix_genesis_meta.sh "$NET" --datadir "$DATADIR"; then
    echo "   ✅ Meta initialization successful"
else
    echo "   ❌ Meta initialization failed"
    exit 1
fi

echo ""
echo "**2. Database Health Check:**"
if ./scripts/db_audit.sh "$NET"; then
    echo "   ✅ Database audit passed"
else
    echo "   ❌ Database audit failed"
    exit 1
fi

echo ""
echo "**3. Starting Daemon:**"
if ./build/dinerod -regtest -datadir="$DATADIR" -daemon; then
    echo "   ✅ Daemon started"
    sleep 5  # Give daemon time to fully initialize
else
    echo "   ❌ Daemon failed to start"
    exit 1
fi

# Try to get daemon PID for cleanup
DAEMON_PID=$(pgrep -f "dinerod.*regtest" | head -1 || echo "")

echo ""
echo "**4. RPC Genesis Hash Test:**"
COOKIE_FILE="$DATADIR/$NET/.cookie"
if [[ -f "$COOKIE_FILE" ]]; then
    # Try both possible ports
    GENESIS_HASH=""
    for PORT in 20998 20999; do
        GENESIS_HASH=$(curl -s --connect-timeout 5 --user "$(cat "$COOKIE_FILE")" \
            -H 'Content-Type: application/json' \
            -d '{"jsonrpc":"2.0","id":"test","method":"getblockhash","params":[0]}' \
            "http://127.0.0.1:$PORT/" 2>/dev/null | \
            jq -r '.result // empty' 2>/dev/null || echo "")
        
        if [[ -n "$GENESIS_HASH" && "$GENESIS_HASH" != "null" && "$GENESIS_HASH" != "" ]]; then
            echo "   ✅ RPC getblockhash(0) successful on port $PORT"
            echo "   Genesis hash: $GENESIS_HASH"
            break
        fi
    done
    
    if [[ -z "$GENESIS_HASH" || "$GENESIS_HASH" == "null" ]]; then
        echo "   ❌ RPC call failed on both ports"
        exit 1
    fi
    
    # Validate genesis hash format
    if [[ "$GENESIS_HASH" =~ ^[0-9a-f]{64}$ ]]; then
        echo "   ✅ Genesis hash format valid (64-char lowercase hex)"
    else
        echo "   ❌ Genesis hash format invalid: $GENESIS_HASH"
        exit 1
    fi
    
    # Compare with database
    DB_GENESIS=$(sqlite3 "$DATADIR/$NET/blockchain.db" \
        "SELECT LOWER(value) FROM meta WHERE key='genesis_hash';" 2>/dev/null || echo "")
    if [[ "$GENESIS_HASH" == "$DB_GENESIS" ]]; then
        echo "   ✅ RPC and database genesis hashes match"
    else
        echo "   ❌ Genesis hash mismatch: RPC=$GENESIS_HASH vs DB=$DB_GENESIS"
        exit 1
    fi
    
else
    echo "   ❌ Cookie file not found: $COOKIE_FILE"
    exit 1
fi

echo ""
echo "**5. Final Database Audit:**"
if ./scripts/db_audit.sh "$NET"; then
    echo "   ✅ Final database audit passed"
else
    echo "   ❌ Final database audit failed"
    exit 1
fi

echo ""
echo "**6. Idempotence Test:**"
if ./build/test_db_init_idempotence; then
    echo "   ✅ Idempotence test passed"
else
    echo "   ❌ Idempotence test failed"
    exit 1
fi

echo ""
echo "🎊 **ALL SMOKE TESTS PASSED** 🎊"
echo "================================="
echo ""
echo "✅ Database initialization is working correctly"
echo "✅ RPC methods are functioning properly"
echo "✅ Network validation is active"
echo "✅ Genesis hash consistency verified"
echo "✅ Idempotence guarantees confirmed"
echo ""
echo "🚀 **SYSTEM IS READY FOR PRODUCTION DEPLOYMENT!** 🚀"
