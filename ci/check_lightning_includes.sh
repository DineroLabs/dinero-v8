#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.6: Enforcement Layer 1 - Header Boundary Guard
# ═══════════════════════════════════════════════════════════════════════════
# Ensures Lightning cannot include L1 headers
#
# Forbidden includes:
# - include/consensus/    (L1 consensus logic)
# - include/chainstate/   (L1 blockchain state)
# - include/wallet/       (L1 wallet functionality)
# - include/mempool/      (L1 mempool management)
# - include/rpc/          (L1 RPC server)
# - include/daemon/       (L1 daemon context)
# - include/net/          (L1 P2P networking)
# - include/mining/       (L1 mining/block assembly)
#
# Exit code:
# - 0: No violations detected
# - 1: L1 header inclusion detected (CI FAIL)
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 8.6 - Enforcement Layer 1: Header Boundary Guard"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

VIOLATIONS=0

# List of forbidden L1 directories
FORBIDDEN=(
  "include/consensus"
  "include/chainstate"
  "include/wallet"
  "include/mempool"
  "include/rpc"
  "include/daemon"
  "include/net"
  "include/mining"
)

# Lightning code directories to check
LIGHTNING_DIRS=(
  "src/lightning"
  "src/lightningd"
  "include/lightning"
)

echo "[1/3] Checking Lightning source files for forbidden L1 includes..."
echo ""

for forbidden_path in "${FORBIDDEN[@]}"; do
  echo "  Checking for: $forbidden_path"

  for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
    if [ ! -d "$lightning_dir" ]; then
      echo "    ⚠️  Directory not found: $lightning_dir (skipping)"
      continue
    fi

    # Search for forbidden includes in Lightning code
    # Matches: #include "path/..." or #include <path/...>
    if grep -r -E "#include [\"<]${forbidden_path}" "$lightning_dir" 2>/dev/null; then
      echo ""
      echo "    ❌ VIOLATION: Lightning code includes L1 header: $forbidden_path"
      echo "    Location: $lightning_dir"
      echo ""
      VIOLATIONS=$((VIOLATIONS + 1))
    fi
  done
done

echo ""
echo "[2/3] Checking for indirect L1 coupling patterns..."
echo ""

# Additional patterns that indicate L1 coupling
COUPLING_PATTERNS=(
  "DaemonContext"
  "ChainstateManager"
  "WalletManager"
  "MempoolService"
  "NodeContext"
  "P2PManager"
)

for pattern in "${COUPLING_PATTERNS[@]}"; do
  echo "  Checking for: $pattern"

  for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
    if [ ! -d "$lightning_dir" ]; then
      continue
    fi

    # Skip comments and string literals (basic heuristic)
    if grep -r "$pattern" "$lightning_dir" 2>/dev/null | grep -v "^[[:space:]]*//\|^[[:space:]]*\*"; then
      echo ""
      echo "    ⚠️  WARNING: Lightning code references L1 type: $pattern"
      echo "    Location: $lightning_dir"
      echo "    Review: Ensure this is via oracle interface, not direct coupling"
      echo ""
      # Don't fail on warnings, but report them
    fi
  done
done

echo ""
echo "[3/3] Verifying Lightning-only headers..."
echo ""

# Ensure Lightning headers exist and are accessible
REQUIRED_HEADERS=(
  "include/lightning/lightning_event_sink.h"
  "src/lightningd/ipc_server.h"
)

for header in "${REQUIRED_HEADERS[@]}"; do
  if [ -f "$header" ]; then
    echo "  ✅ $header"
  else
    echo "  ❌ Missing required header: $header"
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $VIOLATIONS -eq 0 ]; then
  echo "✅ PASS: Lightning header boundary enforced"
  echo "   No L1 includes detected in Lightning code"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 0
else
  echo "❌ FAIL: $VIOLATIONS violation(s) detected"
  echo "   Lightning code MUST NOT include L1 headers"
  echo "   Phase 8.6 Enforcement Layer 1: FAILED"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 1
fi
