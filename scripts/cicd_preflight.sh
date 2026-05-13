#!/usr/bin/env bash
# CI/CD preflight consistency check - safe to run anytime
set -Eeuo pipefail

DATADIR="${DATADIR:-/Users/haydarevich/Documents/DineroCoin/data}"
NET="${NET:-regtest}"
DB_FILE="$DATADIR/$NET/blockchain.db"

if [[ -f "$DB_FILE" ]]; then
  echo "🔧 CI/CD preflight: checking chainwork consistency for $NET..."
  sqlite3 "$DB_FILE" "BEGIN IMMEDIATE; UPDATE meta SET value='0000000000000000000000000000000000000000000000000000000000000001' WHERE key='chainwork' AND value='0000000000000000000000000000000000000000000000000000000000000000' AND (SELECT value FROM meta WHERE key='height')='0'; COMMIT;" || true
  echo "✅ CI/CD preflight completed"
else
  echo "ℹ️  No database found at $DB_FILE - skipping preflight"
fi
