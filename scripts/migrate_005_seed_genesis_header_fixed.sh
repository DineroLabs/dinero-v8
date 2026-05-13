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

# Clear any existing headers data first (idempotent)
sqlite3 "$DB" "DELETE FROM headers WHERE height = 0;" >/dev/null

# Ensure headers table matches the RPC expectation (BLOB columns)
sqlite3 "$DB" "
DROP TABLE IF EXISTS headers;
CREATE TABLE headers(
  hash BLOB PRIMARY KEY,
  height INTEGER NOT NULL,
  version INTEGER NOT NULL,
  prevhash BLOB NOT NULL,
  merkle BLOB NOT NULL,
  time INTEGER NOT NULL,
  bits INTEGER NOT NULL,
  nonce INTEGER NOT NULL,
  chainwork BLOB NOT NULL,
  status INTEGER NOT NULL DEFAULT 0,
  file_no INTEGER NOT NULL DEFAULT 0,
  file_pos INTEGER NOT NULL DEFAULT 0,
  tx_count INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_headers_height ON headers(height);
" >/dev/null

# Convert hex strings to binary data
GENESIS_BLOB="X'$GENESIS'"
ZEROS_BLOB="X'$(printf '%064d' 0)'"
CHAINWORK_BLOB="X'0000000000000000000000000000000000000000000000000000000000000001'"

# Seed genesis header with proper BLOB data
sqlite3 "$DB" "
INSERT INTO headers(hash, height, version, prevhash, merkle, time, bits, nonce, chainwork, status, file_no, file_pos, tx_count)
VALUES (
  $GENESIS_BLOB,
  0,
  1,
  $ZEROS_BLOB,
  $ZEROS_BLOB,
  1700000000,
  553713663,
  0,
  $CHAINWORK_BLOB,
  0,
  0,
  0,
  1
);
" >/dev/null

echo "✅ Seeded genesis header with BLOB data: $GENESIS (height 0)"
