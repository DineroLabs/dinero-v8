-- Chainstate hardening migration - enforce UNIQUE constraints and add indexes
-- Migration 002: UTXO integrity and performance improvements

-- Ensure UTXO table has proper UNIQUE constraint (already exists in 001, but make it explicit)
-- The PRIMARY KEY (txid, vout) already enforces uniqueness, but let's add explicit constraints

-- Add covering indexes for common query patterns
CREATE INDEX IF NOT EXISTS idx_utxo_script ON utxo(script);
CREATE INDEX IF NOT EXISTS idx_utxo_height ON utxo(height);
CREATE INDEX IF NOT EXISTS idx_utxo_coinbase ON utxo(coinbase);
CREATE INDEX IF NOT EXISTS idx_utxo_amount ON utxo(amount);

-- Add composite indexes for wallet queries
CREATE INDEX IF NOT EXISTS idx_utxo_script_height ON utxo(script, height);
CREATE INDEX IF NOT EXISTS idx_utxo_script_coinbase ON utxo(script, coinbase);

-- Add integrity check constraints (SQLite doesn't support CHECK constraints on existing tables)
-- We'll enforce these in application code, but document them here:
-- CONSTRAINT utxo_amount_positive CHECK (amount > 0)
-- CONSTRAINT utxo_height_positive CHECK (height >= 0)
-- CONSTRAINT utxo_coinbase_boolean CHECK (coinbase IN (0, 1))

-- Add metadata for migration tracking
INSERT OR REPLACE INTO meta (key, value) VALUES ('migration_002_applied', '1');
INSERT OR REPLACE INTO meta (key, value) VALUES ('migration_002_timestamp', CAST(strftime('%s', 'now') AS TEXT));
