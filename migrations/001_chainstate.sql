-- Chainstate (blockchain.db) schema - optimized for multi-daemon safety
-- Idempotent migration for robust SQLite architecture

PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
PRAGMA temp_store=MEMORY;
PRAGMA mmap_size=268435456;

CREATE TABLE IF NOT EXISTS meta (
  key   TEXT PRIMARY KEY,
  value BLOB NOT NULL
);

-- required keys (set by script): schema_version, network, genesis_hash, besthash, height, chainwork

CREATE TABLE IF NOT EXISTS headers (
  hash       BLOB PRIMARY KEY,     -- 32B
  height     INTEGER NOT NULL,
  version    INTEGER NOT NULL,
  prevhash   BLOB NOT NULL,        -- 32B
  merkle     BLOB NOT NULL,        -- 32B
  time       INTEGER NOT NULL,
  bits       INTEGER NOT NULL,
  nonce      INTEGER NOT NULL,
  chainwork  BLOB NOT NULL,        -- 32B big-endian
  status     INTEGER NOT NULL,     -- bit flags
  file_no    INTEGER NOT NULL,
  file_pos   INTEGER NOT NULL,
  tx_count   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_headers_height ON headers(height);

CREATE TABLE IF NOT EXISTS block_index (
  height INTEGER PRIMARY KEY,
  hash   BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS utxo (
  txid     BLOB NOT NULL,
  vout     INTEGER NOT NULL,
  amount   INTEGER NOT NULL,       -- una
  script   BLOB NOT NULL,
  height   INTEGER NOT NULL,
  coinbase INTEGER NOT NULL,       -- 0/1
  PRIMARY KEY (txid, vout)
);

-- enable only if you need global tx lookups
CREATE TABLE IF NOT EXISTS txindex (
  txid     BLOB PRIMARY KEY,
  block    BLOB NOT NULL,          -- 32B
  file_no  INTEGER NOT NULL,
  file_pos INTEGER NOT NULL,
  tx_off   INTEGER NOT NULL,
  tx_len   INTEGER NOT NULL
);
