-- Migration 004: Meta table bootstrap
-- Ensures meta table exists and has basic network entry
-- This is idempotent and safe to run multiple times

PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;

-- Create meta table if it doesn't exist
CREATE TABLE IF NOT EXISTS meta (
  key   TEXT PRIMARY KEY,
  value BLOB NOT NULL
);

-- Insert network placeholder if missing (will be updated by daemon)
-- This prevents the daemon from failing on completely fresh databases
INSERT INTO meta(key, value) 
SELECT 'network', 'unknown'
WHERE NOT EXISTS (SELECT 1 FROM meta WHERE key='network');

-- Insert schema version if missing
INSERT INTO meta(key, value)
SELECT 'schema_version', '1'
WHERE NOT EXISTS (SELECT 1 FROM meta WHERE key='schema_version');

-- Note: genesis_hash, besthash, height, chainwork will be set by daemon
-- via EnsureGenesisMeta() during startup
