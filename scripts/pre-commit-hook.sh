#!/bin/bash

# Dinero Pre-commit Hook
# 1. Checks for banned global variable usages

set -e

# ═══════════════════════════════════════════════════════════════
# CHECK 1: Ban new global variable usages
# ═══════════════════════════════════════════════════════════════

echo "🔍 Checking for banned global variable usages..."

# Get all staged changes (only additions, not deletions)
bad=$(git diff --cached -U0 --diff-filter=AM | grep -E '^\+.*\b(g_mempool|g_blockchain|g_wallet_manager|g_chain_db_direct|g_utxo_set_direct|g_peer_manager|g_p2p|g_data_dir)\b' || true)

# Filter out whitelisted globals
bad=$(echo "$bad" | grep -v 'g_rpcRegistry\|g_logger\|g_secp\|g_daemon_start_time\|g_external_ip' || true)

# Filter out comments and documentation
bad=$(echo "$bad" | grep -v '^\+\s*//\|^\+\s*/\*\|^\+\s*\*' || true)

# Filter out intentionally allowed files
bad=$(echo "$bad" | grep -v 'global_shim.hpp\|ban_globals.hpp\|legacy_globals_stub.cpp' || true)

# Filter out documentation files (they document the migration)
bad=$(echo "$bad" | grep -v '\.md$' || true)

if [[ -n "$bad" ]]; then
  echo "❌ ERROR: New banned global usages detected:"
  echo "$bad"
  echo ""
  echo "These globals are banned. Use one of these instead:"
  echo "  1. dinero::legacy::g_mempool() etc. (temporary shim)"
  echo "  2. Inject service via constructor (preferred)"
  echo ""
  echo "To bypass: git commit --no-verify"
  exit 1
fi

echo "✅ No banned global usages detected"

# ═══════════════════════════════════════════════════════════════
# Removed: test_architecture_regression (never implemented)
# TODO: Reintroduce when test exists
# ═══════════════════════════════════════════════════════════════

exit 0