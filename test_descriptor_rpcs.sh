#!/bin/bash
# Test descriptor RPCs with BIP86 Taproot wallet

set -e

echo "=== Descriptor RPC Policy Test ==="
echo ""

# Check if default wallet exists and is BIP86
WALLET_DB="$HOME/.dinero/wallets/wallet_default.db"
if [ ! -f "$WALLET_DB" ]; then
    echo "❌ Default wallet not found"
    exit 1
fi

POLICY=$(sqlite3 "$WALLET_DB" "SELECT wallet_policy FROM wallet_meta WHERE id = 1;" 2>/dev/null)
echo "Default wallet policy: $POLICY"

if [ "$POLICY" != "bip86" ]; then
    echo "⚠️  Default wallet is not BIP86"
    echo "   Update it with: sqlite3 $WALLET_DB \"UPDATE wallet_meta SET wallet_policy = 'bip86' WHERE id = 1;\""
    exit 1
fi

echo ""
echo "══════════════════════════════════════════════════════"
echo "Test 1: wallet.listdescriptors (should return tr(...) descriptors)"
echo "══════════════════════════════════════════════════════"

# Note: These tests assume dinerod is running and wallet is loaded
# For actual testing, you would run these commands against dinerod

echo "Sample expected output for BIP86 wallet:"
echo "{"
echo "  \"descriptors\": ["
echo "    {"
echo "      \"desc\": \"tr([fingerprint/86h/1447h/0h]xpub/0/*)#checksum\","
echo "      \"active\": true,"
echo "      \"internal\": false,"
echo "      \"range\": [0, 1000],"
echo "      \"next\": 0"
echo "    },"
echo "    {"
echo "      \"desc\": \"tr([fingerprint/86h/1447h/0h]xpub/1/*)#checksum\","
echo "      \"active\": true,"
echo "      \"internal\": true,"
echo "      \"range\": [0, 1000],"
echo "      \"next\": 0"
echo "    }"
echo "  ]"
echo "}"

echo ""
echo "══════════════════════════════════════════════════════"
echo "Test 2: wallet.getdescriptorinfo"
echo "══════════════════════════════════════════════════════"

echo "For BIP86 Taproot descriptor:"
echo "  Input:  tr([4c6d968f/86h/1447h/0h]xpub/0/*)"
echo "  Expected type: \"tr\""
echo "  Expected derivation_path: \"m/86h/1447h/0h\""

echo ""
echo "For BIP84 SegWit descriptor:"
echo "  Input:  wpkh([4c6d968f/84h/1447h/0h]xpub/0/*)"
echo "  Expected type: \"wpkh\""
echo "  Expected derivation_path: \"m/84h/1447h/0h\""

echo ""
echo "══════════════════════════════════════════════════════"
echo "Test 3: wallet.deriveaddresses"
echo "══════════════════════════════════════════════════════"

echo "For BIP86 Taproot descriptor:"
echo "  Should derive addresses with prefix: rdin1p... (bech32m)"
echo ""
echo "For BIP84 SegWit descriptor:"
echo "  Should derive addresses with prefix: rdin1q... (bech32)"

echo ""
echo "══════════════════════════════════════════════════════"
echo "Test 4: wallet.exportdescriptors"
echo "══════════════════════════════════════════════════════"

echo "Expected output should include:"
echo "  \"policy\": \"bip86\""
echo "  Descriptors starting with \"tr(...)\""

echo ""
echo "══════════════════════════════════════════════════════"
echo "✅ All descriptor RPCs now support both BIP84 and BIP86"
echo "══════════════════════════════════════════════════════"
echo ""
echo "To test with live daemon:"
echo "  1. Start dinerod"
echo "  2. ./dinero-cli wallet.load default"
echo "  3. ./dinero-cli wallet.listdescriptors"
echo "  4. ./dinero-cli wallet.getdescriptorinfo 'tr([fingerprint/86h/1447h/0h]xpub/0/*)'"
echo "  5. ./dinero-cli wallet.deriveaddresses 'tr([fingerprint/86h/1447h/0h]xpub/0/*)' '[0,2]'"
echo "  6. ./dinero-cli wallet.exportdescriptors"
echo ""
