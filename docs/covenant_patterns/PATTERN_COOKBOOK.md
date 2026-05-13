# Covenant Pattern Cookbook

**Phase C.5: High-Level Wallet Recipes**

This cookbook provides practical examples of using covenant patterns for real-world use cases.

---

## Table of Contents

1. [Vault Patterns](#vault-patterns)
2. [Recovery Flows](#recovery-flows)
3. [Social Recovery](#social-recovery)
4. [Multisig Covenants](#multisig-covenants)

---

## Vault Patterns

### Pattern 1: Simple Vault (Cold Storage with 24h Delay)

**Use Case**: Secure long-term savings with theft protection

**Security Model**:
- Funds locked in vault (CTV-restricted)
- Withdrawal requires 2 steps with 24h delay
- If theft detected, sweep to emergency address

**Example**:
```cpp
#include "wallet/covenant_patterns.h"

// Create vault for 10 BTC with 24-hour withdrawal delay
auto vault = dinero::wallet::patterns::createSimpleVault(
    "tb1qmysavingsaddressxyz...",  // Final destination
    1000000000,                      // 10 BTC (in una)
    144,                             // 24 hours (144 blocks @ 10min/block)
    "my-cold-storage"
);

// Step 1: Fund the vault
// Send 10 BTC to vault.vault_script

// Step 2: Unvault (triggers 24h countdown)
// Broadcast transaction spending vault to unvault_script

// Step 3: Wait 24 hours
// Monitor for unauthorized unvault attempts
// If theft detected: sweep to emergency address

// Step 4: Withdraw (after 24h)
// Complete withdrawal to final destination
```

**When to Use**:
- Long-term savings (>$10k)
- Infrequent withdrawals
- Want protection against hot wallet compromise

**Security Properties**:
- ✅ Time to detect theft
- ✅ Can cancel malicious withdrawals
- ⚠️ Inconvenient for frequent access

---

### Pattern 2: Recovery Vault (Vault with Emergency Key)

**Use Case**: Vault with backup recovery option

**Security Model**:
- Normal path: Standard vault flow (24h delay)
- Recovery path: Emergency key (6 month delay)
- If main key lost, recovery key activates after long wait

**Example**:
```cpp
// Generate recovery key (store securely, e.g., with lawyer)
auto recovery_pubkey = /* ... generate or import recovery key ... */;

// Create recovery vault
auto vault = dinero::wallet::patterns::createRecoveryVault(
    "tb1qfinaldestination...",
    500000000,       // 5 BTC
    recovery_pubkey,
    144,             // Normal: 24h delay
    25920,           // Recovery: ~6 months (25920 blocks)
    "inheritance-vault"
);

// Normal operations use vault.vault path (24h delay)
// If main key lost, recovery_pubkey can spend after 6 months
```

**When to Use**:
- Estate planning
- Key backup scenarios
- Inheritance vaults

**Security Properties**:
- ✅ No loss of funds if key lost
- ✅ Recovery requires long delay (prevents theft)
- ⚠️ Must secure recovery key separately

---

## Recovery Flows

### Pattern 3: Time-Delayed Recovery (Inheritance)

**Use Case**: Allow heir to inherit after prolonged inactivity

**Security Model**:
- Owner has immediate access (can spend anytime)
- Heir can access after 6 months of inactivity
- Owner activity resets timelock

**Example**:
```cpp
auto owner_pubkey = /* ... your pubkey ... */;
auto heir_pubkey = /* ... heir's pubkey ... */;

auto pattern = dinero::wallet::patterns::createTimeDelayedRecovery(
    owner_pubkey,
    heir_pubkey,
    25920,  // 6 months
    "alice-wallet",
    "bob-heir"
);

// Alice can spend anytime (immediate access)
// Bob can spend after 6 months of Alice's inactivity

// Important: Alice should periodically "refresh" by moving funds
// to reset the timelock and prevent accidental inheritance
```

**When to Use**:
- Estate planning
- Automatic inheritance
- Lost key backup

**Security Properties**:
- ✅ Automatic inheritance (no manual process)
- ✅ Owner maintains full control
- ⚠️ Heir must wait full delay period

---

## Social Recovery

### Pattern 4: Social Recovery (3-of-5 Guardians)

**Use Case**: Account recovery via trusted friends/family

**Security Model**:
- Owner has immediate access
- 3-of-5 guardians can recover after 6 months
- Prevents guardian collusion (requires 3 + wait)

**Example**:
```cpp
auto owner_pubkey = /* ... your pubkey ... */;

// Choose 5 trusted guardians
std::vector<std::vector<uint8_t>> guardians = {
    alice_pubkey,     // Friend
    bob_pubkey,       // Family member
    charlie_pubkey,   // Colleague
    david_pubkey,     // Lawyer
    eve_pubkey        // Backup friend
};

std::vector<std::string> labels = {
    "alice-friend",
    "bob-family",
    "charlie-colleague",
    "david-lawyer",
    "eve-backup"
};

auto pattern = dinero::wallet::patterns::createSocialRecovery(
    owner_pubkey,
    guardians,
    3,      // Require 3 guardians
    25920,  // 6 months delay
    "my-wallet",
    labels
);

// Normal: Owner can spend immediately
// Recovery: Any 3 guardians can recover after 6 months inactivity
```

**When to Use**:
- Account recovery
- Wallet backup
- Corporate key recovery

**Guardian Selection Best Practices**:
- Choose **independent** guardians (not all from same family/company)
- Guardians should be **long-term** contacts
- Mix **jurisdictions** (different countries/states)
- Avoid guardians who communicate regularly (reduces collusion risk)

**Security Properties**:
- ✅ Resistant to single point of failure
- ✅ Requires K signatures + time delay
- ⚠️ Guardian collusion possible (choose wisely)

---

## Multisig Covenants

### Pattern 5: Restricted Multisig (Corporate Treasury)

**Use Case**: Company treasury with spending restrictions

**Security Model**:
- 2-of-3 multisig (CFO, CTO, CEO)
- Can ONLY send to whitelisted addresses
- Even with 2 signatures, cannot send to unauthorized address

**Example**:
```cpp
// Company signers
std::vector<std::vector<uint8_t>> signers = {
    cfo_pubkey,
    cto_pubkey,
    ceo_pubkey
};

std::vector<std::string> labels = {"cfo", "cto", "ceo"};

// Whitelisted destinations
std::vector<dinero::wallet::CTVOutput> whitelist;

dinero::wallet::CTVOutput payroll;
payroll.value = 0;  // Variable amount
payroll.address = "tb1qpayrolladdress...";
whitelist.push_back(payroll);

dinero::wallet::CTVOutput vendor_a;
vendor_a.value = 0;
vendor_a.address = "tb1qvendoraaddress...";
whitelist.push_back(vendor_a);

dinero::wallet::CTVOutput savings;
savings.value = 0;
savings.address = "tb1qcompanysavings...";
whitelist.push_back(savings);

auto treasury = dinero::wallet::patterns::createRestrictedMultisig(
    signers,
    2,  // 2-of-3
    whitelist,
    labels,
    {"payroll", "vendor-a", "savings"}
);

// Company can spend with 2-of-3 signatures
// BUT can only send to: payroll, vendor-a, or savings addresses
```

**When to Use**:
- Corporate treasuries
- Exchange cold storage (limit to hot wallet only)
- DAO treasuries (limit to approved grants)

**Security Properties**:
- ✅ Prevents insider theft to unauthorized address
- ✅ Enforces spending policy on-chain
- ⚠️ Whitelist is immutable (requires new covenant to change)

---

### Pattern 6: Escrow Covenant (Trustless Escrow)

**Use Case**: Buyer-seller escrow with timeout refund

**Security Model**:
- Happy path: 2-of-2 buyer + seller (instant release)
- Dispute path: Buyer gets refund after timeout

**Example**:
```cpp
auto buyer_pubkey = /* ... buyer's pubkey ... */;
auto seller_pubkey = /* ... seller's pubkey ... */;

auto escrow = dinero::wallet::patterns::createEscrowCovenant(
    buyer_pubkey,
    seller_pubkey,
    100000000,  // 1 BTC escrow amount
    "tb1qselleraddress...",  // Seller gets paid if both agree
    "tb1qbuyeraddress...",   // Buyer gets refund on timeout
    2016,       // 2 weeks timeout (~2016 blocks)
    "laptop-purchase-agreement"
);

// Flow:
// 1. Buyer funds escrow (1 BTC locked)
// 2. Seller delivers product
// 3. If buyer satisfied: both sign → seller paid instantly
// 4. If dispute: buyer waits 2 weeks → auto-refund
```

**When to Use**:
- Trustless escrow services
- Marketplace purchases
- Service payments with guarantees

**Security Properties**:
- ✅ Buyer protected (gets refund if seller doesn't deliver)
- ✅ Seller protected (must deliver within timeout)
- ✅ No third-party arbiter needed

---

## Pattern Comparison

| Pattern | Use Case | Delay | Complexity | Key Count |
|---------|----------|-------|------------|-----------|
| Simple Vault | Cold storage | 24h | Low | 1 |
| Recovery Vault | Inheritance backup | 24h + 6mo | Medium | 2 |
| Time-Delayed Recovery | Automatic inheritance | 6mo | Low | 2 |
| Social Recovery | Account recovery | 6mo | High | 1 + N guardians |
| Restricted Multisig | Corporate treasury | None | Medium | M-of-N |
| Escrow Covenant | Trustless escrow | 2wk | Medium | 2 |

---

## Best Practices

### General Guidelines

1. **Test on Testnet First**: Always test patterns on testnet before mainnet
2. **Verify Addresses**: Double-check all destination addresses
3. **Document Recovery**: Write down recovery procedures
4. **Secure Keys**: Use hardware wallets for all keys
5. **Monitor Activity**: Set up alerts for unauthorized unvaults

### Pattern-Specific

**Vaults**:
- Use longer delays for larger amounts
- Monitor mempool for unauthorized unvaults
- Have emergency sweep procedure ready

**Social Recovery**:
- Choose geographically distributed guardians
- Inform guardians of their role
- Test recovery flow annually

**Multisig Covenants**:
- Regularly review whitelist addresses
- Plan for address rotation
- Document updating procedures

---

## Common Mistakes to Avoid

1. ❌ **Too short delays**: 1-hour delay insufficient for theft detection
2. ❌ **Poor guardian selection**: All guardians from same family/company
3. ❌ **No monitoring**: Must watch for unauthorized unvaults
4. ❌ **Lost recovery keys**: Backup recovery keys securely
5. ❌ **Immutable whitelist**: Plan for changing whitelist addresses

---

## Next Steps

After creating patterns:

1. **Test**: Run pattern tests (`test_covenant_patterns`)
2. **Deploy**: Fund patterns on testnet
3. **Monitor**: Set up monitoring for pattern activity
4. **Document**: Write recovery procedures for your specific setup
5. **Review**: Annually review and update patterns as needed

---

## Further Reading

- [Vault Patterns](VAULT_PATTERNS.md) - Deep dive into vault security
- [Recovery Patterns](RECOVERY_PATTERNS.md) - Recovery flow details
- [Social Recovery](SOCIAL_RECOVERY.md) - Guardian selection guide

---

**Pattern Cookbook Status**: ✅ Complete

All patterns tested and ready for production use with proper precautions.
