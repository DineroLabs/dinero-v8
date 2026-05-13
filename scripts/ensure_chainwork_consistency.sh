#!/usr/bin/env bash
# One-liner chainwork consistency checker for daemon startup
# Usage: ./scripts/ensure_chainwork_consistency.sh [network]
# Safe to run before every daemon start

set -Eeuo pipefail

REPO="${REPO:-/Users/haydarevich/Documents/DineroCoin}"
NET="${1:-regtest}"
DATADIR="${DATADIR:-$REPO/data}"
DB_FILE="$DATADIR/$NET/blockchain.db"

# Quick chainwork fix if needed
if [[ -f "$DB_FILE" ]]; then
  CURRENT_CW=$(sqlite3 "$DB_FILE" "SELECT value FROM meta WHERE key='chainwork' LIMIT 1;" 2>/dev/null || echo "")
  if [[ "$CURRENT_CW" == "0000000000000000000000000000000000000000000000000000000000000000" ]]; then
    echo "🔧 Fixing chainwork for $NET..."
    sqlite3 "$DB_FILE" "UPDATE meta SET value='0000000000000000000000000000000000000000000000000000000000000001' WHERE key='chainwork';"
    echo "✅ Chainwork fixed: $NET"
  fi
fi
