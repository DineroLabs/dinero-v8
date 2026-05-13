# Phase 2 Design Specification
# Active Descriptor Wallets (Signing, Policy Inheritance, PSBT Integration)

**Project:** DineroCoin
**Phase:** 2 – Active Descriptors
**Status:** Design (No Code Yet)
**Prerequisite:** v3.5.0-descriptor-import (Phase 1.5)
**Audience:** Core developers, auditors, hardware wallet integrators

---

## 1. Scope & Goals

Phase 2 extends the descriptor wallet system to support **active descriptors** capable of signing transactions, while strictly preserving wallet policy and PSBT guardrails.

### Primary Goal
Enable descriptor-based signing without weakening security, privacy, or policy guarantees.

### Explicit Non-Goals
Phase 2 does NOT implement:
- ❌ Tapscript / script-path spending
- ❌ Miniscript / wsh() descriptors
- ❌ Multisig descriptors
- ❌ Descriptor mutation or upgrades
- ❌ Policy combinators
- ❌ Covenant enforcement

Those are Phase 3+ features.

---

## 2. Core Security Principle (Non-Negotiable)

> **Descriptors do not define policy.**
> **Descriptors inherit wallet policy.**

This invariant must hold at all times, regardless of RPC input, PSBT structure, or external signer behavior.

### Consequences
- A BIP86 wallet cannot activate wpkh() descriptors
- A BIP84 wallet cannot activate tr() descriptors
- Descriptors never bypass PSBT guardrails
- Signing capability is granted only after strict validation

---

## 3. Descriptor Classes

Phase 2 introduces three descriptor classes, distinguished by **signing capability**, not descriptor syntax.

### 3.1 Watch-Only (Already Implemented – Phase 1.5)

```json
{
  "desc": "tr([fp/86h/1447h/0h]xpub/0/*)#checksum",
  "active": false
}
```

**Properties:**
- No private keys
- No signing
- UTXO tracking only
- Address derivation deferred (currently)

✅ **Unchanged in Phase 2**

### 3.2 Active Internal Descriptors (Hot Wallet)

```json
{
  "desc": "tr([fp/86h/1447h/0h]xprv/0/*)#checksum",
  "active": true
}
```

**Properties:**
- Signing keys are inside Dinero
- Descriptor fingerprint must match wallet seed
- Derivation path must match wallet policy
- Used for normal wallet operation

### 3.3 Active External Descriptors (Hardware Wallet)

```json
{
  "desc": "tr([fp/86h/1447h/0h]xpub/0/*)#checksum",
  "active": true,
  "signing_capability": "external"
}
```

**Properties:**
- No private keys in Dinero
- Dinero coordinates PSBT creation
- External signer produces signatures
- Dinero verifies policy compliance before finalization

---

## 4. Wallet Policy Enforcement Matrix

| Wallet Policy | Allowed Active Descriptor | Rejected |
|---------------|---------------------------|----------|
| **BIP86** | `tr()` only | `wpkh()`, script-path |
| **BIP84** | `wpkh()` only | `tr()` |
| **Watch-only** | `active=false` only | any `active` |

This is enforced at **descriptor activation time**.

---

## 5. Database Schema Extensions (Phase 2)

Extends Phase 1.5 schema.

```sql
ALTER TABLE imported_descriptors
ADD COLUMN signing_capability TEXT NOT NULL DEFAULT 'none'
  CHECK(signing_capability IN ('none', 'internal', 'external'));

ALTER TABLE imported_descriptors
ADD COLUMN key_origin_fingerprint TEXT;

ALTER TABLE imported_descriptors
ADD COLUMN derivation_path_prefix TEXT;

ALTER TABLE imported_descriptors
ADD COLUMN activation_timestamp INTEGER;

ALTER TABLE imported_descriptors
ADD COLUMN activated_by TEXT;
```

### Invariants
- `signing_capability != 'none'` requires `active = true`
- `signing_capability = 'internal'` requires fingerprint match
- `signing_capability = 'external'` forbids xprv material

---

## 6. Descriptor Activation Gate (Critical)

All `active=true` descriptors pass through:

```cpp
validateDescriptorActivation()
```

### Validation Steps (Mandatory)

1. **Descriptor Type Check**
   - `tr()` ↔ BIP86
   - `wpkh()` ↔ BIP84

2. **Derivation Path Check**
   - BIP86 → `m/86h/...`
   - BIP84 → `m/84h/...`

3. **Fingerprint Validation**
   - `internal`: MUST match wallet seed fingerprint
   - `external`: MUST be present, but not matched

4. **Key Material Check**
   - `internal`: xprv required
   - `external`: xpub only
   - `watch-only`: xpub only, `active=false`

❌ **Failure at any step → hard rejection**

### Error Messages

```
"Cannot activate tr() descriptor in BIP84 wallet (policy mismatch)"
"Fingerprint mismatch: descriptor [abcd1234] != wallet [5678efgh]"
"Derivation path must use m/86h/... for BIP86 wallets"
"External descriptors cannot contain private key material (xprv)"
```

---

## 7. Xpub Deserialization & BIP32 Public Derivation

Phase 2 introduces a new crypto primitive.

### Required Capability

```cpp
class ExtendedPubKey {
    static ExtendedPubKey FromString(const std::string& xpub_string);
    ExtendedPubKey Derive(uint32_t index) const;
    std::vector<uint8_t> GetPublicKey() const;
};
```

### Implementation Requirements

- **Base58Check decoding** with version validation
- **Network version enforcement:**
  - Mainnet xpub: `0x0488B21E`
  - Mainnet zpub (BIP84): `0x04B24746`
- **Chain code extraction** (32 bytes)
- **Public key validation** via secp256k1
- **Non-hardened derivation only** (index < 0x80000000)

### Rules
- Non-hardened derivation only
- Strict base58check validation
- Network version enforcement
- Chain code integrity verified

⚠️ **No address derivation occurs until this is correct.**

---

## 8. Address Derivation (After Xpub Support)

Once BIP32 public derivation exists:

### BIP84
1. `HASH160(pubkey)`
2. `OP_0 <20-byte hash>`
3. Bech32 encoding

### BIP86
1. X-only pubkey extraction
2. `OP_1 <32-byte key>`
3. Bech32m encoding

Derived addresses populate:
```
imported_descriptor_addresses
```

---

## 9. PSBT Integration (No Guardrail Changes)

### Critical Rule
**PSBT guardrails are unchanged.**

Phase 2 only determines **who is allowed to sign**, not **what may be signed**.

### Signing Flow

```
UTXO →
scriptPubKey →
descriptor lookup →
wallet policy check →
PSBT validation →
sign (internal or external)
```

### Enforcement Points
1. Descriptor activation gate
2. PSBT Taproot validator (already implemented)
3. Final PSBT sanity checks before broadcast

---

## 10. External Signing Protocol (Phase 2.2)

For `signing_capability = 'external'`:

1. **Dinero creates unsigned PSBT**
2. **PSBT exported** to external signer (USB, QR code, file)
3. **External signer validates** policy match
4. **External signer produces** signatures
5. **Dinero imports** signed PSBT
6. **Dinero validates:**
   - Signatures match expected pubkeys
   - Script-path fields absent (BIP86)
   - Policy compliance maintained
7. **Finalize and broadcast**

---

## 11. Required Test Coverage (Phase 2)

### Negative Tests (Must Fail)
- ❌ `tr()` active descriptor in BIP84 wallet
- ❌ `wpkh()` active descriptor in BIP86 wallet
- ❌ Fingerprint mismatch (internal)
- ❌ Wrong derivation path (84h vs 86h)
- ❌ External descriptor with xprv
- ❌ BIP86 script-path PSBT

### Positive Tests
- ✅ Valid internal active descriptor
- ✅ Valid external active descriptor
- ✅ Watch-only remains unchanged
- ✅ PSBT guardrails still enforced
- ✅ Address derivation matches HD wallet
- ✅ Migration from v17 to v18 succeeds

---

## 12. Security Guarantees Preserved

Phase 2 does not weaken:
- ✅ BIP86 key-path-only enforcement
- ✅ PSBT script-path rejection
- ✅ Hardware wallet compatibility
- ✅ Wallet policy integrity
- ✅ Descriptor auditability

---

## 13. Implementation Order (Strict)

1. **Xpub deserialization + BIP32 public derivation**
2. **Descriptor activation gate**
3. **Schema migration (v17 → v18)**
4. **Internal signing support**
5. **External signing coordination**
6. **Address derivation**
7. **Tests**
8. **Documentation update**

**Any other order will compromise security or create technical debt.**

---

## 14. Phase Boundary Summary

| Phase | Capability |
|-------|------------|
| **1.5** | Descriptor import (watch-only) |
| **2** | Active descriptors + signing |
| **3+** | Miniscript, multisig, Tapscript |

---

## 15. Audit Safety Check

```sql
-- Migration v18 integrity check
-- Should always return 0 (enforced by activation gate)
SELECT COUNT(*) FROM imported_descriptors
WHERE active = 1 AND signing_capability = 'none';
```

---

## 16. Final Statement

Phase 2 completes the descriptor wallet foundation without compromising Dinero's security model.

This design:
- ✅ Is Bitcoin Core compatible
- ✅ Is auditor-friendly
- ✅ Preserves BIP86 guarantees
- ✅ Avoids premature complexity

**No code should be written until this document is accepted.**

---

**Status:** APPROVED ✅
**Date:** 2026-01-18
**Next Action:** Implement Step 1 (Xpub Deserialization)
