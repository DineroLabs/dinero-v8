-- Explorer cache (explorer.db) schema - read-optimized UI cache
-- Safe to rebuild/drop - all data derivable from chainstate

PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS blocks (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  hash      BLOB UNIQUE NOT NULL,
  height    INTEGER NOT NULL,
  time      INTEGER NOT NULL,
  version   INTEGER NOT NULL,
  merkle    BLOB NOT NULL,
  prevhash  BLOB NOT NULL,
  bits      INTEGER NOT NULL,
  nonce     INTEGER NOT NULL,
  size      INTEGER NOT NULL,
  weight    INTEGER NOT NULL,
  tx_count  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_blocks_height ON blocks(height);

CREATE TABLE IF NOT EXISTS txs (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  txid      BLOB UNIQUE NOT NULL,
  block_id  INTEGER NOT NULL REFERENCES blocks(id) ON DELETE CASCADE,
  vin_count INTEGER NOT NULL,
  vout_count INTEGER NOT NULL,
  vsize     INTEGER NOT NULL,
  weight    INTEGER NOT NULL,
  locktime  INTEGER NOT NULL,
  version   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_txs_block_id ON txs(block_id);

CREATE TABLE IF NOT EXISTS vin (
  tx_id     INTEGER NOT NULL REFERENCES txs(id) ON DELETE CASCADE,
  n         INTEGER NOT NULL,
  prev_txid BLOB NOT NULL,
  prev_vout INTEGER NOT NULL,
  scriptsig BLOB NOT NULL,
  sequence  INTEGER NOT NULL,
  witness   BLOB,
  PRIMARY KEY (tx_id, n)
);

CREATE TABLE IF NOT EXISTS vout (
  tx_id      INTEGER NOT NULL REFERENCES txs(id) ON DELETE CASCADE,
  n          INTEGER NOT NULL,
  value_sat  INTEGER NOT NULL,
  scriptpubkey BLOB NOT NULL,
  type       TEXT,
  address    TEXT,
  PRIMARY KEY (tx_id, n)
);

CREATE TABLE IF NOT EXISTS addr_txs (
  address TEXT NOT NULL,
  tx_id   INTEGER NOT NULL REFERENCES txs(id) ON DELETE CASCADE,
  PRIMARY KEY (address, tx_id)
);
CREATE INDEX IF NOT EXISTS idx_addr_txs_addr ON addr_txs(address);
