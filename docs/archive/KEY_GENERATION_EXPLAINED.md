# 🔑 Key Generation in DineroCoin - Complete Guide

## The Key Hierarchy (BIP39 → BIP32 → BIP84)

```
12 Words → Seed → Master Key → Account Keys → Address Keys
 (Entropy)  (512b)   (Extended)   (Hardened)    (Public/Private)
```

Let's break this down step by step...

---

## Step 1: The 12 Words (BIP39 Mnemonic)

### What They Are:
```
Example: iron expect scout august display north season extra dad material track payment

• 12 random words from a 2048-word dictionary
• Each word represents 11 bits of entropy
• 12 words = 132 bits of entropy (128 bits + 4-bit checksum)
• These words are YOUR MASTER SECRET
```

### When Generated:
```
User clicks "Create HD Wallet" in GUI
    ↓
Daemon generates 128 bits of random entropy
    ↓
Adds 4-bit checksum
    ↓
Converts 132 bits to 12 words using BIP39 wordlist
    ↓
Shows words to user: "iron expect scout august..."
```

### The 12 Words ARE NOT Keys (Yet!)

The 12 words are like a **password** that generates keys. They are:
- ✅ Easy to write down (just words)
- ✅ Easy to backup (paper, metal plate)
- ✅ Easy to type (when restoring)
- ❌ Not directly used for signing transactions

Think of them as: **Master Password → Master Key**

---

## Step 2: Seed Generation (BIP39)

### The Conversion:
```
12 Words: "iron expect scout august display north season extra dad material track payment"
    ↓
PBKDF2-HMAC-SHA512 (2048 rounds)
    ↓
512-bit Seed: 0x8f4a2b7c... (64 bytes)
```

**This is the ACTUAL master secret used for key derivation!**

### The Process:
```python
# Pseudocode
mnemonic = "iron expect scout august display north season extra dad material track payment"
password = ""  # Optional BIP39 passphrase (usually empty)

seed = PBKDF2(
    password=mnemonic.encode('utf-8'),
    salt=("mnemonic" + password).encode('utf-8'),
    iterations=2048,
    hash_function=HMAC-SHA512
)

# Result: 512-bit (64-byte) seed
# This seed is THE SOURCE of all your keys!
```

**Important:** 
- Same 12 words → Same seed (every time)
- Seed is NEVER shown to user (handled internally)
- Seed is what gets encrypted in `wallet.enc`

---

## Step 3: Master Key (BIP32 Extended Key)

### From Seed to Master Key:
```
512-bit Seed
    ↓
HMAC-SHA512("Bitcoin seed", seed)
    ↓
Split result:
  ├─ First 256 bits  → Master Private Key
  └─ Last 256 bits   → Master Chain Code
```

### Master Extended Key:
```
Master Key = {
    private_key: 256-bit number (can sign transactions)
    chain_code:  256-bit number (for deriving child keys)
    depth:       0 (root level)
    fingerprint: hash160(public_key)[0:4]
    child_index: 0
}
```

This is an **Extended Key** because it can derive infinite child keys!

---

## Step 4: Derivation Path (BIP84 for SegWit)

### The Path Structure:
```
m / purpose' / coin_type' / account' / change / address_index

m/84'/1'/0'/0/0 → First receiving address
m/84'/1'/0'/0/1 → Second receiving address
m/84'/1'/0'/0/2 → Third receiving address
m/84'/1'/0'/1/0 → First change address
```

**Breakdown:**
- `m` = Master key
- `84'` = BIP84 (Native SegWit) [Hardened]
- `1'` = Testnet coin type [Hardened]
  - (0' = Bitcoin mainnet, 1' = Testnet/Altcoins)
- `0'` = Account 0 [Hardened]
- `0` = External chain (receiving addresses) [Normal]
- `0,1,2...` = Address index [Normal]

**Hardened (') vs Normal:**
- Hardened: Uses private key (more secure, can't derive from public)
- Normal: Uses public key (less secure, but useful for watch-only)

---

## Step 5: Address Key Generation

### For Each Address:

```
Master Key (m)
    ↓ derive(84')
Purpose Key (m/84')
    ↓ derive(1')
Coin Key (m/84'/1')
    ↓ derive(0')
Account Key (m/84'/1'/0')
    ↓ derive(0)
External Key (m/84'/1'/0'/0)
    ↓ derive(0)
Address Key #0 (m/84'/1'/0'/0/0)
    ├─ Private Key: Used for signing
    └─ Public Key:  Used for address generation
```

### The Actual Keys:

**Private Key (32 bytes):**
```
Example: 0x3a8f9c2e1b4d7a6c...
• 256-bit random number
• Used to SIGN transactions
• NEVER shared or shown
• Derived from seed via path
```

**Public Key (33 bytes compressed):**
```
Example: 0x02f5a8c3b1e9d7a4...
• Derived from private key using secp256k1
• Used to CREATE addresses
• Safe to share (can't reverse to private key)
```

**Address (bech32):**
```
pubkey_hash = HASH160(public_key)
address = bech32_encode("din", 0, pubkey_hash)
Result: din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f
```

---

## Complete Flow Example

### User Creates Wallet:

```
1. USER ACTION: Click "Create HD Wallet"
   ↓
2. GENERATE ENTROPY:
   Random bytes: 0x8f4a2b7c3e1d9a5c4f6e8b2a7d3c1e9f
   ↓
3. CONVERT TO WORDS (BIP39):
   12 words: "iron expect scout august display north season extra dad material track payment"
   ↓ [User writes these down on paper!]
   ↓
4. SHOW TO USER:
   "These 12 words are your backup. Save them!"
   ↓ [User confirms they saved them]
   ↓
5. DERIVE 512-BIT SEED:
   PBKDF2(words) → 512-bit seed
   ↓
6. ENCRYPT SEED (AES-256-GCM + Argon2id):
   User enters password: "MyStrongPass123"
   Seed encrypted and saved to: wallet.enc
   ↓
7. DERIVE MASTER KEY:
   HMAC-SHA512("Bitcoin seed", seed)
   ↓
8. DERIVE FIRST ADDRESS:
   Path: m/84'/1'/0'/0/0
   ├─ Private key: 0x3a8f9c2e1b4d7a6c...
   ├─ Public key: 0x02f5a8c3b1e9d7a4...
   └─ Address: din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f
   ↓
9. SAVE TO DATABASE (wallet.db):
   INSERT INTO addresses (address, script_pubkey, ...)
   ↓
10. SHOW TO USER:
    "Your first address: din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f"
```

### User Generates More Addresses:

```
1. USER: Click "Derive New Address"
   ↓
2. WALLET: Read next index from database (e.g., 1)
   ↓
3. WALLET: Unlock encrypted seed (requires password)
   ↓
4. WALLET: Derive path m/84'/1'/0'/0/1
   ├─ Different private key
   ├─ Different public key
   └─ Different address
   ↓
5. SAVE: Store in database
   ↓
6. SHOW: New address to user
```

### User Signs Transaction:

```
1. USER: Send 10 DIN to din1qrecipient...
   ↓
2. WALLET: Select UTXOs from database
   ↓
3. WALLET: Find which addresses own these UTXOs
   ↓
4. WALLET: Unlock encrypted seed (requires password!)
   ↓
5. WALLET: Derive private keys for those addresses
   Example: m/84'/1'/0'/0/0 → private_key_0
           m/84'/1'/0'/0/3 → private_key_3
   ↓
6. WALLET: Sign transaction with private keys
   signature = ECDSA_sign(transaction_hash, private_key)
   ↓
7. WALLET: Broadcast signed transaction to network
   ↓
8. CLEAR: Wipe private keys from memory (security!)
```

---

## The Key Types - Summary

### 1. **Mnemonic (12 Words)**
```
Type:     Human-readable backup
Purpose:  Easy to write/remember/restore
Location: Written on paper by user
Security: THE MASTER SECRET - never share!
Example:  "iron expect scout august display north..."
```

### 2. **Seed (512 bits)**
```
Type:     Binary data
Purpose:  Source of all key derivation
Location: Encrypted in wallet.enc
Security: Never shown to user, highly protected
Example:  0x8f4a2b7c3e1d9a5c4f6e8b2a7d3c1e9f...
```

### 3. **Master Key (256 bits + chain code)**
```
Type:     Extended key (xprv)
Purpose:  Root of derivation tree
Location: Derived on-the-fly from seed
Security: Never stored, derived when needed
Example:  xprv9s21ZrQH143K... (base58 encoded)
```

### 4. **Account/Address Keys (256 bits each)**
```
Type:     Private keys for specific addresses
Purpose:  Sign transactions
Location: Derived on-the-fly when signing
Security: Never stored unencrypted, wiped after use
Example:  0x3a8f9c2e1b4d7a6c... (32 bytes)
```

### 5. **Public Keys (33 bytes compressed)**
```
Type:     Public keys
Purpose:  Generate addresses, verify signatures
Location: Can be stored in wallet.db (safe)
Security: Public - safe to share
Example:  0x02f5a8c3b1e9d7a4... (33 bytes)
```

### 6. **Addresses (bech32 string)**
```
Type:     Bech32-encoded hash of public key
Purpose:  Receive payments
Location: Stored in wallet.db, shown to users
Security: Public - meant to be shared!
Example:  din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f
```

---

## Security Hierarchy

```
Most Secret    → 12 Words (mnemonic)
    ↓           → Seed (512 bits)
    ↓           → Master Private Key
    ↓           → Account Private Keys
    ↓           → Address Private Keys
    ↓           → Public Keys
Least Secret   → Addresses (share freely!)
```

**Rule:** 
- Never share anything above "Public Keys"
- Only share addresses (or public keys if needed)
- 12 words = Ultimate secret (compromise = total loss)

---

## Common Questions

### Q: "Are the 12 words the key?"

**A:** Not exactly! They are the **SOURCE** of all keys.

```
12 Words → Seed → Master Key → Infinite Keys

Think of it like:
12 Words = Master Password
Seed     = Master Key
Keys     = Individual Keys for each lock
```

### Q: "When are keys generated?"

**A:** Keys are generated **on-demand**:

1. **Mnemonic/Seed**: Generated once when creating wallet
2. **Master Key**: Derived when wallet starts
3. **Address Keys**: Derived when:
   - Generating new address
   - Signing transaction
   - Exporting key

Keys are NOT pre-generated and stored. They're derived from the seed each time!

### Q: "Where are the private keys stored?"

**A:** They're NOT stored (mostly)!

```
Stored:
✅ Mnemonic → encrypted in wallet.enc
✅ Seed → encrypted in wallet.enc

NOT Stored (derived on-the-fly):
❌ Master key
❌ Account keys
❌ Address private keys

Only stored temporarily:
⚠️  Private keys in memory while signing (then wiped)
```

### Q: "What if someone gets my wallet.db?"

**A:** They can see:
- ✅ Your addresses (public anyway)
- ✅ Your transaction history (public on blockchain)
- ✅ Your balance (public on blockchain)
- ❌ **NOT** your 12 words (encrypted in wallet.enc)
- ❌ **NOT** your private keys (not stored)

Result: They can watch your wallet but **cannot spend your coins!**

### Q: "What if someone gets my 12 words?"

**A:** **GAME OVER!** They can:
- ✅ Regenerate your seed
- ✅ Derive all your private keys
- ✅ Sign transactions
- ✅ Steal all your coins
- ✅ Generate all future addresses

Result: **Total compromise. Move funds immediately!**

---

## Backup Recommendations

### Level 1 - Critical (MUST HAVE):
```
✅ Write 12 words on paper (2+ copies)
✅ Store in safe locations (fireproof safe, bank vault)
✅ NEVER digital (no photos, no files, no cloud!)
✅ Remember password (or store separately from words)
```

### Level 2 - Advanced (Optional):
```
✅ Metal plate backup (fireproof, waterproof)
✅ Split words across locations (6 words each place)
✅ Use BIP39 passphrase (25th word) for extra security
✅ Test restore procedure before depositing large amounts
```

### Level 3 - Paranoid (Maximum Security):
```
✅ Multiple copies in different geographic locations
✅ Encrypt the 12 words themselves (extra layer)
✅ Use multisig (requires multiple keys to spend)
✅ Dead man's switch (access instructions for heirs)
```

---

## Key Takeaways 🎯

1. **12 Words** = Master secret that generates ALL keys
2. **Seed** = 512-bit number derived from 12 words
3. **Private Keys** = Specific keys for each address (derived from seed)
4. **Public Keys** = Derived from private keys (safe to share)
5. **Addresses** = Derived from public keys (meant to be public!)

**The Chain:**
```
12 Words → Seed → Master Key → Address Keys → Sign Transactions
```

**Never lose the 12 words!** They're the ONLY way to recover your wallet! 🚨

---

**Everything else can be regenerated from those 12 words.**
**Lose the words = Lose the coins. Forever.** 💸

