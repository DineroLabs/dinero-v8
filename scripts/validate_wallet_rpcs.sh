#!/usr/bin/env bash
set -euo pipefail

# Quick RPC method validation script
# Tests if all required wallet RPC methods are available

PORT="${PORT:-20996}"
COOKIE_PATH="${DINERO_COOKIE_FILE:-$HOME/.dinero/regtest/.cookie}"

if [[ ! -f "$COOKIE_PATH" ]]; then
    echo "❌ Cookie not found at $COOKIE_PATH"
    echo "   Make sure daemon is running with: ./build/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinerod"
    exit 1
fi

COOKIE_VAL=$(cat "$COOKIE_PATH")
echo "🔍 Testing wallet RPC methods on port $PORT"

test_method() {
    local method="$1"
    local params="${2:-[]}"
    local response
    
    response=$(curl -sS --max-time 3 -u "$COOKIE_VAL" \
        -H 'content-type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"$method\",\"params\":$params}" \
        "http://127.0.0.1:$PORT/" 2>/dev/null) || {
        echo "❌ $method - Connection failed"
        return 1
    }
    
    if echo "$response" | jq -e '.error' >/dev/null 2>&1; then
        local error_msg
        error_msg=$(echo "$response" | jq -r '.error.message // "Unknown error"')
        if [[ "$error_msg" == *"Method not found"* ]]; then
            echo "❌ $method - Method not found"
            return 1
        else
            echo "⚠️  $method - Available (error: $error_msg)"
            return 0
        fi
    else
        echo "✅ $method - Available"
        return 0
    fi
}

echo ""
echo "Core wallet methods:"
test_method "getwalletinfo"
test_method "getbalance"
test_method "getnewaddress"
test_method "validateaddress" '["rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"]'
test_method "listtransactions"

echo ""
echo "Extended wallet methods:"
test_method "wallet.encrypt" '{"passphrase":"test"}'
test_method "wallet.unlock" '{"passphrase":"test","timeout":60}'
test_method "wallet.lock"
test_method "wallet.backup" '{"path":"/tmp/test-backup.dat"}'
test_method "wallet.restore" '{"path":"/tmp/test-backup.dat","overwrite":false}'
test_method "wallet.rescan" '{"from_time":1640995200}'

echo ""
echo "Import methods:"
test_method "wallet.import" '{"type":"wif","wif":"test","rescan":false}'
test_method "wallet.vault.import" '{"blob":"test","passphrase":"test","rescan":false}'

echo ""
echo "Network methods:"
test_method "getnetworkinfo"
test_method "getblockchaininfo"

echo ""
echo "🎯 RPC validation complete!"
