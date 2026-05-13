#!/usr/bin/env bash
# CI Database Check - ensures database integrity across all networks
# Usage: ./scripts/ci_db_check.sh
set -Eeuo pipefail

echo "🔍 **CI DATABASE INTEGRITY CHECK**"
echo "================================="
echo ""

FAILED=0

# Check each network
for NET in regtest testnet mainnet; do
    echo "**Testing $NET network:**"
    
    # Ensure meta keys are present
    if ./scripts/fix_genesis_meta.sh "$NET" --datadir ./data; then
        echo "   ✅ Meta keys initialized"
    else
        echo "   ❌ Meta initialization failed"
        FAILED=1
    fi
    
    # Run database audit
    if ./scripts/db_audit.sh "$NET"; then
        echo "   ✅ Database audit passed"
    else
        echo "   ❌ Database audit failed"
        FAILED=1
    fi
    
    echo ""
done

# Test idempotence
echo "**Testing idempotence:**"
if ./build/test_db_init_idempotence; then
    echo "   ✅ Idempotence test passed"
else
    echo "   ❌ Idempotence test failed"
    FAILED=1
fi

echo ""
if [[ $FAILED -eq 0 ]]; then
    echo "🎊 **ALL CI DATABASE CHECKS PASSED** 🎊"
    exit 0
else
    echo "❌ **CI DATABASE CHECKS FAILED** ❌"
    exit 1
fi
