# Phase 35: Wallet RPC Completeness

## Overview

**Status**: In Progress
**Phase**: 35
**Dependencies**: Phase 1 (frozen), Phase 34 (complete)
**Risk Level**: Low (sits above frozen core)

## Purpose

Implement essential wallet RPC methods that provide UTXO visibility, balance reconciliation, and manual transaction management. These RPCs sit **above** the frozen wallet core and provide user-facing functionality without modifying consensus logic.

## Why This Phase Now

Phase 34 closed the economic loop (wallet → mempool → block → chain). Phase 35 makes this loop **observable and controllable** through standard RPC interfaces.

**What this unlocks:**
- UTXO visibility (critical for debugging)
- Balance reconciliation (trust through verification)
- Manual UTXO management (power user features)
- Mempool transaction control (recovery from stuck txs)
- Better transaction building (manual fees, coin control)

**What this does NOT touch:**
- Wallet internals (frozen in Phase 1)
- Coin selection algorithm (frozen in Phase 1.1)
- Ownership semantics (frozen in Phase 1)
- Mempool consensus logic (frozen in Phase 34)

## Entry Criteria

✅ Phase 1 complete (wallet consensus locked)
✅ Phase 1.1 complete (coin selection boundary enforced)
✅ Phase 34 complete (mempool → block assembly integrated)
✅ All 64 Phase 1 tests passing
✅ WalletManager has UTXO enumeration capability
✅ CoinsDB provides confirmed/unconfirmed state

## RPC Implementation Order

### 1. wallet.listunspent (FOUNDATIONAL)

**Priority**: CRITICAL - Everything depends on this

#### Purpose
Expose all UTXOs controlled by the wallet with full metadata for debugging, balance verification, and coin control.

#### Inputs
```json
{
  "minconf": 1,           // Minimum confirmations (default: 1)
  "maxconf": 9999999,     // Maximum confirmations (default: 9999999)
  "addresses": [],        // Filter by addresses (optional)
  "include_unsafe": false,// Include unconfirmed txs (default: false)
  "query_options": {      // Advanced filters (optional)
    "minimumAmount": 0,
    "maximumAmount": null,
    "maximumCount": null,
    "minimumSumAmount": null
  }
}
```

#### Outputs
```json
{
  "unspent": [
    {
      "txid": "abc123...",           // Transaction ID (hex)
      "vout": 0,                      // Output index
      "address": "rdin1p...",         // Display address (informational only)
      "scriptPubKey": "5120...",      // Hex scriptPubKey (AUTHORITATIVE for ownership)
      "amount": 1.23456789,           // Amount in DIN
      "amount_una": 123456789,       // Amount in una (una)
      "confirmations": 42,            // Block depth
      "spendable": true,              // Can be spent (not locked, mature if coinbase)
      "solvable": true,               // Wallet has keys to spend
      "safe": true,                   // Confirmed and not double-spend candidate
      "is_coinbase": false,           // Is coinbase output
      "locked": false,                // Locked via lockunspent
      "witness_version": 1,           // 0=SegWit v0, 1=Taproot, 0xFF=legacy
      "label": "Mining reward"        // Address label (if set)
    }
  ]
}
```

#### Error Cases
- **Wallet not loaded**: `{"error": "Wallet not loaded"}`
- **Wallet locked**: `{"error": "Wallet is locked"}`
- **Invalid parameters**: `{"error": "Invalid minconf/maxconf"}`

#### Invariants
```
✅ Ownership resolution MUST use scriptPubKey (not address)
✅ Address field is DISPLAY ONLY (never used for matching)
✅ spendable = true IFF:
   - Not locked via lockunspent
   - confirmations >= 100 (if is_coinbase)
   - confirmations >= minconf
   - Has private key (solvable)
❌ MUST NOT compare addresses for ownership
❌ MUST NOT mutate wallet state
```

#### What It Is NOT Allowed To Do
- ❌ Filter by address string comparison (use scriptPubKey)
- ❌ Modify UTXO set
- ❌ Trigger rescans
- ❌ Change locked status (that's lockunspent's job)

---

### 2. wallet.getbalance

**Priority**: HIGH - Required for trust through verification

#### Purpose
Provide detailed balance breakdown that reconciles exactly with listunspent. No magic math - every una must be explainable.

#### Inputs
```json
{
  "minconf": 1,           // Minimum confirmations for "confirmed" (default: 1)
  "include_watchonly": false,  // Include watch-only addresses (default: false)
  "avoid_reuse": false    // Avoid spending from reused addresses (default: false)
}
```

#### Outputs
```json
{
  "confirmed": 1234.56789,      // >= minconf confirmations
  "unconfirmed": 12.34,         // < minconf confirmations (not in block)
  "immature": 100.0,            // Coinbase outputs < 100 confirmations
  "locked": 50.0,               // Locked via lockunspent
  "total": 1396.90789,          // Sum of all above
  "breakdown": {
    "spendable": 1234.56789,    // confirmed - locked
    "pending": 12.34,           // unconfirmed
    "unspendable": 150.0        // immature + locked
  }
}
```

#### Error Cases
- **Wallet not loaded**: `{"error": "Wallet not loaded"}`
- **Invalid minconf**: `{"error": "minconf must be >= 0"}`

#### Invariants
```
✅ total = confirmed + unconfirmed + immature
✅ spendable = confirmed - locked
✅ Every balance component MUST reconcile with listunspent
✅ Math MUST be exact (no floating point errors in una)
❌ MUST NOT show balances for addresses we don't own (scriptPubKey check)
```

#### Reconciliation Test
```python
utxos = listunspent(minconf=0, maxconf=9999999)
confirmed = sum(u.amount for u in utxos if u.confirmations >= minconf)
unconfirmed = sum(u.amount for u in utxos if 0 < u.confirmations < minconf)
immature = sum(u.amount for u in utxos if u.is_coinbase and u.confirmations < 100)
locked = sum(u.amount for u in utxos if u.locked)

assert getbalance().confirmed == confirmed
assert getbalance().unconfirmed == unconfirmed
assert getbalance().immature == immature
assert getbalance().locked == locked
```

#### What It Is NOT Allowed To Do
- ❌ Perform address-based balance calculations
- ❌ Cache balances independently of UTXO set
- ❌ Mutate wallet state

---

### 3. wallet.lockunspent

**Priority**: MEDIUM - Required for manual UTXO management

#### Purpose
Lock/unlock specific UTXOs to control which coins are used for spending. Critical for power users and testing.

#### Inputs
```json
{
  "unlock": false,  // true=unlock, false=lock
  "outputs": [
    {
      "txid": "abc123...",
      "vout": 0
    }
  ],
  "persistent": false  // Survive restarts (default: false)
}
```

#### Outputs
```json
{
  "success": true,
  "locked": [
    {
      "txid": "abc123...",
      "vout": 0
    }
  ]
}
```

#### Special Cases
- **Lock all**: `{"unlock": false, "outputs": []}` - locks all UTXOs
- **Unlock all**: `{"unlock": true, "outputs": []}` - unlocks all UTXOs
- **Query locked**: No outputs, no unlock param - returns currently locked UTXOs

#### Error Cases
- **Wallet not loaded**: `{"error": "Wallet not loaded"}`
- **UTXO not found**: `{"error": "UTXO not found: txid:vout"}`
- **Already spent**: `{"error": "UTXO already spent"}`

#### Invariants
```
✅ Locking affects coin selection input set ONLY
✅ Locked UTXOs still appear in listunspent (with locked=true)
✅ Locked UTXOs are excluded from CoinSelector input set
✅ Lock state persists until unlock or restart (if persistent=false)
✅ Lock state survives restart (if persistent=true)
❌ MUST NOT delete or mutate UTXOs
❌ MUST NOT affect blockchain or mempool state
```

#### Implementation Notes
- Lock state stored in wallet database (if persistent=true)
- Lock state stored in memory (if persistent=false)
- CoinSelector MUST check locked status before selecting

#### What It Is NOT Allowed To Do
- ❌ Modify UTXO ownership
- ❌ Remove UTXOs from wallet
- ❌ Affect confirmed vs unconfirmed status

---

### 4. wallet.abandontransaction

**Priority**: MEDIUM - Required for mempool management

#### Purpose
Mark an unconfirmed transaction as abandoned, returning its inputs to the available set for spending. Critical for recovering from stuck transactions.

#### Inputs
```json
{
  "txid": "abc123..."  // Transaction ID to abandon
}
```

#### Outputs
```json
{
  "success": true,
  "abandoned": "abc123...",
  "inputs_returned": 3,  // Number of inputs now available again
  "amount_returned": 123.456  // Total amount returned (DIN)
}
```

#### Error Cases
- **Wallet not loaded**: `{"error": "Wallet not loaded"}`
- **Transaction not found**: `{"error": "Transaction not found in wallet"}`
- **Already confirmed**: `{"error": "Cannot abandon confirmed transaction"}`
- **Not in mempool**: `{"error": "Transaction not in mempool"}`
- **Has descendants**: `{"error": "Cannot abandon transaction with descendants in mempool"}`

#### Invariants
```
✅ Only affects unconfirmed transactions (confirmations = 0)
✅ Returns inputs to spendable set
✅ Marks transaction as abandoned in wallet DB
✅ Does NOT remove transaction from wallet history
✅ Does NOT affect blockchain or other nodes
❌ MUST NOT abandon confirmed transactions
❌ MUST NOT rewrite blockchain history
❌ MUST NOT affect other wallet's view of the transaction
```

#### Rules
1. Transaction MUST be:
   - In wallet
   - Unconfirmed (0 confirmations)
   - In mempool (optional check - can abandon even if evicted)

2. Transaction MUST NOT have:
   - Any confirmations
   - Any descendants in mempool
   - Been abandoned already

3. After abandonment:
   - Inputs return to available set
   - Transaction marked as abandoned in wallet
   - Can create replacement transaction

#### What It Is NOT Allowed To Do
- ❌ Abandon confirmed transactions
- ❌ Remove transaction from wallet history
- ❌ Affect other nodes' mempools
- ❌ Automatically create replacement transaction

---

### 5. wallet.sendtoaddress Enhancements

**Priority**: LOW - Quality of life improvements

#### Purpose
Add manual control over transaction construction without duplicating coin selection logic.

#### New Parameters
```json
{
  "address": "rdin1p...",
  "amount": 1.23,
  "options": {
    // Existing
    "test_mode": false,
    "fee_rate": 1.0,

    // NEW: Fee control
    "subtractfeefromamount": false,  // Deduct fee from recipient (default: false)
    "fee_override": null,            // Exact fee in DIN (bypasses estimation)

    // NEW: Coin control
    "inputs": [                      // Manually select UTXOs (optional)
      {
        "txid": "abc123...",
        "vout": 0
      }
    ],

    // NEW: Change control
    "change_address": null,          // Explicit change address (optional)

    // NEW: Advanced
    "replaceable": true,             // Enable RBF (BIP 125) (default: true)
    "conf_target": 6,                // Confirmation target for fee estimation
    "estimate_mode": "ECONOMICAL"    // UNSET/ECONOMICAL/CONSERVATIVE
  }
}
```

#### Outputs
```json
{
  "txid": "abc123...",
  "status": "signed_and_submitted",
  "accepted": true,
  "amount": 1.23,
  "fee": 0.00000141,
  "fee_rate": 1.0,
  "inputs": 2,
  "outputs": 2,
  "change": 0.76999859,
  "change_address": "rdin1p...",
  "size": 141,
  "vsize": 141,
  "weight": 564
}
```

#### Invariants
```
✅ MUST delegate to CoinSelector (never bypass)
✅ If inputs specified, MUST verify ownership
✅ If inputs specified, MUST verify spendable status
✅ subtractfeefromamount: recipient gets (amount - fee)
✅ fee_override: skip estimation, use exact fee
❌ MUST NOT reimplement coin selection
❌ MUST NOT bypass scriptPubKey ownership checks
❌ MUST NOT create invalid transactions
```

#### Implementation Rules

**For subtractfeefromamount=true:**
```
output_amount = amount - fee
change_amount = total_inputs - output_amount
```

**For manual inputs:**
```
1. Verify all inputs exist in wallet
2. Verify all inputs are spendable
3. Verify all inputs are unspent
4. Skip CoinSelector, use provided inputs
5. Calculate change normally
```

**For fee_override:**
```
1. Skip fee estimation
2. Use exact fee provided
3. Validate: total_inputs >= total_outputs + fee
4. Warn if fee is unusually high/low
```

#### What It Is NOT Allowed To Do
- ❌ Bypass CoinSelector for automatic selection
- ❌ Select coins via address matching
- ❌ Modify coin selection algorithm

---

## Implementation Order (Fixed)

```
Step 1: wallet.listunspent     ← Foundation for everything
Step 2: wallet.getbalance      ← Builds on listunspent
Step 3: wallet.lockunspent     ← Affects listunspent output
Step 4: wallet.abandontransaction ← Mempool management
Step 5: sendtoaddress enhancements ← Builds on coin control
```

## Exit Criteria

Phase 35 is complete when:

✅ **wallet.listunspent is accurate and stable**
- Returns all wallet UTXOs
- scriptPubKey is authoritative for ownership
- spendable flag respects locked/mature/confirmed rules
- All metadata fields populated correctly

✅ **Balances reconcile exactly with UTXOs**
- getbalance() components sum correctly
- No floating point errors in una
- Every una explainable via listunspent

✅ **Locked coins never get spent**
- CoinSelector respects locked status
- Locked UTXOs excluded from automatic selection
- Manual selection can override locks (explicit only)

✅ **Abandoned txs return inputs correctly**
- Inputs become spendable again
- Transaction marked abandoned in wallet
- No effect on blockchain or other nodes

✅ **All Phase 1 + Phase 34 tests still pass unchanged**
- No regressions in wallet core
- No regressions in coin selection
- No regressions in mempool integration

✅ **New integration tests pass**
- RPC-level tests for each endpoint
- Balance reconciliation tests
- Coin locking tests
- Abandonment tests

## Testing Strategy

### Prefer RPC-Level Integration Tests

```bash
tests/wallet_rpc/test_listunspent.sh
tests/wallet_rpc/test_balance_reconciliation.sh
tests/wallet_rpc/test_coin_locking.sh
tests/wallet_rpc/test_abandonment.sh
tests/wallet_rpc/test_sendtoaddress_enhancements.sh
```

### No New Consensus Tests
- Phase 35 does NOT touch consensus
- Use existing Phase 1 + Phase 34 tests for regression

### No New Wallet Core Tests (Unless Unavoidable)
- If a test needs internal access → that's a smell
- All behavior should be testable via RPC

## Invariants to Preserve

### Phase 1 Invariants (FROZEN)
```
✅ scriptPubKey-based ownership (NEVER address strings)
✅ BIP341 Taproot signing (internal key, no private key tweak)
✅ Single coin selection engine (CoinSelector)
✅ Premine recovery semantics (locked)
✅ Consensus constants (locked)
```

### Phase 34 Invariants (FROZEN)
```
✅ Mempool is authoritative for block assembly
✅ CPFP-aware transaction ordering
✅ Automatic mempool cleanup after mining
```

### Phase 35 Invariants (NEW)
```
✅ listunspent is source of truth for balance verification
✅ All balance components reconcile with UTXOs
✅ Locked coins excluded from automatic selection
✅ Abandoned txs do not affect blockchain
✅ Manual coin selection does not bypass ownership checks
```

## What Phase 35 Is NOT

❌ **NOT** wallet core refactoring
❌ **NOT** coin selection improvements
❌ **NOT** consensus changes
❌ **NOT** mempool policy changes
❌ **NOT** optimization of existing code

Phase 35 is **ONLY** RPC interface improvements above the frozen core.

## Completion Tag

When all exit criteria met:
```bash
git tag -a phase35-wallet-rpc-complete -m "Phase 35: Wallet RPC Completeness

✅ wallet.listunspent - UTXO visibility
✅ wallet.getbalance - Balance reconciliation
✅ wallet.lockunspent - Manual UTXO management
✅ wallet.abandontransaction - Mempool recovery
✅ sendtoaddress enhancements - Manual fees & coin control

All Phase 1 + Phase 34 tests passing.
Zero regressions in frozen core."
```

## Risk Assessment

**Overall Risk: LOW**

- ✅ Sits above frozen core
- ✅ No consensus changes
- ✅ No wallet internals modification
- ✅ Easy to test via RPC
- ✅ Easy to revert if issues found

**Potential Issues:**
- Balance calculation errors (mitigated by reconciliation tests)
- Lock state persistence bugs (mitigated by explicit tests)
- Abandonment edge cases (mitigated by strict rules)

**Mitigation:**
- Comprehensive RPC-level tests
- Balance reconciliation assertions
- Phase 1 + Phase 34 regression tests

## Timeline Estimate

- **Step 1 (listunspent)**: 3-4 hours (foundation)
- **Step 2 (getbalance)**: 2-3 hours (reconciliation logic)
- **Step 3 (lockunspent)**: 2-3 hours (state management)
- **Step 4 (abandontransaction)**: 2-3 hours (mempool interaction)
- **Step 5 (sendtoaddress++)**: 3-4 hours (parameter handling)
- **Testing & Documentation**: 3-4 hours
- **Total**: 15-21 hours

## Success Metrics

1. **Usability**: Users can see and control their UTXOs
2. **Trust**: Balances are explainable and verifiable
3. **Power**: Manual transaction control available
4. **Stability**: Zero regressions in frozen core
5. **Professional**: Clean RPC interface matching Bitcoin Core patterns

---

**Status**: Ready to begin Step 1 (wallet.listunspent)
**Next Action**: Explore existing UTXO enumeration code in WalletManager
