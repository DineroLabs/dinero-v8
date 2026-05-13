#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.6: Enforcement Layer 2 - Symbol Boundary Guard
# ═══════════════════════════════════════════════════════════════════════════
# Ensures lightningd binary does not link L1 symbols
#
# Forbidden symbols:
# - Chainstate         (L1 blockchain state manager)
# - Wallet             (L1 wallet implementation)
# - Mempool            (L1 mempool manager)
# - NodeContext        (L1 daemon context)
# - RPCContext         (L1 RPC server)
# - GetTime            (L1 time source)
# - system_clock       (Wall clock time - forbidden in Phase 8.5)
# - std::thread        (Threads - forbidden in Phase 8.5)
#
# Exit code:
# - 0: No violations detected
# - 1: L1 symbol linkage detected (CI FAIL)
# - 2: lightningd binary not found (CI FAIL)
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 8.6 - Enforcement Layer 2: Symbol Boundary Guard"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Find lightningd binary
LIGHTNINGD_BINARY=""
SEARCH_PATHS=(
  "lightningd"
  "./lightningd"
  "build/lightningd"
  "./build/lightningd"
  "bin/lightningd"
  "./bin/lightningd"
)

for path in "${SEARCH_PATHS[@]}"; do
  if [ -f "$path" ]; then
    LIGHTNINGD_BINARY="$path"
    echo "[1/4] Found lightningd binary: $LIGHTNINGD_BINARY"
    break
  fi
done

if [ -z "$LIGHTNINGD_BINARY" ]; then
  echo "❌ FAIL: lightningd binary not found"
  echo "   Searched paths: ${SEARCH_PATHS[*]}"
  echo "   Run: make lightningd"
  exit 2
fi

echo "[2/4] Extracting symbols from lightningd..."
echo ""

# Extract all symbols (defined + undefined)
SYMBOLS=$(nm "$LIGHTNINGD_BINARY" 2>/dev/null || echo "")

if [ -z "$SYMBOLS" ]; then
  echo "❌ FAIL: Could not extract symbols from lightningd"
  echo "   Tool: nm (part of binutils)"
  echo "   Binary: $LIGHTNINGD_BINARY"
  exit 2
fi

echo "[3/4] Checking for forbidden L1 symbols..."
echo ""

VIOLATIONS=0

# Forbidden L1 symbols (exact matches)
FORBIDDEN_EXACT=(
  "Chainstate"
  "ChainstateManager"
  "WalletManager"
  "MempoolService"
  "NodeContext"
  "RPCContext"
  "GetTime"
)

for symbol in "${FORBIDDEN_EXACT[@]}"; do
  if echo "$SYMBOLS" | grep -q "$symbol"; then
    echo "  ❌ VIOLATION: Forbidden L1 symbol detected: $symbol"
    echo "$SYMBOLS" | grep "$symbol" | head -5
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  else
    echo "  ✅ $symbol (not present)"
  fi
done

echo ""
echo "[4/4] Checking for Phase 8.5 runtime violations..."
echo ""

# Phase 8.5 forbidden patterns
RUNTIME_VIOLATIONS=(
  "std::thread"
  "system_clock"
  "steady_clock"
)

for pattern in "${RUNTIME_VIOLATIONS[@]}"; do
  if echo "$SYMBOLS" | grep -q "$pattern"; then
    echo "  ❌ VIOLATION: Phase 8.5 runtime pattern detected: $pattern"
    echo "     Lightning MUST NOT use threads or wall clocks"
    echo "$SYMBOLS" | grep "$pattern" | head -5
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  else
    echo "  ✅ $pattern (not present)"
  fi
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $VIOLATIONS -eq 0 ]; then
  echo "✅ PASS: Lightning symbol boundary enforced"
  echo "   Binary: $LIGHTNINGD_BINARY"
  echo "   No L1 symbols or forbidden runtime patterns detected"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 0
else
  echo "❌ FAIL: $VIOLATIONS violation(s) detected"
  echo "   Binary: $LIGHTNINGD_BINARY"
  echo "   Lightning MUST NOT link L1 symbols or use forbidden runtime patterns"
  echo "   Phase 8.6 Enforcement Layer 2: FAILED"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 1
fi
