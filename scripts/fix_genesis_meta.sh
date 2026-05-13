#!/usr/bin/env bash
# Production-grade fix for missing genesis_hash and besthash in meta table
# Hardened version with proper error handling and RPC fallback
set -Eeuo pipefail

usage() {
  echo "Usage: $0 [regtest|testnet|mainnet] [--datadir PATH]" >&2
}

network="${1:-regtest}"
if [[ "$network" == "--datadir" ]]; then usage; exit 2; fi
shift || true

datadir="${PWD}/data"  # default; override with --datadir
while (($#)); do
  case "$1" in
    --datadir) datadir="$2"; shift 2;;
    *) usage; exit 2;;
  esac
done

# Known genesis hashes (lowercase then we uppercase once)
case "$network" in
  "regtest")
    genesis_default="0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"
    ;;
  "testnet")
    # TODO: replace with your actual testnet genesis when mined
    genesis_default="0000000000000000000000000000000000000000000000000000000000000000"
    ;;
  "mainnet")
    # TODO: replace with your actual mainnet genesis when mined
    genesis_default="0000000000000000000000000000000000000000000000000000000000000000"
    ;;
  *)
    echo "Unknown network: $network"
    exit 1
    ;;
esac

dbdir="${datadir}/${network}"
mkdir -p "$dbdir"
blockchain_db="${dbdir}/blockchain.db"

echo "🔧 **FIXING MISSING GENESIS_HASH AND BESTHASH**"
echo "=============================================="
echo "Network: $network"
echo "Database: $blockchain_db"
echo ""

if [[ ! -f "$blockchain_db" ]]; then
  echo "Creating empty blockchain.db at $blockchain_db"
  sqlite3 "$blockchain_db" 'CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);'
fi

# Prefer RPC getblockhash 0 if cookie is present and daemon is up; else fall back to constants
cookie_path="${dbdir}/.cookie"
genesis="$genesis_default"
if [[ -f "$cookie_path" ]]; then
  echo "🔍 Attempting RPC genesis hash lookup..."
  cookie=$(cat "$cookie_path")
  rpc="$(curl -sS --user "$cookie" -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":"fix","method":"getblockhash","params":[0]}' \
        "http://127.0.0.1:20999/" 2>/dev/null || true)"
  got=$(printf '%s' "$rpc" | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F]\{64\}\)".*/\1/p')
  if [[ "$got" =~ ^[0-9a-fA-F]{64}$ ]]; then 
    genesis="$got"
    echo "✅ Got genesis from RPC: $genesis"
  else
    echo "⚠️  RPC unavailable, using hardcoded genesis"
  fi
else
  echo "⚠️  No cookie file, using hardcoded genesis"
fi

# Normalize to uppercase (portable way)
genesis_upper=$(echo "$genesis" | tr '[:lower:]' '[:upper:]')
echo "Genesis (normalized): $genesis_upper"
echo ""

echo "**Current meta table:**"
sqlite3 "$blockchain_db" "SELECT key, CASE key 
                                WHEN 'besthash' THEN hex(value)
                                WHEN 'genesis_hash' THEN hex(value)
                                WHEN 'chainwork' THEN hex(value)
                                ELSE value END AS value
               FROM meta ORDER BY key;" 2>/dev/null || echo "No meta table"

echo ""
echo "**Applying idempotent updates...**"

sqlite3 "$blockchain_db" <<'SQL'
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
SQL

sqlite3 "$blockchain_db" <<SQL
BEGIN;
INSERT INTO meta(key,value) VALUES('network','$network')
  ON CONFLICT(key) DO UPDATE SET value=excluded.value;

-- Only set genesis_hash/besthash if missing
INSERT INTO meta(key,value) SELECT 'genesis_hash','$genesis_upper'
  WHERE NOT EXISTS (SELECT 1 FROM meta WHERE key='genesis_hash');

INSERT INTO meta(key,value) SELECT 'besthash','$genesis_upper'
  WHERE NOT EXISTS (SELECT 1 FROM meta WHERE key='besthash');

-- Initialize height/chainwork if missing
INSERT INTO meta(key,value) SELECT 'height','0'
  WHERE NOT EXISTS (SELECT 1 FROM meta WHERE key='height');

INSERT INTO meta(key,value) SELECT 'chainwork','0000000000000000000000000000000000000000000000000000000000000000'
  WHERE NOT EXISTS (SELECT 1 FROM meta WHERE key='chainwork');
COMMIT;
SQL

echo "✅ meta fixed for $network in $blockchain_db"
echo ""

echo "**Updated meta table:**"
sqlite3 "$blockchain_db" "SELECT key, CASE key 
                                WHEN 'besthash' THEN hex(value)
                                WHEN 'genesis_hash' THEN hex(value)
                                WHEN 'chainwork' THEN hex(value)
                                ELSE value END AS value
               FROM meta ORDER BY key;"

echo ""
echo "🎊 Genesis hash and besthash are now properly set!"
echo ""
echo "**Next steps:**"
echo "1. Integrate db_init.hpp into daemon startup"
echo "2. Implement getblockhash RPC method"  
echo "3. Update migration scripts to use RPC for genesis hash"
