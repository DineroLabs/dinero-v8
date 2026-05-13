-- ═══════════════════════════════════════════════════════════════════════════
-- DineroCoin Per-Wallet Database Schema
-- ═══════════════════════════════════════════════════════════════════════════
-- Each wallet gets its own isolated database file: wallet_<name>.db
-- This provides true wallet isolation, clean encryption, and Lightning compatibility
-- Based on Bitcoin Core's one-wallet-per-DB design
-- ═══════════════════════════════════════════════════════════════════════════

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA trusted_schema = OFF;

-- ═══════════════════════════════════════════════════════════════════════════
-- Wallet Metadata (single row per wallet)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS wallet_meta (
    id INTEGER PRIMARY KEY CHECK (id = 1),  -- Enforce single row
    name TEXT NOT NULL,
    network TEXT NOT NULL DEFAULT 'mainnet',  -- mainnet, testnet, regtest
    encrypted INTEGER NOT NULL DEFAULT 0,
    fingerprint BLOB,  -- BIP32 master fingerprint (4 bytes)
    wallet_policy TEXT NOT NULL DEFAULT 'bip86',  -- Wallet derivation policy: 'bip84' or 'bip86'
    birthday_height INTEGER,  -- Chain height at wallet creation (for rescan optimization)
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    version INTEGER NOT NULL DEFAULT 1
);

-- ═══════════════════════════════════════════════════════════════════════════
-- Encryption Metadata (Argon2id + AES-256-GCM)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS encryption_metadata (
    id INTEGER PRIMARY KEY CHECK (id = 1),  -- Enforce single row
    encrypted INTEGER NOT NULL DEFAULT 0,
    kdf TEXT NOT NULL DEFAULT 'argon2id',
    kdf_iterations INTEGER,      -- Argon2id time cost (3 recommended)
    kdf_memory_kb INTEGER,        -- Argon2id memory (65536 = 64 MB)
    kdf_parallelism INTEGER,      -- Argon2id parallelism (1 recommended)
    cipher TEXT NOT NULL DEFAULT 'AES-256-GCM',
    salt BLOB,                    -- 16 bytes for Argon2id
    nonce BLOB,                   -- 12 bytes for AES-GCM
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- ═══════════════════════════════════════════════════════════════════════════
-- HD Wallet Seed Storage
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS hd_seeds (
    id INTEGER PRIMARY KEY CHECK (id = 1),  -- Enforce single row
    encrypted_seed BLOB NOT NULL,  -- For encrypted: ciphertext + tag. For unencrypted: raw seed
    salt BLOB NOT NULL,
    coin_type INTEGER NOT NULL DEFAULT 1447,
    encryption_version INTEGER NOT NULL DEFAULT 2,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- ═══════════════════════════════════════════════════════════════════════════
-- HD Addresses (BIP84/BIP86)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS addresses (
    id INTEGER PRIMARY KEY,
    wallet_id INTEGER NOT NULL DEFAULT 1,   -- Per-wallet DB always uses wallet_id=1
    account INTEGER NOT NULL DEFAULT 0,
    change INTEGER NOT NULL DEFAULT 0,
    idx INTEGER NOT NULL,
    address TEXT NOT NULL UNIQUE,
    pubkey BLOB,
    label TEXT,
    is_system_label INTEGER NOT NULL DEFAULT 0,  -- 1 if auto-generated (e.g., mining), 0 if user-set
    type TEXT NOT NULL DEFAULT 'p2wpkh' CHECK(type IN('p2wpkh','p2wsh','p2tr')),
    key_id BLOB,             -- Week 1: KeyID-based descriptor wallet (HASH160 of pubkey)
    internal_key_id BLOB,    -- Week 1: For Taproot internal key (before TapTweak)
    output_key_id BLOB,      -- Week 1: For Taproot output key (after TapTweak)
    script_pubkey TEXT,      -- Bitcoin-grade: scriptPubKey for ownership (not address strings)
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    UNIQUE(wallet_id, account, change, idx)
);

CREATE INDEX IF NOT EXISTS idx_addr_wallet ON addresses(wallet_id);
CREATE INDEX IF NOT EXISTS idx_addr_path ON addresses(wallet_id, account, change, idx);
CREATE INDEX IF NOT EXISTS idx_addr_bech32 ON addresses(address);
CREATE INDEX IF NOT EXISTS idx_addr_label ON addresses(wallet_id, label);
CREATE INDEX IF NOT EXISTS idx_addr_key_id ON addresses(key_id);
CREATE INDEX IF NOT EXISTS idx_addr_output_key_id ON addresses(output_key_id);

-- ═══════════════════════════════════════════════════════════════════════════
-- Address Derivation Paths (HD wallet tracking)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS address_derivation_paths (
    address TEXT PRIMARY KEY,
    derivation_path TEXT NOT NULL,
    account INTEGER NOT NULL DEFAULT 0,
    change INTEGER NOT NULL DEFAULT 0,
    address_index INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS idx_derivation_path ON address_derivation_paths(derivation_path);
CREATE INDEX IF NOT EXISTS idx_derivation_account_change ON address_derivation_paths(account, change, address_index);

-- ═══════════════════════════════════════════════════════════════════════════
-- UTXOs (wallet-specific)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS utxos (
    id INTEGER PRIMARY KEY,
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    amount INTEGER NOT NULL,
    address TEXT NOT NULL,
    script_pubkey TEXT NOT NULL,
    height INTEGER NOT NULL DEFAULT 0,
    is_coinbase INTEGER NOT NULL DEFAULT 0,
    is_spent INTEGER NOT NULL DEFAULT 0,
    spent_txid TEXT,
    spent_height INTEGER,
    confirmations INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    UNIQUE(txid, vout)
);

CREATE INDEX IF NOT EXISTS idx_utxos_spent_height ON utxos(is_spent, height);
CREATE INDEX IF NOT EXISTS idx_utxos_spent_coinbase_height ON utxos(is_spent, is_coinbase, height);
CREATE INDEX IF NOT EXISTS idx_utxos_spent_coinbase ON utxos(is_spent, is_coinbase);
CREATE INDEX IF NOT EXISTS idx_utxos_address ON utxos(address);

-- ═══════════════════════════════════════════════════════════════════════════
-- Transactions (wallet history)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS transactions (
    id INTEGER PRIMARY KEY,
    wallet_id INTEGER NOT NULL DEFAULT 1,
    txid TEXT NOT NULL,
    address TEXT NOT NULL DEFAULT '',
    amount REAL NOT NULL DEFAULT 0,
    confirmations INTEGER NOT NULL DEFAULT 0,
    category TEXT NOT NULL DEFAULT 'unknown',
    label TEXT,
    time INTEGER NOT NULL,
    is_coinbase INTEGER NOT NULL DEFAULT 0,
    height INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    UNIQUE(wallet_id, txid, address, category)
);

CREATE INDEX IF NOT EXISTS idx_transactions_wallet_id ON transactions(wallet_id);
CREATE INDEX IF NOT EXISTS idx_transactions_address ON transactions(address);
CREATE INDEX IF NOT EXISTS idx_transactions_category ON transactions(category);
CREATE INDEX IF NOT EXISTS idx_transactions_time ON transactions(time DESC);
CREATE INDEX IF NOT EXISTS idx_transactions_height ON transactions(height);

-- ═══════════════════════════════════════════════════════════════════════════
-- Transaction I/O (inputs and outputs)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS tx_io (
    id INTEGER PRIMARY KEY,
    txid TEXT NOT NULL,
    vout INTEGER,
    vin INTEGER,
    address TEXT,
    amount INTEGER,
    category TEXT CHECK(category IN('send', 'receive', 'generate')),
    label TEXT,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS idx_txio_txid ON tx_io(txid);
CREATE INDEX IF NOT EXISTS idx_txio_address ON tx_io(address);

-- ═══════════════════════════════════════════════════════════════════════════
-- Watch Scripts (for blockchain scanning)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS watch_scripts (
    script_pubkey BLOB PRIMARY KEY,
    path TEXT,
    is_change INTEGER NOT NULL DEFAULT 0,
    last_seen_height INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS idx_watch_change ON watch_scripts(is_change);
CREATE INDEX IF NOT EXISTS idx_watch_height ON watch_scripts(last_seen_height);

-- ═══════════════════════════════════════════════════════════════════════════
-- Blockchain Tip (current chain height)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS tip (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    height INTEGER NOT NULL DEFAULT 0,
    block_hash TEXT,
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- ═══════════════════════════════════════════════════════════════════════════
-- Settings (wallet-specific configuration)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- ═══════════════════════════════════════════════════════════════════════════
-- Wallet UTXO Tracker (NEW - Bitcoin Core-compatible)
-- ═══════════════════════════════════════════════════════════════════════════
-- Tracks which UTXOs from GlobalUTXOSet (RocksDB) belong to this wallet
-- Replaces legacy 'utxos' table with cleaner separation of concerns

CREATE TABLE IF NOT EXISTS wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,          -- BIP32 path (e.g., "m/84'/1447'/0'/0/5")
    cached_amount INTEGER NOT NULL,         -- Cached from GlobalUTXOSet (RocksDB)
    cached_height INTEGER NOT NULL,         -- Cached from GlobalUTXOSet (RocksDB)
    is_spent INTEGER NOT NULL DEFAULT 0,    -- Spent by this wallet
    is_locked INTEGER NOT NULL DEFAULT 0,   -- Locked for coin control
    label TEXT,                             -- User-defined label
    PRIMARY KEY (txid, vout)
);

CREATE INDEX IF NOT EXISTS idx_wallet_utxos_unspent ON wallet_utxos(is_spent) WHERE is_spent = 0;
CREATE INDEX IF NOT EXISTS idx_wallet_utxos_path ON wallet_utxos(derivation_path);
CREATE INDEX IF NOT EXISTS idx_wallet_utxos_height ON wallet_utxos(cached_height);

-- ═══════════════════════════════════════════════════════════════════════════
-- NOTE: Lightning Network tables moved to lightning.db
-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning state is now in a separate database per wallet:
--   ~/.dinero/wallets/{wallet_name}/lightning.db
--
-- This provides:
--   - Clear separation: on-chain (wallet.db) vs off-chain (lightning.db)
--   - Independent backups
--   - Lightning can be disabled per wallet
--   - Easier to implement wallet without Lightning
--
-- See: resources/schema/lightning_schema.sql

-- ═══════════════════════════════════════════════════════════════════════════
-- Schema Version Tracking
-- ═══════════════════════════════════════════════════════════════════════════
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- Set initial schema version
INSERT OR IGNORE INTO schema_version (version) VALUES (1);

-- ═══════════════════════════════════════════════════════════════════════════
-- Imported Descriptors (watch-only wallet support)
-- ═══════════════════════════════════════════════════════════════════════════
-- Stores descriptors imported via wallet.importdescriptors RPC
-- Enables watch-only wallets, hardware wallet integration, multi-wallet coordination
CREATE TABLE IF NOT EXISTS imported_descriptors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    descriptor TEXT NOT NULL UNIQUE,           -- Full descriptor with checksum
    descriptor_type TEXT NOT NULL,             -- 'wpkh', 'tr', 'wsh', etc.
    internal INTEGER NOT NULL DEFAULT 0,       -- 0 = receive, 1 = change
    active INTEGER NOT NULL DEFAULT 0,         -- 0 = watch-only, 1 = active (future: signing)
    range_start INTEGER NOT NULL DEFAULT 0,    -- Start of derivation range
    range_end INTEGER NOT NULL DEFAULT 1000,   -- End of derivation range (gap limit * 50)
    next_index INTEGER NOT NULL DEFAULT 0,     -- Next unused address index
    timestamp INTEGER,                         -- Import timestamp (or 'now' for rescan from tip)
    label TEXT,                                -- Optional label for this descriptor
    fingerprint TEXT,                          -- Master fingerprint (extracted from descriptor)

    -- Metadata
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS idx_imported_descriptors_active ON imported_descriptors(active);
CREATE INDEX IF NOT EXISTS idx_imported_descriptors_internal ON imported_descriptors(internal);
CREATE INDEX IF NOT EXISTS idx_imported_descriptors_fingerprint ON imported_descriptors(fingerprint);

-- Mapping between imported descriptors and derived addresses
CREATE TABLE IF NOT EXISTS imported_descriptor_addresses (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    descriptor_id INTEGER NOT NULL REFERENCES imported_descriptors(id) ON DELETE CASCADE,
    address_index INTEGER NOT NULL,            -- Derivation index (e.g., 0-1000)
    address TEXT NOT NULL UNIQUE,              -- Derived address (bech32/bech32m)
    script_pubkey BLOB NOT NULL,               -- scriptPubKey for ownership checks
    key_id BLOB,                               -- KeyID (HASH160 of pubkey) for IsMine

    -- Taproot-specific fields (BIP86)
    internal_key_id BLOB,                      -- Internal key (before tweak)
    output_key_id BLOB,                        -- Output key (after tweak)

    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    UNIQUE(descriptor_id, address_index)
);

CREATE INDEX IF NOT EXISTS idx_imported_addresses_descriptor ON imported_descriptor_addresses(descriptor_id);
CREATE INDEX IF NOT EXISTS idx_imported_addresses_address ON imported_descriptor_addresses(address);
CREATE INDEX IF NOT EXISTS idx_imported_addresses_script ON imported_descriptor_addresses(script_pubkey);
CREATE INDEX IF NOT EXISTS idx_imported_addresses_key_id ON imported_descriptor_addresses(key_id);
CREATE INDEX IF NOT EXISTS idx_imported_addresses_output_key_id ON imported_descriptor_addresses(output_key_id);

-- ═══════════════════════════════════════════════════════════════════════════
-- B2: Policy descriptors for Taproot template outputs
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS wallet_policies (
    policy_id BLOB PRIMARY KEY,                  -- 32-byte deterministic policy ID
    template_type INTEGER NOT NULL,              -- 0=STANDARD, 1=PROTECTED, 2=ESCROW
    template_version INTEGER NOT NULL DEFAULT 1, -- Template version (LE uint16)
    params BLOB NOT NULL,                        -- Serialized template params
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    label TEXT,                                  -- Human-readable label
    is_active INTEGER NOT NULL DEFAULT 1         -- Soft delete
);

CREATE TABLE IF NOT EXISTS utxo_policy (
    txid BLOB NOT NULL,                          -- 32-byte txid
    vout INTEGER NOT NULL,                       -- Output index
    policy_id BLOB NOT NULL,                     -- References wallet_policies
    PRIMARY KEY (txid, vout),
    FOREIGN KEY (policy_id) REFERENCES wallet_policies(policy_id)
);

CREATE INDEX IF NOT EXISTS idx_utxo_policy_pid ON utxo_policy(policy_id);
CREATE INDEX IF NOT EXISTS idx_wallet_policies_type ON wallet_policies(template_type);
CREATE INDEX IF NOT EXISTS idx_wallet_policies_active ON wallet_policies(is_active);

-- ═══════════════════════════════════════════════════════════════════════════
-- Run ANALYZE to update query planner statistics
-- ═══════════════════════════════════════════════════════════════════════════
ANALYZE;
