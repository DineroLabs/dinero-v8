# 🏗️ DineroCoin Wallet Architecture

## Storage Layers Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    USER'S WALLET                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. HD Wallet File (wallet.enc)                            │
│     ├─ 🔐 ENCRYPTED with AES-256-GCM + Argon2id          │
│     ├─ Contains: 12-word seed (BIP39 mnemonic)            │
│     ├─ Master key for ALL addresses                        │
│     └─ Password required to decrypt                        │
│                                                             │
│  2. SQLite Database (wallet.db)                            │
│     ├─ 📝 NOT ENCRYPTED (addresses are public)            │
│     ├─ Contains: addresses, transactions, UTXOs, metadata │
│     ├─ Fast queries for balance/history                    │
│     └─ Can be regenerated from seed                        │
│                                                             │
│  3. UTXO Index (utxo.db)                                   │
│     ├─ 🔍 Global blockchain UTXO set                      │
│     ├─ Not wallet-specific                                 │
│     └─ Shared by all wallets                               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 1. HD Wallet File (wallet.enc) - THE KEYS 🔑

**Location:** `./data/wallet.enc` (encrypted file)

**Contains:**
- 🔐 **Encrypted 12-word seed phrase** (512-bit entropy)
- 🔐 **Master private key** derived from seed
- 🔐 **Derivation path info** (BIP84: m/84'/1'/0')

**Encryption:**
```
Password → Argon2id → 256-bit key → AES-256-GCM → Encrypted seed
         (slow hash)  (strong key)   (authenticated)
```

**Security:**
- ✅ Password required to decrypt
- ✅ Wrong password = authentication failure
- ✅ Brute-force resistant (Argon2id is memory-hard)
- ✅ Never stored in plaintext (even in memory briefly)

**Purpose:**
This is your **MASTER SECRET**. Everything else can be regenerated from this!

---

## 2. SQLite Database (wallet.db) - THE DATA 📊

**Location:** `./data/wallet.db` (unencrypted SQLite file)

### Tables:

#### `wallet_meta` - Wallet Metadata
```sql
CREATE TABLE wallet_meta (
    id INTEGER PRIMARY KEY CHECK (id=1),  -- Only one row
    schema_version INTEGER,                -- DB version
    last_applied_height INTEGER,           -- Sync progress
    birth_height INTEGER,                  -- When wallet created
    created_at INTEGER                     -- Timestamp
);
```

#### `keys` - Public Keys (NOT Private Keys!)
```sql
CREATE TABLE keys (
    id INTEGER PRIMARY KEY,
    pubkey TEXT UNIQUE NOT NULL,           -- Public key (safe to store)
    privkey TEXT,                          -- ENCRYPTED private key
    enc_salt TEXT,                         -- Encryption salt
    enc_nonce TEXT,                        -- Encryption nonce
    is_encrypted INTEGER,                  -- 0 or 1
    created_at INTEGER
);
```
**Note:** Private keys are NEVER stored in plaintext!

#### `addresses` - Your Addresses
```sql
CREATE TABLE addresses (
    id INTEGER PRIMARY KEY,
    address TEXT UNIQUE NOT NULL,          -- din1q...
    script_pubkey TEXT NOT NULL,           -- Witness script
    type TEXT NOT NULL,                    -- 'p2wpkh' or 'p2wsh'
    key_id INTEGER,                        -- Links to keys table
    watch_only INTEGER,                    -- 0 or 1
    created_at INTEGER,
    FOREIGN KEY(key_id) REFERENCES keys(id)
);
```

#### `tx` - Transaction History
```sql
CREATE TABLE tx (
    id INTEGER PRIMARY KEY,
    txid TEXT UNIQUE NOT NULL,             -- Transaction ID
    blockhash TEXT,                        -- Which block
    height INTEGER,                        -- Block height
    time INTEGER,                          -- Timestamp
    raw TEXT,                              -- Full tx hex
    direction TEXT,                        -- 'recv', 'send', 'self'
    amount INTEGER,                        -- In una (sats)
    fee INTEGER                            -- Transaction fee
);
```

#### `utxos` - Unspent Outputs (Your Coins!)
```sql
CREATE TABLE utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,                 -- Output index
    address_id INTEGER NOT NULL,           -- Your address
    value INTEGER NOT NULL,                -- Amount in una
    script_pubkey TEXT NOT NULL,
    height INTEGER NOT NULL,               -- When received
    spend_txid TEXT,                       -- NULL if unspent
    spend_height INTEGER,                  -- When spent
    PRIMARY KEY(txid, vout),
    FOREIGN KEY(address_id) REFERENCES addresses(id)
);
```

**Query Example:**
```sql
-- Get wallet balance
SELECT SUM(value) as balance 
FROM utxos 
WHERE spend_txid IS NULL;  -- Only unspent coins

-- Get transaction history
SELECT txid, direction, amount, height, time
FROM tx
ORDER BY height DESC
LIMIT 10;

-- Get all receiving addresses
SELECT address, created_at
FROM addresses
WHERE type = 'p2wpkh'
ORDER BY created_at DESC;
```

---

## 3. How They Work Together 🔄

### Creating a New Address:

```
1. User clicks "Generate Address"
   ↓
2. GUI calls RPC: deriveaddress
   ↓
3. Daemon checks: Is wallet encrypted?
   ├─ Yes → Is it unlocked?
   │   ├─ Yes → Continue
   │   └─ No → Error: "Wallet locked"
   └─ No → Continue (unsafe!)
   ↓
4. HD Wallet derives address:
   - Read encrypted seed from wallet.enc
   - Decrypt with password (if locked)
   - Derive: m/84'/1'/0'/0/next_index
   - Generate: pubkey → script → bech32
   ↓
5. Save to SQLite (wallet.db):
   - INSERT INTO addresses (address, script, type)
   - UPDATE wallet_meta SET next_index++
   ↓
6. Return address to GUI: "din1q..."
```

### Sending a Transaction:

```
1. User enters: recipient + amount
   ↓
2. GUI calls: walletcreatefundedpsbt
   ↓
3. Daemon queries SQLite:
   - SELECT * FROM utxos WHERE spend_txid IS NULL
   - Calculate total available balance
   - Select UTXOs to cover amount + fee
   ↓
4. Daemon reads HD Wallet:
   - Decrypt seed (requires password!)
   - Derive private keys for selected UTXOs
   - Sign transaction with private keys
   ↓
5. Broadcast transaction to network
   ↓
6. Update SQLite:
   - INSERT INTO tx (txid, direction='send', ...)
   - UPDATE utxos SET spend_txid=... (mark as spent)
```

### Receiving Coins:

```
1. Blockchain sync detects transaction to your address
   ↓
2. Daemon queries SQLite:
   - SELECT id FROM addresses WHERE address = 'din1q...'
   ↓
3. If found (your address):
   - INSERT INTO tx (direction='recv', ...)
   - INSERT INTO utxos (txid, vout, value, ...)
   ↓
4. GUI shows updated balance:
   - Query: SELECT SUM(value) FROM utxos WHERE spend_txid IS NULL
```

---

## Security Model 🔒

### What's Encrypted:
```
✅ HD Wallet seed (wallet.enc)
✅ Private keys in database (keys.privkey column)
✅ Requires password to access
```

### What's NOT Encrypted:
```
❌ Addresses (they're public anyway!)
❌ Transaction history (public on blockchain)
❌ UTXOs (public on blockchain)
❌ Balance (calculated from public UTXOs)
```

**Why?**
- Addresses and transactions are PUBLIC on the blockchain
- Anyone can see them via block explorer
- Encrypting them adds complexity without security benefit
- Only PRIVATE KEYS need encryption

---

## Backup Strategy 💾

### Critical (MUST BACKUP):
```
1. 12-word seed phrase (write on paper!)
   └─ Can regenerate EVERYTHING
   
2. Password (remember it!)
   └─ Needed to decrypt seed
```

### Optional (Can regenerate):
```
3. wallet.db (SQLite database)
   └─ Speeds up wallet loading
   └─ Contains transaction history
   └─ Can be regenerated by scanning blockchain
```

### Recovery Scenarios:

**Lost wallet.db but have seed:**
```
1. Import seed phrase
2. Wallet scans blockchain
3. Regenerates all addresses
4. Rebuilds wallet.db from blockchain
5. ✅ Full recovery!
```

**Lost seed but have wallet.db:**
```
1. Database has addresses
2. Database has UTXOs
3. ❌ NO PRIVATE KEYS!
4. ❌ CANNOT SPEND COINS!
5. ❌ FUNDS LOST FOREVER!
```

**Forgot password:**
```
1. If you have seed phrase:
   └─ ✅ Create new wallet, import seed
   
2. If you don't have seed:
   └─ ❌ Wallet.enc is useless
   └─ ❌ Funds lost forever
```

---

## File Locations 📁

```
./data/
├── wallet.enc              # 🔐 Encrypted HD wallet (THE KEY!)
├── wallet.db              # 📊 SQLite database (addresses, txs, UTXOs)
├── utxo.db                # 🔍 Global UTXO index (blockchain data)
├── blockchain_state.json  # 📈 Sync state
└── .cookie                # 🍪 RPC authentication
```

---

## Key Takeaways 🎯

1. **HD Wallet File** = Your master secret (encrypted with password)
2. **SQLite Database** = Your wallet's "bookkeeping" (public data)
3. **Seed phrase** = Ultimate backup (can regenerate everything)
4. **Password** = Unlocks encrypted seed (needed to spend)
5. **Losing seed** = Losing coins forever
6. **Losing wallet.db** = Inconvenient but recoverable

**Golden Rule:** 
```
🔑 Seed phrase = Your money
📊 Wallet.db = Your records
```

Never lose your seed phrase! 🚨

