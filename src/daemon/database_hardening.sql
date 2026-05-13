-- SQLite Database Hardening for Dinero
-- ====================================

-- Enable WAL mode for better concurrency
PRAGMA journal_mode=WAL;

-- Use NORMAL synchronous mode (good balance of safety/performance)
PRAGMA synchronous=NORMAL;

-- Set busy timeout to handle concurrent access
PRAGMA busy_timeout=5000;

-- Enable foreign key constraints
PRAGMA foreign_keys=ON;

-- Optimize for performance
PRAGMA temp_store=MEMORY;
PRAGMA mmap_size=268435456; -- 256MB

-- Add unique constraints to prevent duplicates
CREATE UNIQUE INDEX IF NOT EXISTS idx_block_hash_unique ON block_index(block_hash);
CREATE UNIQUE INDEX IF NOT EXISTS idx_height_unique ON block_index(height);

-- Add indexes for performance
CREATE INDEX IF NOT EXISTS idx_block_timestamp ON block_index(timestamp);
CREATE INDEX IF NOT EXISTS idx_block_chainwork ON block_index(chainwork);

-- Ensure meta table has required keys
INSERT OR IGNORE INTO meta (key, value) VALUES ('besthash', '');
INSERT OR IGNORE INTO meta (key, value) VALUES ('height', '0');
INSERT OR IGNORE INTO meta (key, value) VALUES ('chainwork', '0000000000000000000000000000000000000000000000000000000000000001');

-- Create a view for easy tip access
CREATE VIEW IF NOT EXISTS current_tip AS
SELECT 
    hash as tip_hash,
    height as tip_height,
    timestamp as tip_timestamp,
    chainwork as tip_chainwork
FROM block_index 
WHERE hash = (SELECT value FROM meta WHERE key = 'besthash');

-- Create a function to safely update tip
CREATE TRIGGER IF NOT EXISTS update_tip_after_insert
AFTER INSERT ON block_index
BEGIN
    UPDATE meta SET value = NEW.hash WHERE key = 'besthash';
    UPDATE meta SET value = NEW.height WHERE key = 'height';
    UPDATE meta SET value = NEW.chainwork WHERE key = 'chainwork';
END;
