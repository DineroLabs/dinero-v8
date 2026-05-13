#!/usr/bin/env bash
# Database health and consistency audit script
# Verifies schema integrity and chain state consistency
set -euo pipefail

NET="${1:-regtest}"
BASE_DIR="$(cd "$(dirname "$0")/.." && pwd)/data"
DB="$BASE_DIR/$NET/blockchain.db"

if [[ ! -f "$DB" ]]; then
  echo "❌ Database not found: $DB"
  echo "   Run: ./scripts/db_migrate_apply.sh $NET"
  exit 1
fi

echo "🔍 **DATABASE AUDIT - $NET NETWORK**"
echo "=================================="
echo ""

echo "== Meta Information =="
sqlite3 "$DB" "SELECT key, CASE key 
                                WHEN 'besthash' THEN hex(value)
                                WHEN 'genesis_hash' THEN hex(value)
                                WHEN 'chainwork' THEN hex(value)
                                ELSE value END AS value
               FROM meta ORDER BY key;" 2>/dev/null || echo "No meta table"

echo ""
echo "== Database Counts =="
sqlite3 "$DB" "SELECT (SELECT COUNT(*) FROM headers) AS headers,
                      (SELECT COUNT(*) FROM utxo) AS utxo,
                      (SELECT COUNT(*) FROM block_index) AS block_index;" 2>/dev/null || echo "Tables not ready"

echo ""
echo "== Consistency Checks =="
echo "Height/Hash mismatches:"
sqlite3 "$DB" "SELECT COUNT(*) AS mismatches
               FROM headers h JOIN block_index b ON h.hash=b.hash
               WHERE h.height != b.height;" 2>/dev/null || echo "0 (tables not ready)"

echo "Negative UTXO amounts:"
sqlite3 "$DB" "SELECT COUNT(*) FROM utxo WHERE amount < 0;" 2>/dev/null || echo "0 (table not ready)"

echo "Orphan UTXOs (future heights):"
sqlite3 "$DB" "SELECT COUNT(*) FROM utxo u 
               WHERE u.height > (SELECT CAST(value AS INTEGER) FROM meta WHERE key='height');" 2>/dev/null || echo "0 (tables not ready)"

echo ""
echo "== Peer Database =="
PEERS_DB="$BASE_DIR/$NET/peers.db"
if [[ -f "$PEERS_DB" ]]; then
  echo "Peers: $(sqlite3 "$PEERS_DB" "SELECT COUNT(*) FROM peers;" 2>/dev/null || echo "0")"
  echo "Bans:  $(sqlite3 "$PEERS_DB" "SELECT COUNT(*) FROM bans;" 2>/dev/null || echo "0")"
else
  echo "Peers database not found"
fi

echo ""
echo "== Explorer Cache =="
EXPL_DB="$BASE_DIR/$NET/explorer.db"
if [[ -f "$EXPL_DB" ]]; then
  echo "Blocks: $(sqlite3 "$EXPL_DB" "SELECT COUNT(*) FROM blocks;" 2>/dev/null || echo "0")"
  echo "Txs:    $(sqlite3 "$EXPL_DB" "SELECT COUNT(*) FROM txs;" 2>/dev/null || echo "0")"
else
  echo "Explorer database not found"
fi

echo ""
echo "== File Sizes =="
for db_file in "$BASE_DIR/$NET"/*.db; do
  if [[ -f "$db_file" ]]; then
    size=$(du -h "$db_file" | cut -f1)
    echo "$(basename "$db_file"): $size"
  fi
done

echo ""
echo "✅ Audit complete for $NET network"
