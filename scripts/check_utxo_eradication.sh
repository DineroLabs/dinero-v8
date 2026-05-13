#!/bin/bash
# Phase M.0-pre: Mechanical gate for UTXO eradication
# This script ensures no struct UTXO exists outside allowed types

set -e

echo "Phase M.0-pre Mechanical Gate: Checking UTXO eradication..."
echo ""

# Allowed types:
# - consensus::UTXOEntry (correct consensus type)
# - WalletUTXO (wallet metadata)
# - SigningUTXO (signing helper, if kept)

# Find all struct UTXO definitions, excluding allowed types
# Must match "struct UTXO " or "struct UTXO{" to avoid false positives like UTXOSet, UTXOData
VIOLATIONS=$(rg "struct UTXO\s|struct UTXO\{" include/ src/ 2>/dev/null | rg -v "UTXOEntry|WalletUTXO|SigningUTXO" || true)

if [ -z "$VIOLATIONS" ]; then
    echo "✅ PASS: No dinero::UTXO violations found"
    echo ""
    echo "Allowed types only:"
    rg "struct (UTXOEntry|WalletUTXO|SigningUTXO)" include/ src/ 2>/dev/null || echo "  (none found yet)"
    exit 0
else
    echo "❌ FAIL: Found dinero::UTXO violations:"
    echo ""
    echo "$VIOLATIONS"
    echo ""
    echo "Phase M cannot begin until these are eradicated."
    exit 1
fi
