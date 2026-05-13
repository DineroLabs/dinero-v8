# Confidential Address Format Specification

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Designed (Implementation Pending)

---

## 1. Overview

This document specifies the confidential address format for DineroCoin, which encodes both spend and view public keys to enable confidential transactions.

### 1.1 Requirements

- ✅ Encode both spend and view public keys
- ✅ Include version byte for future upgrades
- ✅ Checksum to detect typos
- ✅ Base58 encoding for human readability
- ✅ Distinguish from transparent addresses

---

## 2. Address Types

### 2.1 Transparent Address (Legacy)

```
Format: Base58(version || pubkey_hash || checksum)
Example: D7x9pQvKz3...
Length: 34 characters
Prefix: 'D' (version byte 0x1E for mainnet)
```

**Usage:** Standard Bitcoin-style addresses (not confidential)

### 2.2 Confidential Address (New)

```
Format: Base58(version || spend_pubkey || view_pubkey || checksum)
Example: dC8h3kP9mN...
Length: 95 characters
Prefix: 'dC' (version byte 0x42 for mainnet)
```

**Usage:** Confidential transactions with rewind capability

---

## 3. Confidential Address Structure

### 3.1 Binary Layout

```
Offset | Size  | Field
-------|-------|---------------------------
0      | 1     | version byte
1      | 33    | spend_pubkey (compressed secp256k1)
34     | 33    | view_pubkey (compressed secp256k1)
67     | 4     | checksum (first 4 bytes of double SHA256)
-------|-------|---------------------------
Total: 71 bytes
```

### 3.2 Version Byte

```
Mainnet Confidential:  0x42  (Base58 prefix: "dC")
Testnet Confidential:  0xA4  (Base58 prefix: "tC")
Mainnet Transparent:   0x1E  (Base58 prefix: "D")
Testnet Transparent:   0x7E  (Base58 prefix: "t")
```

**Purpose:** Distinguish address types and networks

### 3.3 Public Keys

**Spend Public Key:**
```
33 bytes compressed secp256k1 point
Format: 0x02 || X-coordinate (even Y)
     or 0x03 || X-coordinate (odd Y)
```

**View Public Key:**
```
33 bytes compressed secp256k1 point
Format: Same as spend key
```

**Encoding:** Both use secp256k1 compression (NOT Ristretto255)

### 3.4 Checksum

```
payload = version || spend_pubkey || view_pubkey  (67 bytes)
hash1 = SHA256(payload)
hash2 = SHA256(hash1)
checksum = hash2[0:4]  (first 4 bytes)
```

**Purpose:** Detect typos and transmission errors

---

## 4. Address Generation

### 4.1 From Master Seed

**Input:** 64-byte master seed (from BIP39 mnemonic)

**Derivation:**
```cpp
uint8_t master_seed[64];  // From mnemonic

// Derive spend key
uint8_t spend_privkey[32];
HMAC_SHA512("dinero_spend", master_seed, spend_privkey);

secp256k1_pubkey spend_pubkey_point;
secp256k1_ec_pubkey_create(ctx, &spend_pubkey_point, spend_privkey);

uint8_t spend_pubkey[33];
size_t len = 33;
secp256k1_ec_pubkey_serialize(ctx, spend_pubkey, &len,
                               &spend_pubkey_point,
                               SECP256K1_EC_COMPRESSED);

// Derive view key
uint8_t view_privkey[32];
HMAC_SHA512("dinero_view", master_seed, view_privkey);

secp256k1_pubkey view_pubkey_point;
secp256k1_ec_pubkey_create(ctx, &view_pubkey_point, view_privkey);

uint8_t view_pubkey[33];
secp256k1_ec_pubkey_serialize(ctx, view_pubkey, &len,
                               &view_pubkey_point,
                               SECP256K1_EC_COMPRESSED);

// Build address
uint8_t address_bytes[71];
address_bytes[0] = 0x42;  // Mainnet confidential
memcpy(&address_bytes[1], spend_pubkey, 33);
memcpy(&address_bytes[34], view_pubkey, 33);

// Compute checksum
uint8_t hash1[32], hash2[32];
SHA256(address_bytes, 67, hash1);
SHA256(hash1, 32, hash2);
memcpy(&address_bytes[67], hash2, 4);

// Encode to Base58
char address_base58[128];
base58_encode(address_bytes, 71, address_base58);

// Result: dC8h3kP9mN... (95 characters)
```

### 4.2 Example Output

```
Spend Privkey:   0x1234...abcd (32 bytes)
Spend Pubkey:    0x02abcd...ef01 (33 bytes)
View Privkey:    0x5678...1234 (32 bytes)
View Pubkey:     0x03ef01...abcd (33 bytes)
Version:         0x42
Checksum:        0x9a3b7c2d
Address (Base58): dC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h... (95 chars)
```

---

## 5. Address Parsing

### 5.1 Validation Steps

```cpp
bool ParseConfidentialAddress(const char* address, ConfidentialAddress* out) {
    // 1. Decode Base58
    uint8_t decoded[128];
    size_t decoded_len;
    if (!base58_decode(address, decoded, &decoded_len)) {
        return false;  // Invalid Base58
    }

    // 2. Check length
    if (decoded_len != 71) {
        return false;  // Wrong length
    }

    // 3. Check version
    uint8_t version = decoded[0];
    if (version != 0x42 && version != 0xA4) {
        return false;  // Not a confidential address
    }

    // 4. Verify checksum
    uint8_t hash1[32], hash2[32];
    SHA256(decoded, 67, hash1);
    SHA256(hash1, 32, hash2);

    if (memcmp(&decoded[67], hash2, 4) != 0) {
        return false;  // Checksum mismatch
    }

    // 5. Parse public keys
    secp256k1_pubkey spend_pubkey_point;
    if (secp256k1_ec_pubkey_parse(ctx, &spend_pubkey_point,
                                   &decoded[1], 33) != 1) {
        return false;  // Invalid spend pubkey
    }

    secp256k1_pubkey view_pubkey_point;
    if (secp256k1_ec_pubkey_parse(ctx, &view_pubkey_point,
                                   &decoded[34], 33) != 1) {
        return false;  // Invalid view pubkey
    }

    // 6. Extract data
    out->version = version;
    memcpy(out->spend_pubkey, &decoded[1], 33);
    memcpy(out->view_pubkey, &decoded[34], 33);

    return true;
}
```

### 5.2 Error Codes

```cpp
enum AddressValidationError {
    ADDR_VALID = 0,
    ADDR_INVALID_BASE58,
    ADDR_WRONG_LENGTH,
    ADDR_UNKNOWN_VERSION,
    ADDR_CHECKSUM_MISMATCH,
    ADDR_INVALID_SPEND_PUBKEY,
    ADDR_INVALID_VIEW_PUBKEY,
    ADDR_NETWORK_MISMATCH
};
```

---

## 6. Address Display

### 6.1 Human-Readable Format

**Full Address:**
```
dC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA
```

**Truncated Display (UI):**
```
dC8h3k...9fE3dA  (first 6 + last 6 characters)
```

**QR Code:** Full address encoded

### 6.2 URI Scheme

```
dinero:<address>?amount=<value>&label=<name>

Example:
dinero:dC8h3kP9...?amount=1.5&label=Coffee%20Shop
```

**Parameters:**
- `amount` - Value in DINERO (not una)
- `label` - UTF-8 encoded label
- `message` - Optional payment message

---

## 7. Subaddresses

### 7.1 Purpose

Allow single master key to generate multiple unlinkable addresses.

### 7.2 Derivation

```cpp
// Subaddress index i
uint32_t index = i;

// Derive subaddress spend key
uint8_t index_bytes[4];
memcpy(index_bytes, &index, 4);

uint8_t sub_spend_privkey[32];
HMAC_SHA512_Context ctx;
HMAC_SHA512_Init(&ctx, master_spend_privkey, 32);
HMAC_SHA512_Update(&ctx, "dinero_subaddr_spend", 20);
HMAC_SHA512_Update(&ctx, index_bytes, 4);
HMAC_SHA512_Final(sub_spend_privkey, &ctx);

secp256k1_pubkey sub_spend_pubkey;
secp256k1_ec_pubkey_create(secp_ctx, &sub_spend_pubkey, sub_spend_privkey);

// Derive subaddress view key (same view key as master)
uint8_t sub_view_privkey[32];
memcpy(sub_view_privkey, master_view_privkey, 32);

secp256k1_pubkey sub_view_pubkey;
secp256k1_ec_pubkey_create(secp_ctx, &sub_view_pubkey, sub_view_privkey);

// Build subaddress
ConfidentialAddress subaddr;
subaddr.version = 0x42;
serialize_pubkey(sub_spend_pubkey, subaddr.spend_pubkey);
serialize_pubkey(sub_view_pubkey, subaddr.view_pubkey);
```

**Properties:**
- Same view key (can scan with one key)
- Different spend keys (unlinkable)
- Deterministic from index

### 7.3 Use Cases

- **Merchant:** Different subaddress per customer
- **Exchange:** Unique deposit address per user
- **Privacy:** Prevent address reuse linkability

---

## 8. Integrated Addresses

### 8.1 Purpose

Embed payment ID directly in address (temporary use).

### 8.2 Format

```
Offset | Size  | Field
-------|-------|---------------------------
0      | 1     | version (0x52 for integrated)
1      | 33    | spend_pubkey
34     | 33    | view_pubkey
67     | 8     | payment_id (uint64)
75     | 4     | checksum
-------|-------|---------------------------
Total: 79 bytes → ~105 Base58 characters
```

**Prefix:** "dI" (integrated address)

### 8.3 Generation

```cpp
IntegratedAddress CreateIntegrated(const ConfidentialAddress& base_addr,
                                    uint64_t payment_id) {
    uint8_t integrated_bytes[79];
    integrated_bytes[0] = 0x52;  // Integrated version
    memcpy(&integrated_bytes[1], base_addr.spend_pubkey, 33);
    memcpy(&integrated_bytes[34], base_addr.view_pubkey, 33);
    memcpy(&integrated_bytes[67], &payment_id, 8);  // Little-endian

    // Checksum
    uint8_t hash1[32], hash2[32];
    SHA256(integrated_bytes, 75, hash1);
    SHA256(hash1, 32, hash2);
    memcpy(&integrated_bytes[75], hash2, 4);

    char address_base58[128];
    base58_encode(integrated_bytes, 79, address_base58);

    return IntegratedAddress(address_base58, payment_id);
}
```

**Use Case:** Invoice with embedded payment ID

---

## 9. Address Validation

### 9.1 Client-Side Validation

Before sending to address:

```cpp
bool ValidateAddressForSending(const char* address) {
    // 1. Parse address
    ConfidentialAddress addr;
    if (!ParseConfidentialAddress(address, &addr)) {
        return false;  // Invalid format
    }

    // 2. Check network match
    if (IS_MAINNET && addr.version != 0x42) {
        return false;  // Wrong network
    }

    // 3. Validate public keys not identity
    secp256k1_pubkey spend_pk, view_pk;
    secp256k1_ec_pubkey_parse(ctx, &spend_pk, addr.spend_pubkey, 33);
    secp256k1_ec_pubkey_parse(ctx, &view_pk, addr.view_pubkey, 33);

    // Check not point at infinity
    if (is_identity_point(spend_pk) || is_identity_point(view_pk)) {
        return false;
    }

    // 4. Check spend != view (to prevent key confusion)
    if (memcmp(addr.spend_pubkey, addr.view_pubkey, 33) == 0) {
        return false;  // Same key used for both!
    }

    return true;
}
```

### 9.2 Server-Side Validation

Same checks as client, plus:

```cpp
// Additional server checks
bool ServerValidateAddress(const ConfidentialAddress& addr) {
    // ... all client checks ...

    // Check not in blacklist (optional)
    if (IsBlacklistedAddress(addr)) {
        return false;
    }

    // Check rate limiting
    if (TooManySendsToAddress(addr)) {
        return false;
    }

    return true;
}
```

---

## 10. Security Considerations

### 10.1 Checksum Typos

**Detection Rate:**
- 1-character typo: ~100% detected (4-byte checksum)
- 2-character typo: ~100% detected
- Transposition: ~100% detected

**Undetected:** ~1 in 4 billion random corruptions

### 10.2 Phishing Protection

**Address Prefixes:**
- Confidential: Always starts with "dC"
- Transparent: Always starts with "D"
- Testnet: Always starts with "t"

**UI Recommendation:**
- Display first 8 and last 8 characters prominently
- Show full address on hover/click
- Use distinct colors for address types

### 10.3 Key Confusion

**Risk:** Using spend key as view key (or vice versa)

**Prevention:**
- Different HMAC domain separators
- Address parsing validates both keys
- Reject if spend_pubkey == view_pubkey

### 10.4 Privacy Leaks

**View Key Sharing:**
- Reveals all received amounts
- Does NOT reveal spent amounts
- Does NOT allow spending

**Use Case:** Share view key with accountant for auditing

**Warning:** Never share spend key

---

## 11. Implementation Checklist

### 11.1 Wallet Requirements

- [ ] Generate master seed from BIP39 mnemonic
- [ ] Derive spend and view keys using HMAC-SHA512
- [ ] Create confidential addresses with checksum
- [ ] Parse and validate incoming addresses
- [ ] Support subaddresses (index-based derivation)
- [ ] Display addresses in UI with truncation
- [ ] Generate QR codes for addresses
- [ ] Implement address book storage
- [ ] Validate address before sending
- [ ] Support integrated addresses with payment IDs

### 11.2 Node Requirements

- [ ] Validate address format in RPC calls
- [ ] Reject invalid addresses in transactions
- [ ] Support address-to-script conversion
- [ ] Implement network version checks
- [ ] Log address validation failures

---

## 12. Test Vectors

### 12.1 Valid Confidential Address

```json
{
  "test": "valid_conf_address",
  "spend_privkey": "0x1111111111111111111111111111111111111111111111111111111111111111",
  "spend_pubkey": "0x031b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f",
  "view_privkey": "0x2222222222222222222222222222222222222222222222222222222222222222",
  "view_pubkey": "0x02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
  "version": "0x42",
  "checksum": "0x9a3b7c2d",
  "address": "dC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA",
  "validation": "VALID"
}
```

### 12.2 Invalid Checksum

```json
{
  "test": "invalid_checksum",
  "address": "dC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dA5bC8h3kP9mNqRt2Vx7sY6gL4jK9fE3dB",
  "error": "CHECKSUM_MISMATCH",
  "validation": "INVALID"
}
```

### 12.3 Subaddress Derivation

```json
{
  "test": "subaddress_generation",
  "master_spend_privkey": "0x1111...",
  "master_view_privkey": "0x2222...",
  "subaddress_index": 5,
  "derived_spend_pubkey": "0x03...",
  "derived_view_pubkey": "0x02...",
  "subaddress": "dC9x4y...",
  "note": "Same view key, different spend key"
}
```

---

## 13. Migration from Transparent Addresses

### 13.1 Transparent Address Format

```
Old Format: D7x9pQvKz3... (34 chars)
Structure: version(1) || pubkey_hash(20) || checksum(4) = 25 bytes
```

### 13.2 Conversion (One-Way)

Cannot convert transparent → confidential (missing view key)

**User Flow:**
1. Generate new confidential address
2. Send funds from transparent to confidential
3. Deprecate old transparent address

### 13.3 Coexistence

Both address types supported:
- Transparent: "D..." addresses
- Confidential: "dC..." addresses

**Wallet Strategy:**
- Default to confidential for new addresses
- Support transparent for backward compatibility

---

## 14. Future Upgrades

### 14.1 Version 2 Addresses

**Planned Features:**
- Larger public keys (post-quantum)
- Embedded stealth address support
- Payment proof nonces

**Version Byte:** 0x62 ("dD" prefix)

### 14.2 Stealth Addresses

**Concept:** One-time addresses derived on-chain

**Benefit:** Sender can pay without knowing recipient's public key

**Status:** Future research

---

## 15. References

1. **BIP-173:** Bech32 address format (inspiration)
2. **Monero Addresses:** Similar dual-key structure
3. **BIP-39:** Mnemonic seed generation
4. **Base58Check:** Bitcoin address encoding
5. **secp256k1:** Elliptic curve for public keys

---

**End of Specification**
