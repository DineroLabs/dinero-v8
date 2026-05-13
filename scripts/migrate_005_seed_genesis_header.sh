#!/usr/bin/env bash
set -Eeuo pipefail
REPO="${REPO:-/Users/haydarevich/Documents/DineroCoin}"
NET="${NET:-regtest}"
DATADIR="${DATADIR:-$REPO/data}"
DB="$DATADIR/$NET/blockchain.db"

die(){ echo "❌ $*" >&2; exit 1; }
need(){ command -v "$1" >/dev/null || die "Missing dependency: $1"; }
need sqlite3

[[ -f "$DB" ]] || die "DB not found: $DB (start dinerod once to create it)"

# Read genesis (lowercase) from meta
GENESIS=$(sqlite3 "$DB" "select lower(value) from meta where key='genesis_hash';")
[[ -n "$GENESIS" ]] || die "genesis_hash missing in meta"

# Ensure headers table exists (minimal schema; safe if already exists)
sqlite3 "$DB" "
CREATE TABLE IF NOT EXISTS headers(
  hash TEXT PRIMARY KEY,
  height INTEGER NOT NULL,
  prevhash TEXT NOT NULL,
  merkle TEXT,
  time INTEGER,
  bits INTEGER,
  nonce INTEGER,
  raw BLOB
);
CREATE INDEX IF NOT EXISTS headers_height_idx ON headers(height);
" >/dev/null

# If a row for height=0 or hash=genesis already exists, no-op
HAS=$(sqlite3 "$DB" "SELECT 1 FROM headers WHERE hash='$GENESIS' OR height=0 LIMIT 1;")
if [[ "$HAS" == "1" ]]; then
  echo "🟢 Genesis header already present — no-op"
  exit 0
fi

# Seed constants from your logs/chainparams (regtest)
ZEROS=$(printf '%064d' 0)
NTIME=1700000000          # Genesis nTime (regtest)
NBITS_DEC=553713663       # 0x2100ffff in decimal
NNONCE=0

sqlite3 "$DB" "
INSERT INTO headers(hash,height,prevhash,merkle,time,bits,nonce)
SELECT '$GENESIS', 0, '$ZEROS', '$ZEROS', $NTIME, $NBITS_DEC, $NNONCE
WHERE NOT EXISTS (SELECT 1 FROM headers WHERE hash='$GENESIS' OR height=0);
" >/dev/null

echo "✅ Seeded genesis header: $GENESIS (height 0)"
