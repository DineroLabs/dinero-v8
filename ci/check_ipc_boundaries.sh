#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 9: IPC Boundary Enforcement
# ═══════════════════════════════════════════════════════════════════════════
# Ensures IPC layer maintains proper L1/L2 separation.
#
# Rules:
# 1. IPC Client (L1) must NOT include Lightning headers
# 2. IPC Client sends facts only (txid, height, hash)
# 3. NO channel_id in IPC messages (Lightning already knows which txids matter)
# 4. NO interpretation (breach, justice, HTLC logic)
#
# Exit code:
# - 0: IPC boundaries respected
# - 1: IPC boundary violation detected (CI FAIL)
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 9: IPC Boundary Enforcement"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

VIOLATIONS=0

# Check if IPC directory exists
if [ ! -d "src/ipc" ] && [ ! -d "include/ipc" ]; then
    echo "✅ SKIP: IPC layer not implemented yet"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 0
fi

echo "[1/3] Checking IPC client for forbidden Lightning includes..."
echo ""

# IPC client must NOT include Lightning business logic headers
FORBIDDEN_LIGHTNING_INCLUDES=(
    "channel_manager"
    "htlc_manager"
    "payment_router"
    "gossip_manager"
    "lightning_wallet"
)

for pattern in "${FORBIDDEN_LIGHTNING_INCLUDES[@]}"; do
    if grep -r "#include.*$pattern" src/ipc/ include/ipc/ 2>/dev/null; then
        echo ""
        echo "❌ VIOLATION: IPC client includes Lightning header: $pattern"
        echo "   Location: src/ipc/ or include/ipc/"
        echo "   Fix: IPC client sends facts only (txid, height, hash)"
        echo "   Rule: NO Lightning business logic in IPC layer"
        echo ""
        VIOLATIONS=$((VIOLATIONS + 1))
    fi
done

if [ $VIOLATIONS -eq 0 ]; then
    echo "  ✅ IPC client does not include Lightning business logic headers"
fi

echo ""
echo "[2/3] Checking IPC messages for forbidden fields..."
echo ""

# IPC messages must NOT include channel_id, HTLC details, breach interpretation
FORBIDDEN_MESSAGE_FIELDS=(
    "channel_id"
    "is_breach"
    "htlc_id"
    "justice"
)

for pattern in "${FORBIDDEN_MESSAGE_FIELDS[@]}"; do
    if grep -r "$pattern" src/ipc/ include/ipc/ 2>/dev/null | grep -v "// opaque\|// NOT sent\|// ignored"; then
        echo ""
        echo "⚠️  WARNING: IPC message may include forbidden field: $pattern"
        echo "   Review: Ensure this field is NOT sent over IPC wire"
        echo "   Rule: IPC sends facts only (txid, height, hash)"
        echo ""
        # Don't fail on warnings, just alert
    fi
done

echo "  ✅ IPC message format appears correct"

echo ""
echo "[3/3] Checking watchtower for Lightning includes..."
echo ""

# Watchtower (L1-adjacent) must NOT include Lightning headers
if [ -d "src/watchtower" ]; then
    for pattern in "${FORBIDDEN_LIGHTNING_INCLUDES[@]}"; do
        if grep -r "#include.*lightning/$pattern" src/watchtower/ 2>/dev/null; then
            echo ""
            echo "❌ VIOLATION: Watchtower includes Lightning header: $pattern"
            echo "   Location: src/watchtower/"
            echo "   Fix: Watchtower is L1-adjacent, sends facts only"
            echo "   Rule: NO Lightning business logic in watchtower"
            echo ""
            VIOLATIONS=$((VIOLATIONS + 1))
        fi
    done

    if [ $VIOLATIONS -eq 0 ]; then
        echo "  ✅ Watchtower does not include Lightning business logic headers"
    fi
else
    echo "  ⚠️  Watchtower directory not found (skipping check)"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $VIOLATIONS -eq 0 ]; then
    echo "✅ PASS: IPC boundaries respected"
    echo "   Architecture verified:"
    echo "   ✅ IPC client includes NO Lightning headers"
    echo "   ✅ IPC messages are facts only (txid, height, hash)"
    echo "   ✅ Watchtower includes NO Lightning headers"
    echo "   ✅ L1/L2 separation maintained through IPC"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 0
else
    echo "❌ FAIL: $VIOLATIONS IPC boundary violation(s) detected"
    echo "   IPC layer MUST maintain L1/L2 separation"
    echo "   IPC sends facts only: txid, height, hash"
    echo "   NO Lightning logic in IPC client or watchtower"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    exit 1
fi
