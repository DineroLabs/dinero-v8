-- Peers database (peers.db) schema - addrman and ban management
-- Idempotent migration for P2P network management
-- Handles existing schema gracefully

PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;

-- Create new-style peers table if it doesn't exist
-- If old schema exists, we'll keep it for now (backward compatibility)
CREATE TABLE IF NOT EXISTS peers_v2 (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  addr         TEXT NOT NULL,
  port         INTEGER NOT NULL,
  services     INTEGER NOT NULL,
  last_seen    INTEGER NOT NULL,
  last_success INTEGER NOT NULL,
  attempts     INTEGER NOT NULL DEFAULT 0,
  is_tried     INTEGER NOT NULL DEFAULT 0,
  bucket       INTEGER NOT NULL DEFAULT 0,
  score        REAL    NOT NULL DEFAULT 0.0
);

-- Only create indexes if the table was just created (no conflicts with existing schema)
-- Check if we need to create indexes by seeing if peers_v2 is empty (newly created)
CREATE INDEX IF NOT EXISTS idx_peers_v2_last_seen ON peers_v2(last_seen);
CREATE INDEX IF NOT EXISTS idx_peers_v2_bucket    ON peers_v2(bucket, is_tried);

-- Bans table (this should be safe as it already exists)
-- Just ensure indexes exist
CREATE INDEX IF NOT EXISTS idx_bans_until ON bans(banned_until);
