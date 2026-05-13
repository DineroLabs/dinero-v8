-- ═══════════════════════════════════════════════════════════════════════════
-- DineroCoin Lightning Network Database Schema
-- ═══════════════════════════════════════════════════════════════════════════
-- Per-Wallet Lightning State (Off-Chain)
-- Each wallet has its own Lightning node identity and channels
-- Storage: ~/.dinero/wallets/{wallet_name}/lightning.db
-- ═══════════════════════════════════════════════════════════════════════════

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA trusted_schema = OFF;

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning Channels (BOLT-compliant)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_channels (
    channel_id TEXT PRIMARY KEY,               -- 32-byte hex channel ID
    peer_node_id TEXT NOT NULL,                -- Remote peer's node pubkey (33-byte hex)

    -- Funding
    funding_txid TEXT NOT NULL,                -- Funding transaction hash
    funding_vout INTEGER NOT NULL DEFAULT 0,   -- Output index in funding tx
    funding_amount_una INTEGER NOT NULL,      -- Total channel capacity

    -- Balances (in milli-una)
    local_balance_muna INTEGER NOT NULL,      -- Our balance
    remote_balance_muna INTEGER NOT NULL,     -- Peer's balance

    -- Channel state
    state INTEGER NOT NULL DEFAULT 0,          -- ChannelState enum value
    commitment_number INTEGER NOT NULL DEFAULT 0,  -- Current commitment version
    revocation_secret TEXT,                    -- Current revocation secret (hex)

    -- Taproot keys (hex-encoded)
    local_funding_key TEXT NOT NULL,           -- Our funding pubkey
    remote_funding_key TEXT NOT NULL,          -- Peer's funding pubkey
    revocation_basepoint TEXT,                 -- Revocation key base

    -- HD wallet key derivation
    local_key_index INTEGER NOT NULL DEFAULT 0,  -- HD wallet derivation index

    -- Metadata
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    last_update INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    is_initiator INTEGER NOT NULL DEFAULT 0,   -- 1 if we opened
    to_self_delay INTEGER NOT NULL DEFAULT 144,  -- CSV delay (blocks)
    dust_limit_una INTEGER NOT NULL DEFAULT 546  -- Minimum output value
);

CREATE INDEX IF NOT EXISTS idx_ln_channels_state ON ln_channels(state);
CREATE INDEX IF NOT EXISTS idx_ln_channels_peer ON ln_channels(peer_node_id);

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning HTLCs (Hash Time-Locked Contracts)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_htlcs (
    htlc_id TEXT PRIMARY KEY,                  -- Unique HTLC identifier
    channel_id TEXT NOT NULL,                  -- Channel this HTLC belongs to
    amount_muna INTEGER NOT NULL,             -- HTLC amount in milli-una
    payment_hash TEXT NOT NULL,                -- 32-byte payment hash (hex)
    cltv_expiry INTEGER NOT NULL DEFAULT 0,    -- CheckLockTimeVerify expiry height
    is_incoming INTEGER NOT NULL DEFAULT 0,    -- 1 = incoming, 0 = outgoing

    -- Routing information
    next_hop TEXT,                             -- Next channel_id in payment route
    prev_hop TEXT,                             -- Previous channel_id in payment route

    -- State tracking
    state INTEGER NOT NULL DEFAULT 0,          -- HTLC::State enum value
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),

    FOREIGN KEY (channel_id) REFERENCES ln_channels(channel_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ln_htlcs_channel ON ln_htlcs(channel_id);
CREATE INDEX IF NOT EXISTS idx_ln_htlcs_payment_hash ON ln_htlcs(payment_hash);
CREATE INDEX IF NOT EXISTS idx_ln_htlcs_state ON ln_htlcs(state);

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning Commitment Transactions
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_commitments (
    commitment_id TEXT PRIMARY KEY,            -- Unique commitment identifier
    channel_id TEXT NOT NULL,                  -- Channel this commitment belongs to
    commitment_num INTEGER NOT NULL DEFAULT 0, -- Commitment transaction number
    local_sig TEXT,                            -- Local signature (hex)
    remote_sig TEXT,                           -- Remote signature (hex)
    tx_data TEXT,                              -- Serialized transaction (hex)
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),

    FOREIGN KEY (channel_id) REFERENCES ln_channels(channel_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ln_commitments_channel ON ln_commitments(channel_id);
CREATE INDEX IF NOT EXISTS idx_ln_commitments_num ON ln_commitments(channel_id, commitment_num);

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning Peers
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_peers (
    node_id TEXT PRIMARY KEY,                  -- 33-byte compressed pubkey (hex)
    address TEXT,                              -- IP:port or onion address
    last_seen INTEGER NOT NULL DEFAULT 0,      -- Unix timestamp
    trusted INTEGER NOT NULL DEFAULT 0         -- Manual trust flag
);

CREATE INDEX IF NOT EXISTS idx_ln_peers_last_seen ON ln_peers(last_seen);

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning Invoices (BOLT #11)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_invoices (
    payment_hash TEXT PRIMARY KEY,             -- 32-byte payment hash (hex)
    bolt11_string TEXT NOT NULL,               -- Encoded BOLT #11 invoice
    preimage TEXT,                             -- 32-byte payment preimage (hex)

    -- Amount and description
    amount_muna INTEGER NOT NULL DEFAULT 0,   -- 0 = "any amount" invoice
    description TEXT,                          -- Payment description

    -- Timing
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    expires_at INTEGER NOT NULL DEFAULT 0,     -- Unix timestamp when expires

    -- Status
    status INTEGER NOT NULL DEFAULT 0,         -- InvoiceStatus enum value
    paid_at INTEGER NOT NULL DEFAULT 0,        -- Unix timestamp when paid
    paid_by_channel TEXT,                      -- Channel ID used for payment

    -- Metadata
    label TEXT,                                -- User-defined label
    tags TEXT,                                 -- JSON array of user-defined tags

    FOREIGN KEY (paid_by_channel) REFERENCES ln_channels(channel_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_ln_invoices_status ON ln_invoices(status);
CREATE INDEX IF NOT EXISTS idx_ln_invoices_expires ON ln_invoices(expires_at);
CREATE INDEX IF NOT EXISTS idx_ln_invoices_label ON ln_invoices(label);

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning Payments (Outgoing Payment History)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_payments (
    payment_hash TEXT PRIMARY KEY,             -- 32-byte payment hash (hex)
    bolt11_string TEXT,                        -- Original BOLT #11 invoice (if paid via invoice)
    destination_node_id TEXT NOT NULL,         -- Destination node pubkey (hex)

    -- Amount and fees
    amount_muna INTEGER NOT NULL DEFAULT 0,   -- Payment amount in milli-una
    fee_muna INTEGER NOT NULL DEFAULT 0,      -- Total routing fees paid

    -- Status
    status INTEGER NOT NULL DEFAULT 0,         -- 0=pending, 1=succeeded, 2=failed
    failure_reason TEXT,                       -- Failure reason (empty if succeeded)
    attempts INTEGER NOT NULL DEFAULT 0,       -- Number of payment attempts

    -- Timing
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    completed_at INTEGER NOT NULL DEFAULT 0,   -- Unix timestamp when completed (0 = pending)

    -- Proof of payment
    preimage TEXT,                             -- 32-byte preimage (hex) - empty if failed

    -- Route information
    route_json TEXT,                           -- JSON-encoded route taken (channel_ids, fees per hop)
    route_hops INTEGER NOT NULL DEFAULT 0,     -- Number of hops in route

    -- Metadata
    label TEXT,                                -- User-defined label
    tags TEXT                                  -- JSON array of user-defined tags
);

CREATE INDEX IF NOT EXISTS idx_ln_payments_status ON ln_payments(status);
CREATE INDEX IF NOT EXISTS idx_ln_payments_destination ON ln_payments(destination_node_id);
CREATE INDEX IF NOT EXISTS idx_ln_payments_created ON ln_payments(created_at);
CREATE INDEX IF NOT EXISTS idx_ln_payments_label ON ln_payments(label);

-- ═══════════════════════════════════════════════════════════════════════════
-- Lightning Revocation Secrets (Encrypted)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_secrets (
    channel_id TEXT NOT NULL,                  -- Channel this secret belongs to
    commitment_num INTEGER NOT NULL,           -- Commitment number
    secret_data BLOB NOT NULL,                 -- Encrypted revocation secret
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),

    PRIMARY KEY (channel_id, commitment_num),
    FOREIGN KEY (channel_id) REFERENCES ln_channels(channel_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ln_secrets_channel ON ln_secrets(channel_id);

-- ═══════════════════════════════════════════════════════════════════════════
-- Watchtower Metadata (Breach Detection)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE IF NOT EXISTS ln_watchtower (
    channel_id TEXT NOT NULL,                  -- Channel being watched
    breach_txid TEXT,                          -- Detected breach transaction
    penalty_tx BLOB,                           -- Pre-signed penalty transaction
    last_check INTEGER NOT NULL DEFAULT 0,     -- Last time channel was checked
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),

    PRIMARY KEY (channel_id),
    FOREIGN KEY (channel_id) REFERENCES ln_channels(channel_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ln_watchtower_last_check ON ln_watchtower(last_check);

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
-- Run ANALYZE to update query planner statistics
-- ═══════════════════════════════════════════════════════════════════════════

ANALYZE;
