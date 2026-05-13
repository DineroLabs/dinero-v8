#!/usr/bin/env bash
# Phase M.1 Mechanical Gate: Enforce uint256 txid in Core
#
# Prevents regression to std::string-based transaction identifiers
# in consensus and mempool layers.
#
# Allowed: uint256 txid (canonical identity type)
# Forbidden: std::string txid (presentation type)
#
# Scope: consensus/ and mempool/ only
# Explicitly excludes: wallet/, rpc/, tests/ (presentation boundaries)

set -e

# Define core directories to check
CORE_DIRS=(
    "include/consensus"
    "include/mempool"
    "src/consensus"
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

# Search for std::string txid patterns in core directories
# Exclude false positives:
# - Comments (lines starting with // or /* or * after whitespace)
# - GetTxIdHex() method (converts uint256 to string for presentation)
# - Documentation comments about Phase M.0 changes
for dir in "${CORE_DIRS[@]}"; do
    if [[ ! -d "$dir" ]]; then
        continue
    fi

    if [[ $USE_GREP -eq 1 ]]; then
        # Fallback to grep
        MATCHES=$(grep -rn 'std::string\s\+txid\|std::string.*txid' "$dir" 2>/dev/null || true)
    else
        # Use ripgrep (faster, better)
        MATCHES=$(rg 'std::string\s+txid|std::string.*txid' "$dir" 2>/dev/null || true)
    fi

    if [[ -n "$MATCHES" ]]; then
        # Filter out false positives
        FILTERED=$(echo -e "$MATCHES" | grep -v 'GetTxIdHex\|GetHex\|//\|/\*\|\*' || true)
        if [[ -n "$FILTERED" ]]; then
            BAD="${BAD}${FILTERED}\n"
        fi
    fi
done

if [[ -n "$BAD" ]]; then
    echo "❌ ERROR: std::string txid found in core code:"
    echo ""
    echo -e "$BAD"
    echo ""
    echo "Phase M.1 Invariant Violation:"
    echo "  - Core code (consensus/mempool) MUST use uint256 for txid"
    echo "  - std::string is allowed ONLY at presentation boundaries (RPC, logs, tests)"
    echo ""
    echo "Fix: Replace std::string txid with uint256 txid"
    exit 1
fi

echo "✅ txid type check passed (uint256 only in core)"
