# Phase E — Wallet, Mining & RPC Policy Contracts

**Purpose**: Define explicit contracts for wallet, mining, and RPC layer behavior.
**Status**: Phase E.0 - Contract Definition (NOT implementation yet)

These contracts prevent future drift and establish what behavior is **guaranteed**, **forbidden**, or **undefined**.

---

## Contract Philosophy

> **Phase D proved correctness.**
> **Phase E enforces safety.**

- Phase D: "Does the engine work?"
- Phase E: "Can the engine be misused?"

Contracts are written **before** code to prevent:
- Scope creep during implementation
- Implicit assumptions becoming bugs
- Future weakening of safety guarantees
- Undefined behavior in edge cases

---

## E.1 — Mining Contracts

### E.1.1 Mining Ownership Contract

**MUST**: Mining requires explicit wallet ownership

```
INVARIANT: mining.start MUST fail if mining address is not owned by active wallet

Exception: --allow-external-mining=true (dangerous, requires explicit flag)
```

**Guarantees**:
- ✅ Mining address MUST belong to loaded wallet (verified via `WalletManager::isAddressMine()`)
- ✅ Mining address MUST be from currently active wallet (not a different wallet)
- ✅ Wallet MUST be in unlocked state (if encrypted)

**Forbidden**:
- ❌ Mining to arbitrary addresses without wallet ownership
- ❌ Mining with locked encrypted wallet
- ❌ Mining before wallet is loaded

**Error Behavior**:
```json
// mining.start called with non-owned address
{
  "error": {
    "code": -13,
    "message": "Mining address not owned by active wallet. Load wallet first or use --allow-external-mining."
  }
}

// mining.start called with locked wallet
{
  "error": {
    "code": -13,
    "message": "Wallet is locked. Unlock wallet before mining to ensure reward security."
  }
}
```

**Rationale**: Prevents accidental loss of mining rewards to addresses the user cannot spend from.

---

### E.1.2 Mining Lifecycle Contract

**MUST**: Mining state is explicit, never implicit

```
INVARIANT: mining.start MUST be called explicitly after daemon restart
```

**Guarantees**:
- ✅ Mining does NOT auto-resume after daemon restart (Bitcoin Core behavior)
- ✅ Mining address persists (proven in Phase D.3)
- ✅ Mining state is queryable via `mining.info`

**Forbidden**:
- ❌ Auto-resume mining on daemon restart (silent, confusing)
- ❌ Implicit mining based on config file (must be explicit RPC call)

**Error Behavior**:
```json
// mining.info after restart (when mining was active before shutdown)
{
  "mining": false,
  "last_session": {
    "was_mining": true,
    "stopped_at": 1735416000,
    "reason": "daemon_shutdown"
  },
  "message": "Mining stopped on shutdown. Call mining.start to resume."
}
```

**Rationale**: Explicit > Implicit. Users should consciously decide to mine, not rely on automatic behavior.

---

### E.1.3 Mining Resource Safety Contract

**MUST**: Mining respects thread limits and resource constraints

```
INVARIANT: mining.start MUST enforce thread limits (1-16 threads)
```

**Guarantees**:
- ✅ Thread count MUST be between 1 and 16 (or system max, whichever is lower)
- ✅ Thread count=0 means auto-detect optimal count (not "disable mining")
- ✅ Invalid thread counts rejected before mining starts

**Forbidden**:
- ❌ Thread count > 16 (prevents resource exhaustion)
- ❌ Thread count < 0 (invalid)
- ❌ Negative or NaN thread values

**Error Behavior**:
```json
// mining.start with invalid thread count
{
  "error": {
    "code": -32602,
    "message": "Invalid thread count: 32. Must be between 1-16 or 0 for auto-detect."
  }
}
```

**Rationale**: Prevents users from accidentally DoS-ing their own systems.

---

## E.2 — IBD & Sync Safety Contracts

### E.2.1 Initial Block Download Contract

**MUST**: Mining is blocked during IBD

```
INVARIANT: mining.start MUST fail during initial block download
```

**Guarantees**:
- ✅ Mining MUST NOT start while chainstate is syncing headers
- ✅ Mining MUST NOT start while blockchain is incomplete
- ✅ Mining MUST NOT start during reindex operations

**Forbidden**:
- ❌ Mining during IBD (wastes resources, no network propagation)
- ❌ Mining during header sync (incorrect chain tip)
- ❌ Mining during reindex (chainstate inconsistent)

**Error Behavior**:
```json
// mining.start called during IBD
{
  "error": {
    "code": -10,
    "message": "Mining disabled during initial block download. Sync progress: 45.2% (blocks: 123456/273891)"
  }
}

// mining.start called during reindex
{
  "error": {
    "code": -10,
    "message": "Mining disabled during blockchain reindex. Wait for reindex to complete."
  }
}
```

**Rationale**: Mining during IBD wastes electricity and produces orphan blocks. Hard block prevents this.

---

### E.2.2 Chainstate Consistency Contract

**MUST**: Mining requires consistent chainstate

```
INVARIANT: mining.start MUST verify chainstate is ready before starting
```

**Guarantees**:
- ✅ Chain database MUST be accessible
- ✅ UTXO set MUST be loaded and consistent
- ✅ Mempool MUST be initialized (for transaction selection)

**Forbidden**:
- ❌ Mining with corrupted chainstate
- ❌ Mining with uninitialized mempool
- ❌ Mining with missing UTXO set

**Error Behavior**:
```json
// mining.start with unavailable chainstate
{
  "error": {
    "code": -10,
    "message": "Chainstate service not available. Verify daemon initialization completed."
  }
}
```

**Rationale**: Mining on inconsistent state produces invalid blocks. Prevent at source.

---

## E.3 — Wallet Safety Contracts

### E.3.1 Wallet Encryption Contract

**MUST**: Mining respects wallet encryption state

```
INVARIANT: mining.start MUST fail if wallet is locked
```

**Guarantees**:
- ✅ Encrypted wallets MUST be unlocked before mining
- ✅ Unlock timeout does not affect mining (mining address is public key)
- ✅ Warning issued if mining to encrypted wallet that might lock soon

**Forbidden**:
- ❌ Mining with locked wallet (user cannot verify address ownership)
- ❌ Silent auto-unlock for mining (security risk)

**Error Behavior**:
```json
// mining.start with locked wallet
{
  "error": {
    "code": -13,
    "message": "Wallet is locked. Unlock wallet to verify mining address ownership: wallet.unlock <passphrase> <timeout>"
  }
}
```

**Rationale**: User should consciously verify they can spend rewards before mining.

---

### E.3.2 Wallet Load State Contract

**MUST**: Mining requires active wallet

```
INVARIANT: mining.start MUST fail if no wallet is loaded
```

**Guarantees**:
- ✅ At least one wallet MUST be loaded
- ✅ Mining uses currently active wallet
- ✅ Switching wallets requires stopping mining first

**Forbidden**:
- ❌ Mining without any wallet loaded
- ❌ Mining to address from inactive wallet
- ❌ Switching wallets while mining is active

**Error Behavior**:
```json
// mining.start with no wallet loaded
{
  "error": {
    "code": -13,
    "message": "No active wallet. Load wallet first: wallet.load <name>"
  }
}

// Attempting to switch wallet while mining
{
  "error": {
    "code": -13,
    "message": "Cannot switch wallet while mining is active. Stop mining first: mining.stop"
  }
}
```

**Rationale**: Prevents ambiguous wallet state and accidental reward loss.

---

## E.4 — RPC Error Hygiene Contracts

### E.4.1 Structured Error Contract

**MUST**: All errors are structured and parsable

```
INVARIANT: RPC errors MUST follow JSON-RPC 2.0 error format
```

**Guarantees**:
- ✅ All errors include `code` (numeric) and `message` (string)
- ✅ Error codes follow Bitcoin Core conventions where applicable
- ✅ Error messages are human-readable and actionable

**Forbidden**:
- ❌ Throwing raw exceptions to RPC client
- ❌ Returning success with error embedded in result
- ❌ Returning null/undefined instead of proper error

**Standard Format**:
```json
{
  "error": {
    "code": -32602,
    "message": "Human-readable error message"
  }
}
```

**Error Code Ranges**:
- `-32700` to `-32600`: JSON-RPC protocol errors
- `-1` to `-99`: General Bitcoin Core compatible errors
- `-100` to `-999`: Dinero-specific errors

---

### E.4.2 No Silent Fallbacks Contract

**MUST**: Ambiguous operations fail loudly

```
INVARIANT: RPCs MUST NOT silently choose defaults for critical parameters
```

**Guarantees**:
- ✅ Missing critical parameters → Error (not default)
- ✅ Ambiguous input → Error (not guessing)
- ✅ Deprecated behavior → Warning then error

**Forbidden**:
- ❌ Silent defaults for mining address
- ❌ Silent defaults for wallet selection
- ❌ Silent fallbacks on encryption failures

**Example - Correct Behavior**:
```python
# WRONG (silent default)
mining.start()  # → Silently uses "first available address" ❌

# RIGHT (explicit requirement)
mining.start()  # → Error: "Missing mining address parameter" ✅
mining.start(threads=4, address="din1...")  # → Success ✅
```

**Rationale**: Explicit > Implicit. Users should know exactly what they're doing.

---

## E.5 — Restart Persistence Contracts

### E.5.1 Persistent State Contract

**MUST**: Define what persists across restarts

```
INVARIANT: Mining configuration persists, mining STATE does not
```

**What MUST Persist** (Proven in Phase D):
- ✅ Mining address (WalletManager SQLite)
- ✅ Wallet UTXO set (WalletManager SQLite)
- ✅ Blockchain UTXO set (UTXOIndex RocksDB)
- ✅ Wallet encryption state

**What MUST NOT Persist**:
- ❌ Mining active/inactive state (requires explicit restart)
- ❌ Mining thread count (reverts to default)
- ❌ Mining session stats (hashrate, uptime)

**Behavior After Restart**:
```json
// mining.info immediately after daemon restart
{
  "mining": false,  // Always false after restart
  "address": "din1...",  // Persisted ✅
  "threads": 0,  // Reset to default
  "hashrate": 0,  // Session stat, reset
  "uptime_seconds": 0  // Session stat, reset
}
```

**Rationale**: Configuration persists (user intent), but runtime state requires conscious restart decision.

---

### E.5.2 Restart Safety Contract

**MUST**: Restarts are safe and predictable

```
INVARIANT: Daemon restart MUST NOT corrupt persistent state
```

**Guarantees**:
- ✅ Unclean shutdown → State recoverable (proven in Phase D.4)
- ✅ Wallet database ACID transactions
- ✅ UTXO set recoverable from RocksDB

**Forbidden**:
- ❌ In-memory state assumed available post-restart
- ❌ Partial writes without transaction boundaries
- ❌ Relying on destructor-based cleanup for critical state

**Rationale**: Users should be able to restart daemon safely without data loss.

---

## E.6 — Policy Enforcement Test Requirements

Each contract MUST have corresponding tripwire tests:

### E.6.1 Tripwire Test Pattern

```cpp
/**
 * Tripwire: Mining during IBD MUST be rejected
 *
 * If this test fails, it means the safety gate was weakened.
 * DO NOT modify this test to make it pass - fix the code instead.
 */
TEST(MiningPolicyTripwires, MiningDuringIBDMustBeRejected) {
    // Setup: Simulate IBD state
    MockChainstateService chainstate;
    chainstate.setIsInitialBlockDownload(true);

    // Execute: Try to start mining
    auto result = rpc_mining_start(chainstate, {});

    // TRIPWIRE: This MUST return an error
    ASSERT_TRUE(result.has("error"))
        << "CRITICAL: Mining was allowed during IBD!";
    EXPECT_EQ(-10, result["error"]["code"]);
}
```

### E.6.2 Required Tripwire Tests

**E.1 - Mining Ownership**:
- ✅ Reject mining with non-owned address
- ✅ Reject mining with locked wallet
- ✅ Reject mining with no wallet loaded

**E.2 - IBD Safety**:
- ✅ Reject mining during IBD
- ✅ Reject mining during reindex
- ✅ Reject mining with unavailable chainstate

**E.3 - Wallet Safety**:
- ✅ Reject mining with locked encrypted wallet
- ✅ Reject wallet switch while mining active

**E.4 - Error Hygiene**:
- ✅ All errors properly structured
- ✅ No silent fallbacks on critical params

**E.5 - Restart Semantics**:
- ✅ Mining does not auto-resume after restart
- ✅ Mining address persists after restart

---

## E.7 — Boundary Ownership

### E.7.1 Layer Responsibilities

**RPC Layer** (src/rpc/methods_*.cpp):
- Validates parameters
- Enforces policy contracts
- Returns structured errors
- **DOES NOT**: Make policy decisions

**Service Layer** (src/daemon/services/*.cpp):
- Executes validated operations
- Maintains runtime state
- **DOES NOT**: Validate user input

**Storage Layer** (WalletManager, UTXOIndex):
- Persists state
- Guarantees consistency
- **DOES NOT**: Enforce business logic

**Policy Enforcement**: Happens at **RPC Layer ONLY**

This prevents:
- Policy scattered across layers
- Inconsistent enforcement
- Difficult auditing

---

## E.8 — Success Criteria

Phase E is complete when:

1. ✅ All contracts written (this document)
2. ✅ All contracts implemented in code
3. ✅ All contracts have tripwire tests
4. ✅ All existing RPCs follow error hygiene rules
5. ✅ Documentation updated with contract behavior

**NOT** when:
- ❌ "Most" contracts implemented
- ❌ Tripwire tests can be disabled
- ❌ Silent fallbacks still exist

---

## E.9 — Future-Proofing

These contracts are **append-only**:
- ✅ Can add new contracts
- ✅ Can strengthen existing contracts
- ❌ **CANNOT** weaken existing contracts without explicit deprecation process

Any PR that weakens a contract triggers:
1. Tripwire test failure
2. Required security review
3. Major version bump (breaking change)

---

## Implementation Order

**Phase E.0**: ✅ Define contracts (this document)
**Phase E.1**: Implement wallet ownership enforcement
**Phase E.2**: Implement IBD/sync safety gates
**Phase E.3**: Enforce restart semantics
**Phase E.4**: Add policy tripwire tests
**Phase E.5**: Enforce error hygiene across RPCs

**Each phase**:
1. Review contracts
2. Implement enforcement
3. Add tripwire tests
4. Verify no existing code violates contract

---

## References

- Phase D.1: Mining RPC correctness (TODOs removed)
- Phase D.2: Mining → Wallet reward flow (proven)
- Phase D.3: RPC ↔ Wallet consistency (persistence enabled)
- Phase D.4: Restart safety (persistence proven)

**Phase E builds on Phase D's correctness proofs by adding safety enforcement.**

---

**Document Status**: Phase E.0 Contract Definition
**Next Step**: Review contracts, then implement E.1 (Wallet Ownership)
**Author**: Generated via Phase E contract-first methodology
