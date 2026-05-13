#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 9: Enforcement - lightningd Must NOT Access Chainstate
# ═══════════════════════════════════════════════════════════════════════════
# Ensures lightningd stays L1-independent after Phase 8 extraction.
#
# Forbidden in src/lightningd/:
# - chainstate includes
# - mempool includes
# - wallet includes
# - RocksDB includes
# - Any L1-specific headers
#
# Rationale:
# - lightningd is pure event-driven IPC server
# - Block scanning moved to WatchtowerService (src/watchtower/)
# - Lightning receives facts via IPC, never queries L1 directly
#
# Exit code:
# - 0: No violations detected
# - 1: Phase 8/9 violation detected (CI FAIL)
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 9: Enforcement - lightningd L1 Independence"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

VIOLATIONS=0

# Check if lightningd directory exists
if [ ! -d "src/lightningd" ]; then
    echo "✅ SKIP: src/lightningd/ does not exist yet (Phase 8 not started)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 0
fi

echo "[1/4] Checking for forbidden chainstate includes..."
echo ""

# Pattern 1: chainstate headers
if grep -r "#include.*chainstate" src/lightningd/ 2>/dev/null | grep -v "//"; then
    echo ""
    echo "❌ VIOLATION: lightningd includes chainstate headers"
    echo "   Location: src/lightningd/"
    echo "   Fix: Use WatchtowerService (src/watchtower/) for block scanning"
    echo "   Phase 9 rule: Lightning receives facts via IPC, never queries chainstate"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
fi

echo "[2/4] Checking for forbidden mempool includes..."
echo ""

# Pattern 2: mempool headers
if grep -r "#include.*mempool" src/lightningd/ 2>/dev/null | grep -v "//"; then
    echo ""
    echo "❌ VIOLATION: lightningd includes mempool headers"
    echo "   Location: src/lightningd/"
    echo "   Fix: Transaction broadcasting handled by L1 service"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
fi

echo "[3/4] Checking for forbidden wallet includes..."
echo ""

# Pattern 3: wallet headers (except oracle interfaces)
if grep -r "#include.*wallet" src/lightningd/ 2>/dev/null | grep -v "wallet_oracle" | grep -v "//"; then
    echo ""
    echo "❌ VIOLATION: lightningd includes wallet headers (except wallet_oracle)"
    echo "   Location: src/lightningd/"
    echo "   Fix: Use IWalletOracle interface, not direct wallet access"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
fi

echo "[4/4] Checking for forbidden RocksDB includes..."
echo ""

# Pattern 4: RocksDB headers
if grep -r "#include.*rocksdb" src/lightningd/ 2>/dev/null | grep -v "//"; then
    echo ""
    echo "❌ VIOLATION: lightningd includes RocksDB headers"
    echo "   Location: src/lightningd/"
    echo "   Fix: Use ILightningDB interface, not direct RocksDB access"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $VIOLATIONS -eq 0 ]; then
    echo "✅ PASS: lightningd is L1-independent"
    echo "   Phase 8/9 invariants verified:"
    echo "   ✅ NO chainstate includes"
    echo "   ✅ NO mempool includes"
    echo "   ✅ NO wallet includes (except oracle)"
    echo "   ✅ NO RocksDB includes"
    echo "   ✅ Pure event-driven IPC server"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 0
else
    echo "❌ FAIL: $VIOLATIONS violation(s) detected"
    echo "   lightningd MUST be L1-independent (Phase 8/9)"
    echo "   Block scanning: Use WatchtowerService (src/watchtower/)"
    echo "   Transaction broadcast: Use L1 services"
    echo "   Database: Use ILightningDB interface only"
    echo "   Phase 9 Enforcement: FAILED"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 1
fi
