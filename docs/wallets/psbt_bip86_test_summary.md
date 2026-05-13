# PSBT BIP86 Guardrails - Test Summary

## Overview
DineroCoin implements strict PSBT (Partially Signed Bitcoin Transaction) guardrails for BIP86 Taproot wallets to prevent accidental script-path spending and maintain maximum privacy and security.

## BIP86 Policy Enforcement

### What is BIP86?
BIP86 defines Taproot key-path spending using derivation path `m/86'/coin_type'/account'`. BIP86 wallets use **key-path-only** spending (no script trees) for:
- **Maximum privacy**: All outputs look identical on-chain
- **Simplicity**: Single signature spending, no complex scripts
- **Security**: Reduced attack surface

### PSBT Guardrails Implementation

**Location**: `/Users/haydarevich/Documents/DineroCoin/src/wallet/psbt_signer.cpp:155-159`

```cpp
// BIP86 TAPROOT GUARDRAIL: Validate Taproot inputs for policy compliance
auto validation = dinero::PSBTTaprootValidator::validateInput(input.kv, wallet_policy);
if (!validation.valid) {
    // Reject PSBT with script-path spending for BIP86 wallets
    throw std::runtime_error(validation.error);
}
```

**Validation Logic**: `/Users/haydarevich/Documents/DineroCoin/src/wallet/psbt_taproot_validator.cpp`

## Test Results

### ✅ Test 1: BIP86 Rejects TAP_SCRIPT_SIG
**Scenario**: PSBT contains `TAP_SCRIPT_SIG` (script-path signature)
**Expected**: Rejected
**Result**: ✅ PASS
**Error Message**:
```
BIP86 Policy Violation: Script-path spending detected.

This wallet uses BIP86 Taproot (key-path only) for maximum simplicity and privacy.
Script-path spending (complex Tapscript) is NOT allowed.

The PSBT contains one or more of:
  - TAP_SCRIPT_SIG: Script-path signature (BIP 342)
  - TAP_LEAF_SCRIPT: Tapscript leaf in merkle tree
  - TAP_MERKLE_ROOT: Non-empty merkle root (indicates script tree)

BIP86 wallets ONLY support key-path spending (simple single signature).
```

### ✅ Test 2: BIP86 Accepts TAP_KEY_SIG
**Scenario**: PSBT contains `TAP_KEY_SIG` (key-path signature)
**Expected**: Accepted
**Result**: ✅ PASS
**Reason**: Key-path spending is the standard BIP86 spending method

### ✅ Test 3: BIP86 Rejects TAP_LEAF_SCRIPT
**Scenario**: PSBT contains `TAP_LEAF_SCRIPT` (script tree leaf)
**Expected**: Rejected
**Result**: ✅ PASS
**Reason**: Script trees violate BIP86's key-path-only policy

### ✅ Test 4: BIP86 Rejects Non-Zero TAP_MERKLE_ROOT
**Scenario**: PSBT contains non-zero `TAP_MERKLE_ROOT`
**Expected**: Rejected
**Result**: ✅ PASS
**Reason**: Merkle root indicates script tree existence

### ✅ Test 5: BIP84 Has No Restrictions
**Scenario**: BIP84 wallet with all script-path fields
**Expected**: Accepted
**Result**: ✅ PASS
**Reason**: BIP84 (SegWit) wallets don't have Taproot restrictions

### ✅ Test 6: Legacy Wallets Have No Restrictions
**Scenario**: Legacy wallet (empty policy) with script-path fields
**Expected**: Accepted
**Result**: ✅ PASS
**Reason**: Legacy wallets default to unrestricted mode

## PSBT Field Reference

### Forbidden for BIP86
| Field | Type | Description | BIP86 Status |
|-------|------|-------------|--------------|
| `TAP_SCRIPT_SIG` | 0x14 | Script-path signature (BIP 342) | ❌ FORBIDDEN |
| `TAP_LEAF_SCRIPT` | 0x15 | Tapscript leaf in merkle tree | ❌ FORBIDDEN |
| `TAP_MERKLE_ROOT` | 0x18 | Non-zero merkle root | ❌ FORBIDDEN |

### Allowed for BIP86
| Field | Type | Description | BIP86 Status |
|-------|------|-------------|--------------|
| `TAP_KEY_SIG` | 0x13 | Key-path Schnorr signature | ✅ ALLOWED |
| `WITNESS_UTXO` | 0x01 | Previous output being spent | ✅ REQUIRED |
| `TAP_INTERNAL_KEY` | 0x17 | Internal key (optional) | ✅ ALLOWED |

## Security Considerations

### Why Enforce BIP86 Guardrails?

1. **Prevent User Error**: Users might accidentally sign script-path PSBTs from untrusted sources
2. **Hardware Wallet Safety**: Many hardware wallets enforce BIP86 key-path-only spending
3. **Privacy Protection**: Script-path reveals script structure on-chain, reducing privacy
4. **Compatibility**: Ensures PSBTs work with standard BIP86 hardware wallets

### Attack Scenarios Prevented

**Scenario 1: Malicious PSBT with Hidden Script**
- Attacker creates PSBT with complex Tapscript containing hidden spend conditions
- User's BIP86 wallet attempts to sign
- **Guardrail**: Wallet detects `TAP_LEAF_SCRIPT` and rejects PSBT
- **Result**: User protected from signing malicious transaction

**Scenario 2: Cross-Wallet PSBT Confusion**
- User receives PSBT intended for advanced Tapscript wallet
- Tries to sign with BIP86 wallet
- **Guardrail**: Wallet detects script-path fields and rejects
- **Result**: Clear error message prevents confusion

## Integration Points

### 1. Wallet Creation
- `wallet.createhd <name> ... bip86` sets `wallet_policy = "bip86"` in database
- Policy persisted in `wallet_meta` table

### 2. PSBT Signing
- `PSBTSigner::sign()` reads wallet policy from WalletManager
- Calls `PSBTTaprootValidator::validateInput()` for each Taproot input
- Throws exception if BIP86 violation detected

### 3. Descriptor Export
- `wallet.exportdescriptors` includes `policy` field
- External tools can verify wallet capabilities before creating PSBTs

## Test Coverage

| Component | Test Type | Status |
|-----------|-----------|--------|
| `PSBTTaprootValidator` | Unit Test | ✅ test_psbt_guardrails |
| PSBT Signing Integration | Integration Test | ✅ test_psbt_bip86_integration |
| Live Wallet RPC | Manual Test | ✅ (via descriptor RPCs) |

## Compliance

### BIP References
- **BIP86**: Deterministic Taproot key-path-only derivation
- **BIP174**: PSBT (Partially Signed Bitcoin Transaction) format
- **BIP341**: Taproot validation rules
- **BIP342**: Tapscript validation rules (explicitly avoided for BIP86)
- **BIP371**: PSBT extensions for Taproot

### Standards Compliance
✅ Bitcoin Core compatible descriptor format
✅ Hardware wallet compatible (Ledger, Trezor BIP86 policies)
✅ BIP86 reference implementation compliance
✅ PSBT v2 field support (BIP371)

## Conclusion

DineroCoin's PSBT BIP86 guardrails provide:
1. ✅ Strict enforcement of BIP86 key-path-only policy
2. ✅ Clear error messages for policy violations
3. ✅ Compatibility with hardware wallets
4. ✅ Protection against malicious PSBTs
5. ✅ No restrictions on BIP84 or legacy wallets

**All tests pass successfully** - BIP86 guardrails working as designed!
