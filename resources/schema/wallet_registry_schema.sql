-- ═══════════════════════════════════════════════════════════════════════════
-- DineroCoin Wallet Registry Database Schema
-- ═══════════════════════════════════════════════════════════════════════════
-- Small, lightweight registry that tracks all wallets
-- Does NOT contain keys, seeds, UTXOs, or sensitive data
-- Only metadata for wallet discovery and selection
-- Location: ~/.dinero/wallet_registry.db
-- ═══════════════════════════════════════════════════════════════════════════

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA trusted_schema = OFF;

-- ═══════════════════════════════════════════════════════════════════════════
-- Wallet Registry (main table)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS wallets (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    path TEXT NOT NULL UNIQUE,  -- Path to wallet_<name>.db file
    network TEXT NOT NULL DEFAULT 'mainnet',
    encrypted INTEGER NOT NULL DEFAULT 0,
    fingerprint BLOB,  -- BIP32 master fingerprint (4 bytes) for identification
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    last_opened INTEGER  -- Unix timestamp of last access
);

CREATE INDEX IF NOT EXISTS idx_registry_name ON wallets(name);
CREATE INDEX IF NOT EXISTS idx_registry_encrypted ON wallets(encrypted);
CREATE INDEX IF NOT EXISTS idx_registry_last_opened ON wallets(last_opened);

-- ═══════════════════════════════════════════════════════════════════════════
-- Schema Version Tracking
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

INSERT OR IGNORE INTO schema_version (version) VALUES (1);

-- ═══════════════════════════════════════════════════════════════════════════
-- Run ANALYZE
-- ═══════════════════════════════════════════════════════════════════════════
ANALYZE;
