# 🔐 Individual Address Keys - When Generated & How Saved

## THE KEY INSIGHT: Private Keys Are NOT Saved! 🚨

**Private keys are derived on-demand from the seed, NOT stored in the database!**

---

## Timeline: When Are Keys Generated?

### 1️⃣ **Creating New Address** (e.g., "Generate Address" button)

```
USER: Click "Generate Address"
    ↓
DAEMON: deriveaddress RPC called
    ↓
HDWallet::DeriveNextAddress()
    ├─ index_ = 0 (first address)
    ├─ Call DeriveAddressAt(0)
    │   ├─ Read seed from memory (decrypted)
    │   ├─ Derive path: m/84'/1'/0'/0/0
    │   ├─ Compute private key in memory
    │   ├─ Compute public key from private key
    │   ├─ Compute address from public key
    │   └─ ⚠️ WIPE private key from memory!
    ├─ index_++ (now 1)
    └─ Save() → Store index to wallet.enc

DATABASE: Insert into wallet.db:
    ├─ addresses table: (address, script_pubkey, path)
    ├─ NO PRIVATE KEY STORED! ❌
    └─ Only public data (address, script, path)
```

**Code from `hd_wallet.cpp:243-248`:**
```cpp
std::string HDWallet::DeriveNextAddress() {
  std::string addr = DeriveAddressAt(index_);  // ← Private key generated here
  index_++;                                     // ← Increment counter
  Save();                                       // ← Save counter (NOT private key!)
  return addr;                                  // ← Return address
}
```

**What's Stored:**
```sql
-- wallet.db (SQLite)
INSERT INTO addresses (address, script_pubkey, type, created_at)
VALUES (
    'din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f',  -- ✅ Address (public)
    '001477729f724a09636d9021c8868984914907533a35',   -- ✅ Script (public)
    'p2wpkh',                                         -- ✅ Type (public)
    1728177600                                        -- ✅ Timestamp (public)
);
-- ❌ NO PRIVATE KEY COLUMN!
```

**What's NOT Stored:**
- ❌ Private key
- ❌ Public key (can be derived from address)
- ❌ Any secret data

---

### 2️⃣ **Signing Transaction** (e.g., "Send 10 DIN" button)

```
USER: Click "Send 10 DIN"
    ↓
DAEMON: walletcreatefundedpsbt RPC called
    ↓
HDWallet::CreateTransaction()
    ├─ Select UTXOs from wallet.db
    ├─ Example: UTXO from address index 0 and 3
    └─ Need private keys to sign!

HDWallet::SignTransaction()
    ├─ For each input:
    │   ├─ Get address index (0, 3)
    │   ├─ Call GetPrivateKeyAt(0)  ← GENERATE KEY NOW!
    │   │   ├─ Read seed from memory
    │   │   ├─ Derive path: m/84'/1'/0'/0/0
    │   │   ├─ Compute private key
    │   │   └─ Return private key (in memory)
    │   ├─ Call GetPrivateKeyAt(3)  ← GENERATE KEY NOW!
    │   │   ├─ Read seed from memory
    │   │   ├─ Derive path: m/84'/1'/0'/0/3
    │   │   ├─ Compute private key
    │   │   └─ Return private key (in memory)
    │   └─ Sign input with private key
    ├─ ⚠️ WIPE ALL private keys from memory!
    └─ Broadcast signed transaction
```

**Code from `hd_wallet.cpp:611-655`:**
```cpp
std::vector<uint8_t> HDWallet::GetPrivateKeyAt(uint32_t index) const {
  // Derive private key using same BIP32 path as address
  uint8_t I[64];
  const uint8_t* seed = seed_.data();  // ← Read seed from memory
  HMAC512((const uint8_t*)"Bitcoin seed", 12, seed, 64, I);
  uint8_t k[32]; memcpy(k, I, 32);
  uint8_t c[32]; memcpy(c, I+32, 32);

  // ... BIP32 derivation (hardened: 84', 1', 0') ...
  // ... BIP32 derivation (normal: 0, index) ...
  
  // Return private key
  std::vector<uint8_t> privkey(k, k + 32);
  return privkey;  // ← Fresh private key, derived from seed!
}
```

**What Happens in Memory:**
```
Time: 0ms    → Seed in memory (encrypted)
Time: 1ms    → Decrypt seed with password
Time: 2ms    → Derive private key #0
Time: 3ms    → Sign input #0
Time: 4ms    → Wipe private key #0
Time: 5ms    → Derive private key #3
Time: 6ms    → Sign input #3
Time: 7ms    → Wipe private key #3
Time: 8ms    → Broadcast transaction
Time: 9ms    → All private keys GONE from memory
```

**Nothing is stored! Private keys exist for ~1ms per signature!**

---

## How Are Keys "Saved"? 🤔

**Short answer: They're NOT saved!**

### What IS Saved:

#### 1. **Encrypted Seed (wallet.enc)**
```
File: ./data/wallet.enc
Content: [AES-256-GCM encrypted blob]
Contains:
  ├─ 12-word mnemonic (encrypted)
  ├─ 512-bit seed (encrypted)
  ├─ Address counter (encrypted)
  └─ Salt + nonce (not encrypted)

Encryption: Password → Argon2id → AES-256-GCM
```

#### 2. **Address Index (wallet.enc)**
```
index_ = 5  ← "I've generated 5 addresses"
```
This tells the wallet: "Next time, derive index 5"

#### 3. **Public Address Data (wallet.db)**
```sql
-- SQLite table: addresses
CREATE TABLE addresses (
    address TEXT,           -- din1q... (public)
    script_pubkey TEXT,     -- 0014... (public)
    type TEXT,              -- p2wpkh (public)
    path TEXT,              -- m/84'/1'/0'/0/0 (public)
    created_at INTEGER      -- timestamp (public)
);

-- Example row:
| din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f | 001477729f... | p2wpkh | m/84'/1'/0'/0/0 | 1728177600 |
```

### What is NOT Saved:

```
❌ Private keys (derived on-demand from seed)
❌ Public keys (derived from private keys)
❌ Master key (derived from seed)
❌ Account keys (derived from master key)
```

---

## The Derivation Algorithm (BIP32)

### Same Index → Same Key (Always!)

```
Input:  Seed + Index
Output: Private Key

Example:
  Seed: 0x8f4a2b7c3e1d9a5c4f6e8b2a7d3c1e9f... (from 12 words)
  Index: 0

  Derivation:
    m/84'/1'/0'/0/0
    ├─ m                  ← Seed
    ├─ m/84'              ← Hardened (BIP84 SegWit)
    ├─ m/84'/1'           ← Hardened (Testnet)
    ├─ m/84'/1'/0'        ← Hardened (Account 0)
    ├─ m/84'/1'/0'/0      ← Normal (Receiving)
    └─ m/84'/1'/0'/0/0    ← Normal (Index 0)
         ↓
    Private Key: 0x3a8f9c2e1b4d7a6c... (32 bytes)
```

**Key Property: Deterministic!**
```
Same seed + Same index = Same private key (every time)
```

This is why we don't need to store private keys!

---

## Security Model 🔒

### Threat: Someone Steals `wallet.db`

**What they get:**
```
✅ Your addresses (already public on blockchain)
✅ Your transaction history (already public on blockchain)
✅ Your balance (already public on blockchain)
✅ Derivation paths (public, just numbers)
```

**What they DON'T get:**
```
❌ Your seed (not in wallet.db)
❌ Your private keys (not in wallet.db)
❌ Ability to spend your coins
```

**Result: 🟢 Safe! They can watch but not spend.**

---

### Threat: Someone Steals `wallet.enc`

**What they get:**
```
⚠️  Encrypted seed (useless without password)
⚠️  Address counter (useless without password)
⚠️  Salt and nonce (public, not secret)
```

**What they DON'T get (unless they crack password):**
```
❌ Decrypted seed
❌ Ability to derive private keys
❌ Ability to spend coins
```

**Result: 🟡 Moderately Safe (depends on password strength)**
- Strong password (20+ chars) → Very hard to crack
- Weak password (8 chars) → Can be cracked

---

### Threat: Someone Steals `wallet.enc` + Password

**What they get:**
```
🚨 Decrypted seed
🚨 Ability to derive ALL private keys
🚨 Ability to spend ALL coins
🚨 Ability to generate future addresses
```

**Result: 🔴 GAME OVER! Total compromise.**

---

### Threat: Someone Gets 12 Words

**What they get:**
```
🚨 Seed (can regenerate from words)
🚨 ALL private keys (can derive)
🚨 ALL future addresses (can derive)
🚨 Total control of wallet
```

**Result: 🔴 GAME OVER! Worse than stealing wallet.enc!**

**Why worse?** They don't need the password - they have the raw seed!

---

## Memory Lifecycle of Private Keys ⏱️

```
┌─────────────────────────────────────────────────────┐
│  PRIVATE KEY LIFECYCLE                              │
├─────────────────────────────────────────────────────┤
│                                                     │
│  Birth:  Generated when signing transaction        │
│          (derived from seed in ~1ms)                │
│                                                     │
│  Life:   Exists in RAM for 1-10ms                  │
│          (only while signing)                       │
│                                                     │
│  Death:  Wiped from memory after use               │
│          (OPENSSL_cleanse or memset_s)              │
│                                                     │
│  ⚠️  NEVER written to disk                          │
│  ⚠️  NEVER in logs                                  │
│  ⚠️  NEVER cached                                   │
│  ⚠️  NEVER in swap (mlock if supported)            │
│                                                     │
└─────────────────────────────────────────────────────┘
```

**Code Pattern:**
```cpp
// Generate private key
std::vector<uint8_t> privkey = GetPrivateKeyAt(index);

// Use it
bool signed = SignTransaction(tx, privkey);

// WIPE IT IMMEDIATELY
OPENSSL_cleanse(privkey.data(), privkey.size());
privkey.clear();

// ✅ Private key is now GONE from memory
```

---

## Comparison: HD Wallet vs Traditional Wallet

### Traditional Wallet (Bitcoin Core old style):

```
Private Keys:
  ├─ Key #1: [stored in wallet.dat]
  ├─ Key #2: [stored in wallet.dat]
  ├─ Key #3: [stored in wallet.dat]
  └─ Key #N: [stored in wallet.dat]

Backup: Must backup wallet.dat after generating new address!
```

**Problem:** If you generate 10 new addresses, your old backup is useless!

### HD Wallet (DineroCoin):

```
Seed: [stored encrypted in wallet.enc]
    ↓ derive
Private Keys:
  ├─ Key #0: [derived on-demand, NOT stored]
  ├─ Key #1: [derived on-demand, NOT stored]
  ├─ Key #2: [derived on-demand, NOT stored]
  └─ Key #N: [derived on-demand, NOT stored]

Backup: Backup 12 words ONCE, restore INFINITE addresses!
```

**Advantage:** One backup works forever (even for future addresses!)

---

## Real-World Analogy 🏠

**Traditional Wallet:**
```
You have a keychain with 100 physical keys.
├─ Need to carry all 100 keys
├─ Lose keychain = lose access to all locks
└─ Add new lock = add new key (must backup again!)
```

**HD Wallet:**
```
You have a MASTER KEY that can generate any key.
├─ Only carry master key
├─ Generate specific key when needed (then throw it away!)
├─ Add new lock = derive new key from master
└─ Backup master once = can generate all keys forever!
```

---

## Summary: Key Generation & Storage 🎯

### When Generated:

1. **Address Creation:**
   - Private key generated temporarily
   - Public key derived
   - Address created
   - **Private key WIPED** (not saved!)
   - Only address/script saved to database

2. **Transaction Signing:**
   - Private key generated on-demand
   - Transaction signed
   - **Private key WIPED** immediately
   - Nothing saved

### How Saved:

```
What's Saved:
✅ Seed (encrypted in wallet.enc)
✅ Address counter (in wallet.enc)
✅ Addresses (in wallet.db)
✅ Scripts (in wallet.db)
✅ Derivation paths (in wallet.db)

What's NOT Saved:
❌ Private keys (derived on-demand)
❌ Public keys (derived from private keys)
❌ Master keys (derived from seed)
```

### The Golden Rule:

```
🔑 ONE seed → INFINITE keys
📝 ONE backup (12 words) → FOREVER secure
🔐 Private keys: Born → Used → Wiped (in milliseconds)
```

**Bottom Line:**
- Private keys are **ephemeral** (temporary)
- Only the seed is **persistent** (permanent)
- This is MORE secure than storing keys!

---

## Why This Design? 🤔

### Advantages:

1. **Security:**
   - Smaller attack surface (only seed needs protection)
   - Private keys barely exist (hard to steal what doesn't exist!)
   - Memory wiping prevents leaks

2. **Backup:**
   - One-time backup (12 words)
   - Works forever (even for future addresses)
   - Easy to write down

3. **Privacy:**
   - Can't correlate addresses (without seed)
   - Watch-only wallets possible (xpub)

4. **Portability:**
   - Restore wallet on any device
   - Just need 12 words
   - No need to transfer wallet files

### Disadvantages:

1. **Performance:**
   - Slight overhead (derive key each time)
   - ~1ms per key (negligible for most use cases)

2. **Single Point of Failure:**
   - Lose 12 words = lose everything
   - No recovery mechanism
   - Must protect seed carefully

**Trade-off: Security > Performance** ✅

---

## Final Warning ⚠️

```
12 Words = Your Entire Wallet
    ↓
Lose them = Lose all coins (forever)
Share them = Lose all coins (stolen)
Forget password = Lose all coins (locked)
```

**Never:**
- ❌ Store 12 words digitally
- ❌ Take photos of 12 words
- ❌ Email 12 words
- ❌ Store in cloud
- ❌ Store in password manager (encrypted seed is enough)

**Always:**
- ✅ Write on paper (2+ copies)
- ✅ Store in safe places (fireproof safe, bank vault)
- ✅ Test recovery before depositing large amounts
- ✅ Consider metal backup (fireproof/waterproof)

---

**Remember: Private keys are temporary. Only the seed is forever.** 🔑

