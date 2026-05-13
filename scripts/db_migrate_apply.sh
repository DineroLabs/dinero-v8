#!/usr/bin/env bash
# Multi-daemon safe database migration and initialization script
# Idempotent - safe to run multiple times
set -euo pipefail

BASE_DIR="$(cd "$(dirname "$0")/.." && pwd)/data"   # ../data relative to repo root
NET="${1:-regtest}"                                 # regtest|testnet|mainnet
case "$NET" in
  mainnet|testnet) DEFAULT_RPC_PORT=20998 ;;
  regtest) DEFAULT_RPC_PORT=20996 ;;
  *) echo "Unknown network '$NET'" >&2; exit 2 ;;
esac
RPC_PORT="${RPC_PORT:-$DEFAULT_RPC_PORT}"            # adjust if needed

DB_DIR="$BASE_DIR/$NET"
CHAIN_DB="$DB_DIR/blockchain.db"
PEERS_DB="$DB_DIR/peers.db"
EXPL_DB="$DB_DIR/explorer.db"
COOKIE="$DB_DIR/.cookie"

mkdir -p "$DB_DIR"

apply_sql () {
  local db="$1"; local sql="$2"
  echo "  Applying $(basename "$sql") to $(basename "$db")..."
  sqlite3 "$db" < "$sql"
}

echo "== Applying migrations to $NET in $DB_DIR =="
apply_sql "$CHAIN_DB" "$(dirname "$0")/../migrations/001_chainstate.sql"
apply_sql "$PEERS_DB"  "$(dirname "$0")/../migrations/002_peers.sql"
apply_sql "$EXPL_DB"   "$(dirname "$0")/../migrations/003_explorer.sql"

# helpers
sql() { sqlite3 "$1" "$2"; }
upsert_meta () {
  local k="$1"; local v="$2"
  sql "$CHAIN_DB" "INSERT INTO meta(key,value) VALUES('$k',$v)
                   ON CONFLICT(key) DO UPDATE SET value=excluded.value;"
}

# fetch genesis via RPC
echo "  Fetching genesis hash via RPC on port $RPC_PORT..."
if [[ -f "$COOKIE" ]]; then
  GENESIS="$(curl -s --user "$(cat "$COOKIE")" \
    -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":"g","method":"getblockhash","params":[0]}' \
    "http://127.0.0.1:${RPC_PORT}/" | sed -n 's/.*"result":"\([0-9a-fA-F]\+\)".*/\1/p')" || true
else
  echo "WARN: No cookie at $COOKIE; setting empty genesis_hash for now"
  GENESIS=""
fi

# minimal chain pointers if not present
CURRENT_HEIGHT="$(sql "$CHAIN_DB" "SELECT value FROM meta WHERE key='height';" 2>/dev/null || true)"
CURRENT_BEST="$(sql "$CHAIN_DB" "SELECT value FROM meta WHERE key='besthash';" 2>/dev/null || true)"
CURRENT_WORK="$(sql "$CHAIN_DB" "SELECT value FROM meta WHERE key='chainwork';" 2>/dev/null || true)"

echo "  Setting meta keys..."
upsert_meta schema_version "'1'"
upsert_meta network "'$NET'"

# If RPC didn't return genesis, use hardcoded values
if [[ -z "$GENESIS" ]]; then
  echo "  RPC unavailable, using hardcoded genesis hash..."
  case "$NET" in
    "regtest") GENESIS="0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206" ;;
    "testnet") GENESIS="000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943" ;;
    "mainnet") GENESIS="000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f" ;;
    *) echo "  Warning: No genesis hash for network $NET" ;;
  esac
fi

[[ -n "$GENESIS" ]] && upsert_meta genesis_hash "X'$GENESIS'"
[[ -z "$CURRENT_HEIGHT" ]] && upsert_meta height "'0'"
[[ -z "$CURRENT_BEST" && -n "$GENESIS" ]] && upsert_meta besthash "X'$GENESIS'"
[[ -z "$CURRENT_WORK" ]] && upsert_meta chainwork "X'0000000000000000000000000000000000000000000000000000000000000000'"

# Ensure block_index has height 0 entry
[[ -n "$GENESIS" ]] && sql "$CHAIN_DB" "INSERT OR IGNORE INTO block_index(height,hash) VALUES(0, X'$GENESIS');" 2>/dev/null || true

# pragmas on all DBs
echo "  Checkpointing WAL files..."
for DB in "$CHAIN_DB" "$PEERS_DB" "$EXPL_DB"; do
  sql "$DB" "PRAGMA wal_checkpoint(TRUNCATE);" 2>/dev/null || true
done

echo "== Done. Meta =="
sql "$CHAIN_DB" "SELECT key, CASE key
  WHEN 'besthash' THEN hex(value)
  WHEN 'genesis_hash' THEN hex(value)
  WHEN 'chainwork' THEN hex(value)
  ELSE value END AS value
FROM meta ORDER BY key;" 2>/dev/null || echo "No meta table yet"

echo ""
echo "✅ Database migration complete for $NET network"
echo "   Chainstate: $CHAIN_DB"
echo "   Peers:      $PEERS_DB"  
echo "   Explorer:   $EXPL_DB"
