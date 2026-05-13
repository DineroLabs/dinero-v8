#!/usr/bin/env bash
# Phase M.1 Mechanical Gate: Enforce ChainStateView Abstraction
#
# Prevents mempool from directly depending on CoinsDB implementation.
# Mempool MUST use ChainStateView abstraction (read-only interface).
#
# Allowed: ChainStateView (abstract interface)
# Forbidden: CoinsDB, CoinsViewCache (implementation details)
#
# Scope: mempool/ only
# Exception: CoinsViewMemPool wraps ChainStateView (allowed)

set -e

# Define mempool directories to check
MEMPOOL_DIRS=(
    "include/mempool"
    "src/mempool"
)

# Change to repository root
cd "$(dirname "$0")/.."

# Check if ripgrep is available
if ! command -v rg &> /dev/null; then
    echo "⚠️  WARNING: ripgrep (rg) not found, falling back to grep"
    USE_GREP=1
else
    USE_GREP=0
fi

BAD=""

# Search for direct CoinsDB dependencies in mempool
for dir in "${MEMPOOL_DIRS[@]}"; do
    if [[ ! -d "$dir" ]]; then
        continue
    fi

    if [[ $USE_GREP -eq 1 ]]; then
        # Fallback to grep
        # Exclude coins_view_mempool.h/cpp (allowed to reference ChainStateView)
        MATCHES=$(grep -rn 'coins_db\.h\|CoinsViewCache\|getCoinsDB' "$dir" \
            --exclude="coins_view_mempool.h" \
            --exclude="coins_view_mempool.cpp" \
            2>/dev/null || true)
    else
        # Use ripgrep (faster, better)
        MATCHES=$(rg 'coins_db\.h|CoinsViewCache|getCoinsDB' "$dir" \
            --glob '!coins_view_mempool.h' \
            --glob '!coins_view_mempool.cpp' \
            2>/dev/null || true)
    fi

    if [[ -n "$MATCHES" ]]; then
        BAD="${BAD}${MATCHES}\n"
    fi
done

if [[ -n "$BAD" ]]; then
    echo "❌ ERROR: Mempool depends on CoinsDB implementation directly:"
    echo ""
    echo -e "$BAD"
    echo ""
    echo "Phase M.1 Invariant Violation:"
    echo "  - Mempool MUST use ChainStateView abstraction"
    echo "  - Direct CoinsDB/CoinsViewCache access is forbidden"
    echo ""
    echo "Fix: Replace CoinsDB/CoinsViewCache with const ChainStateView&"
    exit 1
fi

echo "✅ mempool dependency check passed (ChainStateView only)"
