# DineroCoin Descriptor Wallet Implementation Plan
## Bitcoin-Grade Architecture (Option 1)

**Critical Rule**: DescriptorWallet is the **LAST** thing to implement, not the first.

---

## Architecture Layers (Build in Order)

```
Layer 5: DescriptorWallet (glue layer)          ← WEEK 3+
         ↓
Layer 4: Wallet Persistence (state)             ← WEEK 2-3
         ↓
Layer 3: Descriptor Engine (determinism)        ← WEEK 2
         ↓
Layer 2: Script Ownership (IsMine)              ← WEEK 1
         ↓
Layer 1: Key Identity (FOUNDATION)              ← WEEK 1 (START HERE)
```

---

## LAYER 1: Key Identity & Origin (WEEK 1)

### 1.1 KeyID Structure

```cpp
// include/wallet/key_identity.h
namespace dinero {
namespace wallet {

// Primary wallet identity - hash of internal public key
// Bitcoin uses uint160 (RIPEMD160(SHA256(pubkey)))
// For Taproot: HASH160(x-only-internal-pubkey)
using KeyID = std::array<uint8_t, 20>;  // 20 bytes (160 bits)

// Compute KeyID from public key
KeyID ComputeKeyID(const std::vector<uint8_t>& pubkey);

// Compute KeyID from x-only pubkey (Taproot internal key)
KeyID ComputeKeyIDFromXOnly(const std::array<uint8_t, 32>& xonly_pubkey);

} // namespace wallet
} // namespace dinero
```

### 1.2 KeyOriginInfo Structure

```cpp
// include/wallet/key_origin.h
namespace dinero {
namespace wallet {

/**
 * Key origin metadata for BIP32 hierarchical deterministic keys.
 * This is what makes deterministic re-derivation possible.
 *
 * Example: [f23a9c12/86'/1447'/0'/0/12]
 *   - fingerprint: f23a9c12 (first 4 bytes of master pubkey hash)
 *   - path: [86' + HARDENED, 1447' + HARDENED, 0' + HARDENED, 0, 12]
 */
struct KeyOriginInfo {
    uint32_t fingerprint;           // Master key fingerprint (first 4 bytes of HASH160)
    std::vector<uint32_t> path;     // Full derivation path from master

    // Serialization
    std::string toString() const;
    static std::optional<KeyOriginInfo> parse(const std::string& str);

    // Path helpers
    std::string getPathString() const;  // "m/86'/1447'/0'/0/12"
    uint32_t getPurpose() const;        // 84 or 86
    bool isBIP84() const { return getPurpose() == 84; }
    bool isBIP86() const { return getPurpose() == 86; }
};

/**
 * Complete key metadata stored in wallet DB
 */
struct WalletKey {
    KeyID id;                       // Primary identifier
    KeyOriginInfo origin;           // Where this key came from
    bool spendable;                 // true = have privkey, false = watch-only
    std::optional<std::string> label;
    uint64_t created_at;            // Unix timestamp

    // For descriptor wallets: which descriptor produced this key
    std::optional<uint32_t> descriptor_id;
    std::optional<uint32_t> descriptor_index;
};

} // namespace wallet
} // namespace dinero
```

### 1.3 Implementation Tasks (Week 1, Day 1-2)

1. Create `src/wallet/key_identity.cpp`:
   ```cpp
   KeyID ComputeKeyID(const std::vector<uint8_t>& pubkey) {
       // SHA256(pubkey)
       uint8_t sha256_hash[32];
       SHA256(pubkey.data(), pubkey.size(), sha256_hash);

       // RIPEMD160(sha256_hash)
       uint8_t hash160[20];
       RIPEMD160(sha256_hash, 32, hash160);

       KeyID result;
       std::copy(hash160, hash160 + 20, result.begin());
       return result;
   }

   KeyID ComputeKeyIDFromXOnly(const std::array<uint8_t, 32>& xonly_pubkey) {
       // For Taproot: hash the x-only internal pubkey
       uint8_t sha256_hash[32];
       SHA256(xonly_pubkey.data(), 32, sha256_hash);

       uint8_t hash160[20];
       RIPEMD160(sha256_hash, 32, hash160);

       KeyID result;
       std::copy(hash160, hash160 + 20, result.begin());
       return result;
   }
   ```

2. Create `src/wallet/key_origin.cpp`:
   ```cpp
   std::string KeyOriginInfo::toString() const {
       std::ostringstream oss;
       oss << std::hex << std::setfill('0') << std::setw(8) << fingerprint;
       for (uint32_t component : path) {
           oss << "/" << (component & ~0x80000000);
           if (component & 0x80000000) {
               oss << "'";
           }
       }
       return oss.str();
   }

   std::string KeyOriginInfo::getPathString() const {
       std::ostringstream oss;
       oss << "m";
       for (uint32_t component : path) {
           oss << "/" << (component & ~0x80000000);
           if (component & 0x80000000) {
               oss << "'";
           }
       }
       return oss.str();
   }

   uint32_t KeyOriginInfo::getPurpose() const {
       if (path.empty()) return 0;
       return path[0] & ~0x80000000;  // Strip hardened bit
   }
   ```

3. Integrate with existing `WalletManager::getNewAddress()`:
   - Compute KeyID after deriving pubkey
   - Create KeyOriginInfo from derivation path
   - Store in new `wallet_keys` table

---

## LAYER 2: Script Ownership Model (IsMine) (WEEK 1)

### 2.1 Ownership Classification

```cpp
// include/wallet/script_ownership.h
namespace dinero {
namespace wallet {

enum class ScriptOwnership {
    NO = 0,          // Not ours
    WATCH_ONLY = 1,  // Ours to watch, but can't spend (no privkey)
    SPENDABLE = 2,   // Ours and spendable (have privkey)
};

/**
 * Bitcoin's IsMine logic - determines wallet's relationship to a script.
 * This is the KEY to solving the Taproot spending bug.
 */
class ScriptOwnershipResolver {
public:
    explicit ScriptOwnershipResolver(class WalletKeyStore* keystore);

    // Primary interface - replaces address-based ownership checks
    ScriptOwnership IsMine(const std::vector<uint8_t>& scriptPubKey) const;

    // Extract key IDs from script (may be multiple for multisig)
    std::vector<KeyID> ExtractKeyIDs(const std::vector<uint8_t>& scriptPubKey) const;

    // Check if we have a specific key
    bool HaveKey(const KeyID& key_id) const;

    // Get key origin info (for deterministic re-derivation)
    std::optional<KeyOriginInfo> GetKeyOrigin(const KeyID& key_id) const;

    // Derive private key on-demand (never stored)
    std::optional<std::vector<uint8_t>> GetPrivateKey(const KeyID& key_id) const;

private:
    WalletKeyStore* keystore_;

    // Script type detection
    bool IsP2WPKH(const std::vector<uint8_t>& script) const;
    bool IsP2TR(const std::vector<uint8_t>& script) const;

    // Key extraction
    std::optional<KeyID> ExtractP2WPKHKeyID(const std::vector<uint8_t>& script) const;
    std::optional<KeyID> ExtractP2TRKeyID(const std::vector<uint8_t>& script) const;
};

} // namespace wallet
} // namespace dinero
```

### 2.2 WalletKeyStore Interface

```cpp
// include/wallet/keystore.h
namespace dinero {
namespace wallet {

/**
 * Wallet key storage - knows which keys we have and where they came from.
 * Does NOT store private keys - only metadata for deterministic derivation.
 */
class WalletKeyStore {
public:
    // Key queries
    virtual bool HaveKey(const KeyID& key_id) const = 0;
    virtual std::optional<WalletKey> GetKey(const KeyID& key_id) const = 0;
    virtual std::vector<WalletKey> GetAllKeys() const = 0;

    // Key addition
    virtual bool AddKey(const WalletKey& key) = 0;

    // Master seed access (for deterministic derivation)
    virtual bool HaveMasterSeed() const = 0;
    virtual std::optional<std::vector<uint8_t>> GetMasterSeed() const = 0;

    // Key derivation
    virtual std::optional<std::vector<uint8_t>> DerivePrivateKey(
        const KeyOriginInfo& origin) const = 0;
};

} // namespace wallet
} // namespace dinero
```

### 2.3 Implementation Tasks (Week 1, Day 3-5)

1. Implement `ScriptOwnershipResolver::IsMine()`:
   ```cpp
   ScriptOwnership ScriptOwnershipResolver::IsMine(
       const std::vector<uint8_t>& scriptPubKey) const {

       // Extract all key IDs referenced by this script
       auto key_ids = ExtractKeyIDs(scriptPubKey);
       if (key_ids.empty()) {
           return ScriptOwnership::NO;
       }

       // Check if we have all required keys
       bool have_all = true;
       bool any_spendable = false;

       for (const auto& key_id : key_ids) {
           if (!HaveKey(key_id)) {
               have_all = false;
               break;
           }

           auto key = keystore_->GetKey(key_id);
           if (key && key->spendable) {
               any_spendable = true;
           }
       }

       if (!have_all) {
           return ScriptOwnership::NO;
       }

       return any_spendable ? ScriptOwnership::SPENDABLE
                            : ScriptOwnership::WATCH_ONLY;
   }
   ```

2. Implement key extraction for Taproot:
   ```cpp
   std::optional<KeyID> ScriptOwnershipResolver::ExtractP2TRKeyID(
       const std::vector<uint8_t>& script) const {

       // P2TR: OP_1 (0x51) PUSH_32 <32-byte-xonly-pubkey>
       if (script.size() != 34) return std::nullopt;
       if (script[0] != 0x51) return std::nullopt;  // Not OP_1
       if (script[1] != 0x20) return std::nullopt;  // Not 32 bytes

       // Extract x-only pubkey (this is the TWEAKED output key)
       std::array<uint8_t, 32> output_key;
       std::copy(script.begin() + 2, script.end(), output_key.begin());

       // CRITICAL: To get KeyID, we need the INTERNAL key, not output key
       // This requires untweaking: internal_key = output_key - HASH(output_key)
       // For now, we'll use output_key directly and fix this in descriptor layer

       return ComputeKeyIDFromXOnly(output_key);
   }
   ```

3. **Fix the Taproot keyID problem**: The script contains the **tweaked** output key, but we need to match it to the **internal** key we stored. This requires:
   ```cpp
   // Store BOTH internal KeyID and output KeyID in wallet_keys table
   // - internal_key_id: for key origin lookup
   // - output_key_id: for script matching (what's in scriptPubKey)
   ```

---

## LAYER 3: Descriptor Engine (WEEK 2)

### 3.1 Descriptor Grammar (DineroCoin Subset)

Support these descriptors only (Phase 1):

```
wpkh(KEY)           # BIP84 P2WPKH
tr(KEY)             # BIP86 P2TR (key-path only, no script tree)
combo(KEY)          # Optional: generates multiple address types
```

Where `KEY` is:
```
[ORIGIN]xpub/DERIVATION_PATH
```

Examples:
```
wpkh([f23a9c12/84'/1447'/0']xpub.../0/*)      # BIP84 receive addresses
tr([f23a9c12/86'/1447'/0']xpub.../0/*)        # BIP86 Taproot addresses
```

### 3.2 Descriptor Structure

```cpp
// include/wallet/descriptor.h
namespace dinero {
namespace wallet {

/**
 * Descriptor types (Phase 1 - minimal set)
 */
enum class DescriptorType {
    WPKH,   // wpkh(KEY) - BIP84
    TR,     // tr(KEY) - BIP86 keypath-only
    COMBO,  // combo(KEY) - multiple types
};

/**
 * Parsed descriptor with derivation capability.
 * This is a PURE FUNCTION - no state, no side effects.
 */
class Descriptor {
public:
    // Parse descriptor string
    static std::optional<Descriptor> Parse(const std::string& desc_str);

    // Descriptor properties
    DescriptorType getType() const { return type_; }
    std::string toString() const { return descriptor_string_; }
    bool isRange() const { return is_range_; }  // Has wildcard '*'

    // Deterministic derivation (CORE FUNCTIONALITY)
    struct DerivedScript {
        std::vector<uint8_t> scriptPubKey;
        KeyOriginInfo key_origin;
        KeyID internal_key_id;      // For wallet key lookup
        std::optional<KeyID> output_key_id;  // For Taproot tweaked key
    };

    std::optional<DerivedScript> DeriveScript(uint32_t index) const;

    // Address generation (uses DeriveScript internally)
    std::optional<std::string> DeriveAddress(uint32_t index,
                                              const std::string& hrp) const;

    // Checksum
    std::string computeChecksum() const;
    std::string withChecksum() const;
    static bool verifyChecksum(const std::string& desc_with_checksum);

private:
    DescriptorType type_;
    std::string descriptor_string_;
    bool is_range_;

    // Parsed components
    struct ParsedKey {
        std::optional<KeyOriginInfo> origin;
        std::string xpub;
        std::vector<uint32_t> derivation_path;  // Path after xpub
    };

    ParsedKey key_;

    // Private constructor (use Parse)
    Descriptor(DescriptorType type, const std::string& desc_str);

    // Parsing helpers
    static std::optional<ParsedKey> parseKey(const std::string& key_section);
    static std::optional<KeyOriginInfo> parseOrigin(const std::string& origin_str);
};

} // namespace wallet
} // namespace dinero
```

### 3.3 Implementation Tasks (Week 2, Day 1-3)

1. Implement descriptor parser:
   ```cpp
   std::optional<Descriptor> Descriptor::Parse(const std::string& desc_str) {
       // Remove checksum if present: "tr(...)#12345678" -> "tr(...)"
       std::string desc = desc_str;
       size_t hash_pos = desc.find('#');
       if (hash_pos != std::string::npos) {
           if (!verifyChecksum(desc_str)) {
               return std::nullopt;  // Invalid checksum
           }
           desc = desc.substr(0, hash_pos);
       }

       // Detect descriptor type
       DescriptorType type;
       std::string key_section;

       if (desc.starts_with("wpkh(") && desc.ends_with(")")) {
           type = DescriptorType::WPKH;
           key_section = desc.substr(5, desc.length() - 6);
       } else if (desc.starts_with("tr(") && desc.ends_with(")")) {
           type = DescriptorType::TR;
           key_section = desc.substr(3, desc.length() - 4);
       } else {
           return std::nullopt;  // Unknown descriptor type
       }

       // Parse key
       auto key = parseKey(key_section);
       if (!key) return std::nullopt;

       // Create descriptor
       Descriptor result(type, desc_str);
       result.key_ = *key;
       result.is_range_ = (key_section.find('*') != std::string::npos);

       return result;
   }
   ```

2. Implement `DeriveScript()` for Taproot:
   ```cpp
   std::optional<Descriptor::DerivedScript> Descriptor::DeriveScript(
       uint32_t index) const {

       if (type_ != DescriptorType::TR) {
           // Implement WPKH separately
           return std::nullopt;
       }

       // Derive child key from xpub at specified index
       // (Use existing HDKeychain::derive logic)
       auto child_key = deriveChildKey(key_.xpub, index);
       if (!child_key) return std::nullopt;

       // Get 33-byte compressed pubkey
       auto pubkey_33 = child_key->getPublicKey();

       // Extract x-only internal key (drop 0x02/0x03 prefix)
       std::array<uint8_t, 32> internal_xonly;
       std::copy(pubkey_33.begin() + 1, pubkey_33.end(), internal_xonly.begin());

       // Compute internal KeyID
       KeyID internal_key_id = ComputeKeyIDFromXOnly(internal_xonly);

       // Apply BIP341 TapTweak
       auto output_key = ComputeTaprootOutputKey(internal_xonly);
       if (!output_key) return std::nullopt;

       // Compute output KeyID (for scriptPubKey matching)
       KeyID output_key_id = ComputeKeyIDFromXOnly(*output_key);

       // Build scriptPubKey: OP_1 PUSH_32 <output_key>
       std::vector<uint8_t> scriptPubKey;
       scriptPubKey.push_back(0x51);  // OP_1
       scriptPubKey.push_back(0x20);  // 32 bytes
       scriptPubKey.insert(scriptPubKey.end(), output_key->begin(), output_key->end());

       // Build KeyOriginInfo
       KeyOriginInfo origin = key_.origin.value_or(KeyOriginInfo{});
       origin.path.insert(origin.path.end(),
                         key_.derivation_path.begin(),
                         key_.derivation_path.end());
       origin.path.push_back(index);  // Add final index

       DerivedScript result;
       result.scriptPubKey = scriptPubKey;
       result.key_origin = origin;
       result.internal_key_id = internal_key_id;
       result.output_key_id = output_key_id;

       return result;
   }
   ```

3. Implement checksum (Bitcoin descriptor checksum algorithm):
   - Use PolyMod checksum from Bitcoin Core
   - See: https://github.com/bitcoin/bitcoin/blob/master/src/script/descriptor.cpp

---

## LAYER 4: Wallet Persistence (WEEK 2-3)

### 4.1 Database Schema

```sql
-- Descriptor definitions
CREATE TABLE descriptors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    descriptor_string TEXT NOT NULL UNIQUE,     -- "wpkh([...]xpub.../0/*)"
    descriptor_type TEXT NOT NULL,              -- "wpkh", "tr", "combo"
    checksum TEXT NOT NULL,                     -- "abcd1234"
    range_start INTEGER NOT NULL DEFAULT 0,     -- First index to derive
    range_end INTEGER NOT NULL DEFAULT 1000,    -- Last index to derive
    active INTEGER NOT NULL DEFAULT 1,          -- 0 = inactive, 1 = active
    is_internal INTEGER NOT NULL DEFAULT 0,     -- 0 = receive, 1 = change
    created_at INTEGER NOT NULL
);

-- Wallet keys (derived from descriptors)
CREATE TABLE wallet_keys (
    id BLOB PRIMARY KEY,                        -- KeyID (20 bytes)
    fingerprint INTEGER NOT NULL,               -- Master key fingerprint
    derivation_path TEXT NOT NULL,              -- "m/86'/1447'/0'/0/12"
    spendable INTEGER NOT NULL DEFAULT 1,       -- 0 = watch-only, 1 = spendable
    descriptor_id INTEGER,                      -- Which descriptor produced this
    descriptor_index INTEGER,                   -- Index within descriptor range
    label TEXT,
    created_at INTEGER NOT NULL,

    -- Taproot-specific: track both internal and output key IDs
    output_key_id BLOB,                         -- Tweaked key (for script matching)
    internal_key_id BLOB,                       -- Internal key (for derivation)

    FOREIGN KEY (descriptor_id) REFERENCES descriptors(id) ON DELETE SET NULL
);

CREATE INDEX idx_wallet_keys_fingerprint ON wallet_keys(fingerprint);
CREATE INDEX idx_wallet_keys_path ON wallet_keys(derivation_path);
CREATE INDEX idx_wallet_keys_descriptor ON wallet_keys(descriptor_id, descriptor_index);
CREATE INDEX idx_wallet_keys_output ON wallet_keys(output_key_id);

-- Script → Key mapping (for IsMine queries)
CREATE TABLE watch_scripts (
    scriptPubKey BLOB PRIMARY KEY,              -- The actual script bytes
    key_id BLOB NOT NULL,                       -- Which key owns this script
    descriptor_id INTEGER,                      -- Which descriptor produced it
    descriptor_index INTEGER,                   -- Index within descriptor
    created_at INTEGER NOT NULL,

    FOREIGN KEY (key_id) REFERENCES wallet_keys(id) ON DELETE CASCADE,
    FOREIGN KEY (descriptor_id) REFERENCES descriptors(id) ON DELETE CASCADE
);

CREATE INDEX idx_watch_scripts_key ON watch_scripts(key_id);
CREATE INDEX idx_watch_scripts_descriptor ON watch_scripts(descriptor_id, descriptor_index);

-- Master seed (encrypted, one per wallet)
CREATE TABLE hd_seeds (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    encrypted_seed BLOB NOT NULL,
    salt BLOB NOT NULL,
    fingerprint INTEGER NOT NULL,               -- For key origin tracking
    encryption_version INTEGER NOT NULL,
    created_at INTEGER NOT NULL
);

-- Migration tracking
CREATE TABLE wallet_version (
    version INTEGER PRIMARY KEY CHECK (version >= 5),
    migrated_at INTEGER NOT NULL
);
INSERT INTO wallet_version (version, migrated_at) VALUES (5, strftime('%s', 'now'));
```

### 4.2 Migration from Old Schema

```sql
-- Phase 1: Add new columns to existing tables (backward compatible)
ALTER TABLE addresses ADD COLUMN key_id BLOB;
ALTER TABLE addresses ADD COLUMN output_key_id BLOB;

-- Phase 2: Populate key_id for existing addresses
-- (This requires re-deriving keys from master seed)
UPDATE addresses
SET key_id = compute_key_id_from_path(derivation_path)
WHERE key_id IS NULL;

-- Phase 3: Create new descriptor-based tables alongside old ones
-- (Old tables remain readable for legacy wallets)
```

---

## LAYER 5: DescriptorWallet Implementation (WEEK 3+)

### 5.1 Minimal DescriptorWallet (Glue Layer)

```cpp
// include/wallet/descriptor_wallet_v2.h
namespace dinero {
namespace wallet {

/**
 * Bitcoin Core-style descriptor wallet.
 * This is THIN GLUE - all heavy lifting done by lower layers.
 */
class DescriptorWalletV2 {
public:
    explicit DescriptorWalletV2(const std::string& wallet_path);
    ~DescriptorWalletV2();

    // Initialization
    bool Initialize(const std::vector<std::string>& descriptors);
    bool IsInitialized() const;

    // Address generation (delegates to descriptor engine)
    std::string GetNewAddress(const std::string& label = "");
    std::string GetNewChangeAddress();

    // Balance (delegates to keystore + blockchain)
    uint64_t GetBalance() const;
    std::vector<UTXO> GetSpendableUTXOs() const;

    // Ownership (delegates to IsMine)
    bool IsOwned(const std::vector<uint8_t>& scriptPubKey) const;
    bool CanSpend(const std::vector<uint8_t>& scriptPubKey) const;

    // Signing (delegates to keystore for key derivation)
    std::optional<std::vector<uint8_t>> SignInput(
        const std::vector<uint8_t>& scriptPubKey,
        const std::vector<uint8_t>& tx_hash,
        uint32_t input_index) const;

private:
    std::unique_ptr<WalletKeyStore> keystore_;
    std::unique_ptr<ScriptOwnershipResolver> ownership_;
    std::vector<Descriptor> descriptors_;

    // Range management
    struct DescriptorRange {
        uint32_t next_receive_index;
        uint32_t next_change_index;
        uint32_t gap_limit;
    };

    std::unordered_map<uint32_t, DescriptorRange> ranges_;

    // Helpers
    void TopUpKeys();  // Ensure gap_limit unused addresses exist
    bool SaveToDatabase();
    bool LoadFromDatabase();
};

} // namespace wallet
} // namespace dinero
```

### 5.2 Key Methods

```cpp
std::string DescriptorWalletV2::GetNewAddress(const std::string& label) {
    // Find active receive descriptor
    auto receive_desc = std::find_if(descriptors_.begin(), descriptors_.end(),
        [](const Descriptor& d) { return d.isRange() && !d.isInternal(); });

    if (receive_desc == descriptors_.end()) {
        throw std::runtime_error("No receive descriptor configured");
    }

    // Get next index
    uint32_t index = ranges_[receive_desc->id()].next_receive_index++;

    // Derive script and key
    auto derived = receive_desc->DeriveScript(index);
    if (!derived) {
        throw std::runtime_error("Failed to derive script at index " + std::to_string(index));
    }

    // Store key in wallet_keys table
    WalletKey key;
    key.id = derived->internal_key_id;
    key.fingerprint = /* get from seed */;
    key.derivation_path = derived->key_origin.getPathString();
    key.spendable = true;
    key.descriptor_id = receive_desc->id();
    key.descriptor_index = index;
    key.output_key_id = derived->output_key_id;
    key.label = label;

    keystore_->AddKey(key);

    // Register script for IsMine queries
    RegisterScript(derived->scriptPubKey, derived->internal_key_id);

    // Derive address from scriptPubKey
    return receive_desc->DeriveAddress(index, GetNetworkHRP());
}

bool DescriptorWalletV2::CanSpend(const std::vector<uint8_t>& scriptPubKey) const {
    return ownership_->IsMine(scriptPubKey) == ScriptOwnership::SPENDABLE;
}
```

---

## Week-by-Week Implementation Plan

### WEEK 1: Foundation (Layers 1 & 2)

**Day 1-2: Key Identity**
- [ ] Create `key_identity.h/cpp`
- [ ] Implement `ComputeKeyID()`
- [ ] Implement `ComputeKeyIDFromXOnly()`
- [ ] Create `key_origin.h/cpp`
- [ ] Implement `KeyOriginInfo` serialization
- [ ] Add unit tests

**Day 3-4: Script Ownership**
- [ ] Create `script_ownership.h/cpp`
- [ ] Implement `ScriptOwnershipResolver`
- [ ] Implement `IsMine()` for P2WPKH
- [ ] Implement `IsMine()` for P2TR
- [ ] Implement `ExtractKeyIDs()`

**Day 5: KeyStore Interface**
- [ ] Create `keystore.h/cpp`
- [ ] Implement `WalletKeyStore` with SQLite backend
- [ ] Add `wallet_keys` table to schema
- [ ] Implement `HaveKey()`, `GetKey()`, `AddKey()`

**MILESTONE**: Can compute KeyIDs, determine script ownership, store key metadata

### WEEK 2: Descriptor Engine (Layer 3)

**Day 1-2: Descriptor Parsing**
- [ ] Create `descriptor.h/cpp`
- [ ] Implement descriptor parser for `wpkh()` and `tr()`
- [ ] Implement origin parsing `[fingerprint/path]`
- [ ] Implement checksum algorithm
- [ ] Add parser unit tests

**Day 3-4: Script Derivation**
- [ ] Implement `DeriveScript()` for WPKH
- [ ] Implement `DeriveScript()` for Taproot
- [ ] Implement TapTweak properly
- [ ] Store both internal and output KeyIDs
- [ ] Add derivation unit tests

**Day 5: Integration**
- [ ] Integrate descriptors with `WalletKeyStore`
- [ ] Implement descriptor range tracking
- [ ] Test full derivation pipeline

**MILESTONE**: Can parse descriptors and derive scripts deterministically

### WEEK 3: Persistence & DescriptorWallet (Layers 4 & 5)

**Day 1-2: Database Schema**
- [ ] Add `descriptors` table
- [ ] Update `wallet_keys` table schema
- [ ] Update `watch_scripts` table
- [ ] Implement migration from old schema
- [ ] Test migration on existing wallets

**Day 3-4: DescriptorWallet**
- [ ] Implement `DescriptorWalletV2` core
- [ ] Implement `GetNewAddress()`
- [ ] Implement `GetNewChangeAddress()`
- [ ] Implement signing via KeyStore
- [ ] Add integration tests

**Day 5: RPC Integration**
- [ ] Wire `DescriptorWalletV2` into RPC handlers
- [ ] Update `wallet.getnewaddress` to use descriptors
- [ ] Update `wallet.sendtoaddress` to use `IsMine()`
- [ ] Run Phase 4C-lite test suite

**MILESTONE**: Descriptor wallet working end-to-end, Taproot spending works

### WEEK 4+: Cleanup & Testing

- [ ] Delete skeleton `DescriptorWallet` (the dangerous one)
- [ ] Add comprehensive test coverage
- [ ] Performance optimization
- [ ] Documentation
- [ ] Migrate remaining RPC methods
- [ ] Legacy wallet deprecation plan

---

## Success Criteria

### Week 1 Complete
```bash
# Can compute KeyIDs
$ ./tests/unit/test_key_identity
✅ ComputeKeyID matches Bitcoin Core
✅ KeyOriginInfo serialization works
✅ IsMine detects Taproot scripts
```

### Week 2 Complete
```bash
# Can parse and derive from descriptors
$ ./tests/unit/test_descriptors
✅ Parse tr([...]/0/*) descriptor
✅ Derive script at index 0,1,2,...
✅ Checksum validation works
✅ TapTweak matches BIP341 test vectors
```

### Week 3 Complete
```bash
# Phase 4C-lite passes
$ ./tests/wallet_tests/test_rpc_spending_integration.sh
✅ 13/13 tests passing
✅ Taproot address generation works
✅ Taproot UTXO recognition works
✅ Taproot spending works
✅ No "Could not retrieve private keys" errors
```

---

## Critical Warnings

### ❌ DO NOT:
1. Try to "finish" the existing `DescriptorWallet` skeleton
2. Store private keys in database
3. Skip layers (must do 1→2→3→4→5 in order)
4. Implement miniscript or policy trees yet
5. Migrate existing wallets before testing works

### ✅ DO:
1. Build KeyID/KeyOrigin first
2. Test each layer independently
3. Keep legacy WalletManager working during migration
4. Use Bitcoin Core test vectors for validation
5. Ask for clarification if uncertain

---

## References

- Bitcoin Core descriptor implementation: https://github.com/bitcoin/bitcoin/blob/master/src/script/descriptor.cpp
- BIP 380-386: Output Script Descriptors
- BIP 32: Hierarchical Deterministic Wallets
- BIP 341: Taproot
- Bitcoin Core IsMine logic: https://github.com/bitcoin/bitcoin/blob/master/src/wallet/ismine.cpp

---

## Next Steps

**Immediate**: Start Week 1, Day 1
1. Create `include/wallet/key_identity.h`
2. Create `src/wallet/key_identity.cpp`
3. Implement `ComputeKeyID()`
4. Write unit tests

**Question for user**: Should we quarantine the skeleton `DescriptorWallet` now, or delete it completely?
