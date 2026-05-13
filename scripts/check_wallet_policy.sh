#!/bin/bash
# Check wallet policy for a given wallet

WALLET_NAME=${1:-default}
WALLET_DB="$HOME/.dinero/wallets/wallet_${WALLET_NAME}.db"

if [ ! -f "$WALLET_DB" ]; then
    echo "❌ Wallet not found: $WALLET_NAME"
    echo "   Expected: $WALLET_DB"
    exit 1
fi

echo "=== Wallet Policy Check ==="
echo "Wallet: $WALLET_NAME"
echo ""

# Get wallet metadata
sqlite3 "$WALLET_DB" "SELECT
    'Name: ' || name || char(10) ||
    'Network: ' || network || char(10) ||
    'Policy: ' || wallet_policy || char(10) ||
    'Created: ' || datetime(created_at, 'unixepoch')
FROM wallet_meta WHERE id = 1;" 2>/dev/null

POLICY=$(sqlite3 "$WALLET_DB" "SELECT wallet_policy FROM wallet_meta WHERE id = 1;" 2>/dev/null)

echo ""
if [ "$POLICY" = "bip86" ]; then
    echo "✅ Taproot (BIP86) - Maximum privacy"
    echo "   Derivation: m/86'/1447'/0'"
    echo "   Addresses: rdin1p... (bech32m)"
elif [ "$POLICY" = "bip84" ]; then
    echo "✅ SegWit (BIP84) - Maximum compatibility"
    echo "   Derivation: m/84'/1447'/0'"
    echo "   Addresses: rdin1q... (bech32)"
else
    echo "⚠️  Unknown policy: $POLICY"
fi

# Check if there are any addresses
ADDR_COUNT=$(sqlite3 "$WALLET_DB" "SELECT COUNT(*) FROM addresses;" 2>/dev/null)
echo ""
echo "Addresses derived: $ADDR_COUNT"

if [ "$ADDR_COUNT" -gt 0 ]; then
    echo ""
    echo "Sample addresses:"
    sqlite3 "$WALLET_DB" "SELECT '  ' || address || ' (' || type || ')' FROM addresses ORDER BY id LIMIT 3;" 2>/dev/null
fi

echo ""
echo "=== End ===="
