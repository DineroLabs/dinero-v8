# Explorer API v1 - SQLite Index Requirements

## Required SQLite Tables

### 1. Blocks Table
```sql
CREATE TABLE IF NOT EXISTS blocks (
    hash TEXT PRIMARY KEY,
    height INTEGER UNIQUE NOT NULL,
    time INTEGER NOT NULL,
    bits TEXT NOT NULL,
    nonce INTEGER NOT NULL,
    merkleroot TEXT NOT NULL,
    previousblockhash TEXT NOT NULL,
    header_bytes BLOB,
    tx_count INTEGER NOT NULL
);

CREATE INDEX idx_blocks_height ON blocks(height);
CREATE INDEX idx_blocks_time ON blocks(time);
```

### 2. Block-Transaction Mapping
```sql
CREATE TABLE IF NOT EXISTS block_tx (
    block_hash TEXT NOT NULL,
    tx_index INTEGER NOT NULL,
    txid TEXT NOT NULL,
    raw_bytes BLOB NOT NULL,
    PRIMARY KEY(block_hash, tx_index),
    FOREIGN KEY(block_hash) REFERENCES blocks(hash)
);

CREATE INDEX idx_block_tx_txid ON block_tx(txid);
```

### 3. Transaction Index
```sql
CREATE TABLE IF NOT EXISTS tx (
    txid TEXT PRIMARY KEY,
    block_hash TEXT,
    height INTEGER,
    time INTEGER,
    tx_index INTEGER,
    version INTEGER NOT NULL,
    locktime INTEGER NOT NULL,
    size INTEGER NOT NULL,
    vsize INTEGER NOT NULL,
    raw_bytes BLOB NOT NULL
);

CREATE INDEX idx_tx_block ON tx(block_hash, tx_index);
CREATE INDEX idx_tx_height ON tx(height);
```

### 4. Address/Script Index (Core for fast lookups)
```sql
-- UTXO tracking
CREATE TABLE IF NOT EXISTS addr_utxo (
    scripthash TEXT NOT NULL,
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    value INTEGER NOT NULL,
    height INTEGER NOT NULL,
    tx_index INTEGER NOT NULL,
    spk_hex TEXT NOT NULL,
    is_spent INTEGER DEFAULT 0,
    spent_txid TEXT,
    spent_height INTEGER,
    PRIMARY KEY(txid, vout)
);

CREATE INDEX idx_addr_utxo_scripthash ON addr_utxo(scripthash, height, tx_index);
CREATE INDEX idx_addr_utxo_unspent ON addr_utxo(scripthash, is_spent, height);
CREATE INDEX idx_addr_utxo_spent ON addr_utxo(spent_txid, spent_height) WHERE is_spent = 1;

-- Transaction history (pre-computed for speed)
CREATE TABLE IF NOT EXISTS addr_hist (
    scripthash TEXT NOT NULL,
    height INTEGER NOT NULL,
    tx_index INTEGER NOT NULL,
    txid TEXT NOT NULL,
    delta INTEGER NOT NULL, -- net change in una (can be negative)
    PRIMARY KEY(scripthash, height, tx_index)
);

CREATE INDEX idx_addr_hist_scripthash ON addr_hist(scripthash, height DESC, tx_index DESC);
```

### 5. Address Statistics Cache
```sql
CREATE TABLE IF NOT EXISTS addr_stats (
    scripthash TEXT PRIMARY KEY,
    received INTEGER DEFAULT 0,
    sent INTEGER DEFAULT 0,
    tx_count INTEGER DEFAULT 0,
    last_updated INTEGER DEFAULT 0
);
```

### 6. Mempool
```sql
CREATE TABLE IF NOT EXISTS mempool (
    txid TEXT PRIMARY KEY,
    first_seen INTEGER NOT NULL,
    fee_rate REAL NOT NULL,
    size INTEGER NOT NULL,
    vsize INTEGER NOT NULL,
    raw_bytes BLOB NOT NULL
);

CREATE INDEX idx_mempool_fee_rate ON mempool(fee_rate DESC);
CREATE INDEX idx_mempool_first_seen ON mempool(first_seen);
```

## Critical Indexing Logic

### When Processing a New Block:

1. **Insert Block Header**
```sql
INSERT INTO blocks (hash, height, time, bits, nonce, merkleroot, previousblockhash, header_bytes, tx_count)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
```

2. **For Each Transaction in Block**
```sql
-- Insert transaction
INSERT INTO tx (txid, block_hash, height, time, tx_index, version, locktime, size, vsize, raw_bytes)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);

-- Insert block-tx mapping
INSERT INTO block_tx (block_hash, tx_index, txid, raw_bytes)
VALUES (?, ?, ?, ?);
```

3. **For Each Transaction Output (New UTXOs)**
```sql
INSERT INTO addr_utxo (scripthash, txid, vout, value, height, tx_index, spk_hex, is_spent)
VALUES (?, ?, ?, ?, ?, ?, ?, 0);
```

4. **For Each Transaction Input (Spent UTXOs)**
```sql
UPDATE addr_utxo 
SET is_spent = 1, spent_txid = ?, spent_height = ?
WHERE txid = ? AND vout = ?;
```

5. **Update Address History**
```sql
-- For each affected scripthash, calculate net delta and insert
INSERT INTO addr_hist (scripthash, height, tx_index, txid, delta)
VALUES (?, ?, ?, ?, ?);
```

6. **Update Address Statistics**
```sql
-- Update received/sent/tx_count for each affected scripthash
UPDATE addr_stats 
SET received = received + ?, sent = sent + ?, tx_count = tx_count + 1, last_updated = ?
WHERE scripthash = ?;

-- Insert if not exists
INSERT OR IGNORE INTO addr_stats (scripthash, received, sent, tx_count, last_updated)
VALUES (?, ?, ?, 1, ?);
```

### Scripthash Calculation (Electrum Standard)
```cpp
std::string ScriptToElectrumScripthash(const std::string& scriptPubKey) {
    // 1. SHA256 of scriptPubKey bytes
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(scriptPubKey.data()), 
           scriptPubKey.length(), hash);
    
    // 2. Reverse bytes for little-endian
    std::reverse(hash, hash + SHA256_DIGEST_LENGTH);
    
    // 3. Convert to hex string
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}
```

## Query Patterns for API Endpoints

### Chain Tip
```sql
SELECT height, hash, time FROM blocks ORDER BY height DESC LIMIT 1;
```

### Block by Hash/Height
```sql
-- By hash
SELECT * FROM blocks WHERE hash = ?;

-- By height  
SELECT * FROM blocks WHERE height = ?;

-- Get transactions in block (verbosity >= 1)
SELECT txid, raw_bytes FROM block_tx WHERE block_hash = ? ORDER BY tx_index;
```

### Transaction by TXID
```sql
SELECT * FROM tx WHERE txid = ?;
```

### Address Summary
```sql
SELECT received, sent, tx_count FROM addr_stats WHERE scripthash = ?;
```

### Address UTXOs (with pagination)
```sql
SELECT txid, vout, value, height, spk_hex 
FROM addr_utxo 
WHERE scripthash = ? AND is_spent = 0 AND height >= ?
ORDER BY height, tx_index 
LIMIT ?;
```

### Address History (with pagination)
```sql
SELECT txid, height, tx_index, delta
FROM addr_hist 
WHERE scripthash = ? AND (height < ? OR (height = ? AND tx_index < ?))
ORDER BY height DESC, tx_index DESC 
LIMIT ?;
```

### Mempool Summary
```sql
SELECT COUNT(*) as tx_count, SUM(size) as total_bytes, MIN(fee_rate) as min_fee_rate
FROM mempool;

-- Fee rate histogram
SELECT 
    CASE 
        WHEN fee_rate < 1 THEN '0-1'
        WHEN fee_rate < 5 THEN '1-5'
        WHEN fee_rate < 10 THEN '5-10'
        ELSE '10+'
    END as bucket,
    COUNT(*) as count
FROM mempool 
GROUP BY bucket;
```

## Performance Considerations

1. **Use Prepared Statements** - All queries should use prepared statements for performance
2. **Batch Inserts** - Process blocks in transactions for atomicity
3. **Index Maintenance** - The indices above are critical for sub-second response times
4. **WAL Mode** - Use `PRAGMA journal_mode=WAL` for better concurrency
5. **Cache Size** - Set `PRAGMA cache_size=10000` or higher for better performance

## Cursor-Based Pagination

Encode `(height, tx_index)` as base64 for stateless pagination:

```cpp
struct Cursor {
    uint32_t height;
    uint32_t tx_index;
    
    std::string to_base64() const {
        std::string data(8, 0);
        std::memcpy(data.data(), &height, 4);
        std::memcpy(data.data() + 4, &tx_index, 4);
        return base64_encode(data);
    }
    
    static Cursor from_base64(const std::string& b64) {
        std::string decoded = base64_decode(b64);
        Cursor c;
        std::memcpy(&c.height, decoded.data(), 4);
        std::memcpy(&c.tx_index, decoded.data() + 4, 4);
        return c;
    }
};
```

## Database Size Estimates

For a blockchain with 1M transactions:
- `blocks`: ~50MB (assuming 1000 blocks)
- `tx`: ~200MB 
- `addr_utxo`: ~300MB (assuming 2M UTXOs)
- `addr_hist`: ~400MB (assuming 5M address interactions)
- `addr_stats`: ~10MB (assuming 100K unique addresses)

Total: ~1GB for 1M transactions. Plan accordingly for your expected blockchain size.

## Reorg Handling

When a reorg occurs:
1. Delete all data for blocks >= reorg_height
2. Re-process the new chain from reorg_height
3. Update address statistics by recalculating from scratch or maintaining deltas

This index structure provides the foundation for sub-second API responses even with millions of transactions.
