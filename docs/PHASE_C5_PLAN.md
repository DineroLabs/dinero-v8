# Phase C.5: Covenant Patterns - Implementation Plan

**Date**: 2025-12-27
**Status**: Planning
**Foundation**: Built on Phase C.4 (covenant RPC endpoints)

---

## 🎯 Phase C.5 Objectives

**Goal**: Provide high-level wallet recipes using covenant primitives

**Scope**: Pattern library ONLY, no new primitives
- ✅ Vault templates (simple, recovery, hierarchical)
- ✅ Recovery flows (time-delayed, multi-path, gradual)
- ✅ Social recovery (k-of-n, timelock, hybrid)
- ✅ Multisig covenants (restricted, budget, escrow)
- ❌ NO new opcodes (use existing CTV + CSFS)
- ❌ NO consensus changes

**Key Principle**:
```
Patterns = Composition of Primitives
(Build complex behavior from simple building blocks)
```

---

## 🧱 Architectural Constraints

| Rule | Status |
|------|--------|
| Patterns use only CTV + CSFS primitives | ✅ |
| Patterns are CONSTRUCTION helpers | ✅ |
| Patterns NEVER validate covenants | ✅ |
| Patterns provide clear use-case documentation | ✅ |
| Patterns are optional (not required) | ✅ |

**Foundation Stack**:
- Phase C.1: Consensus primitives (VerifyCTV, VerifySignatureFromStack)
- Phase C.3: Construction helpers (buildCTVTemplate, signCSFSDelegation)
- Phase C.4: RPC endpoints (wallet.createctvtemplate, wallet.signcsfs)
- Phase C.5: **Patterns** (combine primitives for common use cases)

---

## 📦 Pattern Categories

### 1. Vault Patterns

**Simple Vault**:
- Lock funds with CTV to pre-committed withdrawal address
- Requires two transactions: vault → unvault → withdraw
- Provides time window to cancel malicious withdrawals

**Recovery Vault**:
- Normal path: CTV-based vault
- Recovery path: Emergency key can recover after delay
- Use case: Protect against key compromise

**Hierarchical Vault**:
- Multi-tier security (hot → warm → cold)
- Each tier has increasing delays
- Large amounts require longer waiting periods

### 2. Recovery Flows

**Time-delayed Recovery**:
- After N blocks, allow recovery key to spend
- Useful for inheritance or lost key scenarios
- Can be combined with any spending condition

**Multi-path Recovery**:
- Owner can spend immediately
- Recovery key can spend after timelock
- Provides backup without weakening security

**Gradual Recovery**:
- Small amounts (<1 BTC): immediate
- Medium amounts (1-10 BTC): 1 day delay
- Large amounts (>10 BTC): 1 week delay

### 3. Social Recovery

**K-of-N Recovery**:
- Require K signatures from N guardians
- Used for account recovery (e.g., 3-of-5 friends)
- Guardians cannot steal (only recover)

**Timelock Social Recovery**:
- Owner can spend immediately
- Guardians can recover after 6 months
- Prevents guardian theft while owner is active

**Hybrid Recovery**:
- Owner OR (K guardians + 1 month timelock)
- Best of both worlds: solo control + social backup
- Industry standard for smart contract wallets

### 4. Multisig Covenants

**Restricted Multisig**:
- 2-of-3 multisig that can only send to whitelisted addresses
- Prevents insider theft to unauthorized addresses
- Use case: Corporate treasury

**Budget Multisig**:
- Multisig with per-period spending limits
- Small amounts: 1-of-3
- Large amounts: 2-of-3
- Use case: Team spending accounts

**Escrow Covenant**:
- 2-of-2 buyer + seller
- After timeout: refund to buyer
- Use case: Trustless escrow

---

## 🎨 Pattern API Design

### Pattern Builder Functions

```cpp
namespace dinero {
namespace wallet {
namespace patterns {

/**
 * Simple Vault Pattern
 *
 * Creates a vault with pre-committed withdrawal path
 *
 * Flow:
 * 1. Lock: Funds → Vault (CTV-locked)
 * 2. Unvault: Vault → Unvault (time-delayed)
 * 3. Withdraw: Unvault → Final destination
 *
 * Security: 24-hour delay gives time to react to theft
 */
struct SimpleVaultPattern {
    std::vector<uint8_t> vault_script;      // CTV-locked vault
    std::vector<uint8_t> unvault_script;    // Time-delayed unvault
    CTVOutput final_output;                 // Final destination
    uint32_t unvault_delay_blocks;          // Delay (e.g., 144 blocks = 24h)
};

SimpleVaultPattern createSimpleVault(
    const std::string& final_address,
    uint64_t amount,
    uint32_t delay_blocks = 144
);

/**
 * Recovery Vault Pattern
 *
 * Vault with emergency recovery key
 *
 * Paths:
 * 1. Normal: Owner vault → unvault → withdraw
 * 2. Recovery: Recovery key (after 6 months) → recover
 */
struct RecoveryVaultPattern {
    SimpleVaultPattern vault;               // Normal vault path
    std::vector<uint8_t> recovery_pubkey;   // Recovery key
    uint32_t recovery_delay_blocks;         // Recovery timelock (25920 = 6mo)
    std::vector<uint8_t> recovery_script;   // Recovery path script
};

RecoveryVaultPattern createRecoveryVault(
    const std::string& final_address,
    uint64_t amount,
    const std::vector<uint8_t>& recovery_pubkey,
    uint32_t vault_delay = 144,
    uint32_t recovery_delay = 25920
);

/**
 * Time-delayed Recovery Pattern
 *
 * Owner OR (Recovery key + timelock)
 *
 * Use case: Inheritance, lost keys
 */
struct TimeDelayedRecoveryPattern {
    std::vector<uint8_t> owner_pubkey;
    std::vector<uint8_t> recovery_pubkey;
    uint32_t recovery_delay_blocks;
    std::vector<uint8_t> spending_script;
};

TimeDelayedRecoveryPattern createTimeDelayedRecovery(
    const std::vector<uint8_t>& owner_pubkey,
    const std::vector<uint8_t>& recovery_pubkey,
    uint32_t delay_blocks = 25920  // 6 months
);

/**
 * Social Recovery Pattern (K-of-N)
 *
 * Owner OR (K guardians + timelock)
 *
 * Use case: Account recovery
 */
struct SocialRecoveryPattern {
    std::vector<uint8_t> owner_pubkey;
    std::vector<std::vector<uint8_t>> guardian_pubkeys;
    size_t threshold;                       // K (e.g., 3)
    uint32_t recovery_delay_blocks;
    std::vector<uint8_t> spending_script;
};

SocialRecoveryPattern createSocialRecovery(
    const std::vector<uint8_t>& owner_pubkey,
    const std::vector<std::vector<uint8_t>>& guardian_pubkeys,
    size_t threshold,
    uint32_t delay_blocks = 25920
);

/**
 * Restricted Multisig Pattern
 *
 * M-of-N multisig that can only send to whitelisted addresses
 *
 * Use case: Corporate treasury
 */
struct RestrictedMultisigPattern {
    std::vector<std::vector<uint8_t>> pubkeys;
    size_t threshold;
    std::vector<CTVOutput> whitelist;       // Allowed outputs
    std::vector<uint8_t> spending_script;
};

RestrictedMultisigPattern createRestrictedMultisig(
    const std::vector<std::vector<uint8_t>>& pubkeys,
    size_t threshold,
    const std::vector<CTVOutput>& whitelist
);

} // namespace patterns
} // namespace wallet
} // namespace dinero
```

---

## 🏗️ Implementation Strategy

### Vault Patterns (Using CTV)

**Simple Vault**:
1. Create CTV template committing to unvault tx
2. Unvault tx has timelock (OP_CHECKSEQUENCEVERIFY)
3. Final tx sends to destination

**Script Structure**:
```
Vault Script: <ctv_hash> OP_CHECKTEMPLATEVERIFY
Unvault Script: <delay> OP_CSV OP_DROP <pubkey> OP_CHECKSIG
```

### Recovery Patterns (Using Timelocks)

**Time-delayed Recovery**:
```
OP_IF
    <owner_pubkey> OP_CHECKSIG
OP_ELSE
    <recovery_delay> OP_CSV OP_DROP
    <recovery_pubkey> OP_CHECKSIG
OP_ENDIF
```

### Social Recovery (Using CSFS + Multisig)

**Hybrid Pattern**:
```
OP_IF
    <owner_pubkey> OP_CHECKSIG
OP_ELSE
    <recovery_delay> OP_CSV OP_DROP
    <guardian_1_pubkey> OP_CHECKSIGADD
    <guardian_2_pubkey> OP_CHECKSIGADD
    <guardian_3_pubkey> OP_CHECKSIGADD
    <threshold> OP_GREATERTHANOREQUAL
OP_ENDIF
```

### Multisig Covenants (Using CTV + Multisig)

**Restricted Multisig**:
1. Multisig unlocks funds
2. CTV restricts outputs to whitelist
3. Combination provides controlled spending

---

## 📋 Implementation Order

### Stage 1: Foundation Patterns (2-3 days)
1. Create `include/wallet/covenant_patterns.h`
2. Create `src/wallet/covenant_patterns.cpp`
3. Implement Simple Vault pattern
4. Implement Time-delayed Recovery pattern
5. Add basic tests

### Stage 2: Advanced Vaults (2-3 days)
1. Implement Recovery Vault pattern
2. Implement Hierarchical Vault pattern
3. Add vault-specific tests
4. Document vault use cases

### Stage 3: Social Recovery (2-3 days)
1. Implement K-of-N Social Recovery pattern
2. Implement Timelock Social Recovery pattern
3. Implement Hybrid Recovery pattern
4. Add social recovery tests

### Stage 4: Multisig Covenants (2-3 days)
1. Implement Restricted Multisig pattern
2. Implement Budget Multisig pattern
3. Implement Escrow Covenant pattern
4. Add multisig covenant tests

### Stage 5: Integration & Documentation (2-3 days)
1. Create pattern usage examples
2. Add RPC methods for patterns (optional)
3. Write pattern cookbook
4. Create integration tests
5. Sign off Phase C.5

**Total Duration**: 10-15 days

---

## 🗂️ File Structure

### New Files

**Headers**:
- `include/wallet/covenant_patterns.h` - Pattern API

**Implementation**:
- `src/wallet/covenant_patterns.cpp` - Pattern builders

**Tests**:
- `tests/covenant/test_covenant_patterns.cpp` - Pattern tests
- `tests/covenant/test_vault_patterns.cpp` - Vault-specific tests
- `tests/covenant/test_recovery_patterns.cpp` - Recovery tests

**Documentation**:
- `docs/covenant_patterns/VAULT_PATTERNS.md` - Vault documentation
- `docs/covenant_patterns/RECOVERY_PATTERNS.md` - Recovery docs
- `docs/covenant_patterns/SOCIAL_RECOVERY.md` - Social recovery docs
- `docs/covenant_patterns/PATTERN_COOKBOOK.md` - Usage examples

---

## 🎯 Pattern Examples

### Example 1: Simple Vault

**Use Case**: Store 10 BTC with 24-hour withdrawal delay

```cpp
auto vault = dinero::wallet::patterns::createSimpleVault(
    "tb1q...",  // Final destination
    1000000000, // 10 BTC in sats
    144         // 24 hours (144 blocks)
);

// Step 1: Fund vault
auto funding_tx = /* ... create tx sending to vault.vault_script ... */;

// Step 2: Unvault (after 24 hours)
auto unvault_tx = /* ... spend vault with witness ... */;

// Step 3: Withdraw (immediate)
auto withdraw_tx = /* ... spend unvault to final destination ... */;
```

### Example 2: Social Recovery

**Use Case**: Account with 3-of-5 guardian recovery

```cpp
std::vector<std::vector<uint8_t>> guardians = {
    friend1_pubkey, friend2_pubkey, friend3_pubkey,
    friend4_pubkey, friend5_pubkey
};

auto recovery = dinero::wallet::patterns::createSocialRecovery(
    owner_pubkey,
    guardians,
    3,      // Require 3 guardians
    25920   // 6 months delay
);

// Owner can spend immediately
// OR 3 guardians can recover after 6 months
```

### Example 3: Restricted Multisig

**Use Case**: Corporate treasury (2-of-3) with whitelisted addresses

```cpp
std::vector<CTVOutput> whitelist = {
    {payroll_address, 0},     // Payroll
    {vendor_address, 0},      // Vendors
    {savings_address, 0}      // Savings
};

auto treasury = dinero::wallet::patterns::createRestrictedMultisig(
    {cfo_pubkey, cto_pubkey, ceo_pubkey},
    2,          // 2-of-3
    whitelist
);

// Multisig can only send to whitelisted addresses
```

---

## 🛡️ Security Considerations

### Vault Patterns

**Simple Vault**:
- ✅ Provides time to react to theft
- ⚠️ Vulnerable to fee sniping (mitigate with CPFP)
- ⚠️ All parameters public (no privacy)

**Recovery Vault**:
- ✅ Emergency recovery if main key lost
- ⚠️ Recovery key must be secured separately
- ⚠️ Long delays can be inconvenient

### Recovery Patterns

**Time-delayed Recovery**:
- ✅ Automatic inheritance
- ⚠️ Recovery key holder must wait (can't steal immediately)
- ⚠️ Owner must periodically "refresh" to prevent accidental recovery

**Social Recovery**:
- ✅ Resistant to single point of failure
- ⚠️ Guardian collusion risk (choose independent guardians)
- ⚠️ Guardians learn about recovery attempt (privacy leak)

### Multisig Covenants

**Restricted Multisig**:
- ✅ Prevents unauthorized destinations
- ⚠️ Whitelist must be updated via new covenant
- ⚠️ All destinations public (on-chain analysis)

---

## 🚫 Explicitly Out of Scope

**Phase C.5 will NOT include**:
- ❌ New opcodes or consensus changes
- ❌ Dynamic covenants (state changes)
- ❌ Turing-complete scripts
- ❌ Cross-chain covenants
- ❌ Privacy-preserving covenants (separate phase)
- ❌ Lightning-specific covenants (separate phase)
- ❌ GUI for pattern creation (separate phase)

---

## Success Criteria

Phase C.5 is complete when:

- ✅ Simple Vault pattern implemented
- ✅ Recovery Vault pattern implemented
- ✅ Time-delayed Recovery pattern implemented
- ✅ Social Recovery (K-of-N) pattern implemented
- ✅ Restricted Multisig pattern implemented
- ✅ All pattern tests passing
- ✅ Pattern documentation complete
- ✅ Usage examples provided
- ✅ No new primitives added (only composition)

---

## Testing Strategy

### Pattern Tests

**For Each Pattern**:
1. Create pattern with valid parameters
2. Verify script structure correct
3. Test normal spending path
4. Test recovery/fallback paths
5. Test timelock enforcement
6. Test invalid spending attempts

### Integration Tests

1. End-to-end vault flow (fund → unvault → withdraw)
2. Social recovery activation
3. Multisig covenant spending
4. Time-based recovery scenarios

---

## Risk Mitigation

**Risk 1**: Patterns too complex for users
- **Mitigation**: Provide clear documentation and examples

**Risk 2**: Patterns have edge cases or vulnerabilities
- **Mitigation**: Extensive testing, security review

**Risk 3**: Patterns not composable
- **Mitigation**: Design patterns as modular building blocks

---

## Next Immediate Action

**Start with Simple Vault**:

1. Create `include/wallet/covenant_patterns.h`
2. Define `SimpleVaultPattern` struct
3. Implement `createSimpleVault()` function
4. Add basic test
5. Iterate to other patterns

---

## Phase Dependencies

**Requires**:
- ✅ Phase C.1 (covenant consensus) - COMPLETE
- ✅ Phase C.2 (covenant mempool policy) - COMPLETE
- ✅ Phase C.3 (covenant construction helpers) - COMPLETE
- ✅ Phase C.4 (covenant RPC endpoints) - COMPLETE

**Enables**:
- Production vault deployments
- Real-world covenant applications
- User-friendly covenant tools
- Advanced custody solutions

---

**Plan Status**: ✅ Ready for implementation
