#!/usr/bin/env bash
set -Eeuo pipefail

# Auto-fix chainwork across all networks
# Safe to run anytime - only fixes databases with wrong chainwork

REPO="${REPO:-/Users/haydarevich/Documents/DineroCoin}"
DATADIR="${DATADIR:-$REPO/data}"
MIGRATION="$REPO/scripts/migrations/fix_genesis_chainwork.sql"

log() { printf "%s %s\n" "$(date +'%H:%M:%S')" "$*"; }

echo "🔧 CHAINWORK AUTO-MIGRATION"
echo "==========================="
echo "Repo:      $REPO"
echo "Data dir:  $DATADIR"
echo "Migration: $MIGRATION"
echo

# Check if migration exists
[[ -f "$MIGRATION" ]] || { echo "❌ Migration not found: $MIGRATION"; exit 1; }

# Find all blockchain.db files
DB_FILES=()
for net_dir in "$DATADIR"/{mainnet,testnet,regtest}; do
  db_file="$net_dir/blockchain.db"
  if [[ -f "$db_file" ]]; then
    DB_FILES+=("$db_file")
    log "Found: $db_file"
  fi
done

if [[ ${#DB_FILES[@]} -eq 0 ]]; then
  log "No blockchain.db files found - nothing to migrate"
  exit 0
fi

echo

# Apply migration to each database
for db_file in "${DB_FILES[@]}"; do
  network=$(basename "$(dirname "$db_file")")
  log "Migrating $network: $db_file"
  
  # Run the migration
  if sqlite3 "$db_file" < "$MIGRATION"; then
    log "✅ $network migration completed"
  else
    log "❌ $network migration failed"
    exit 1
  fi
  echo
done

log "🎉 All chainwork migrations completed successfully"
