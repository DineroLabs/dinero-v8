# Wallet Architecture Documentation

**Version**: v0.12.0 (Wallet Feature Complete)
**Status**: FROZEN INTERFACES (implementations may evolve)
**Last Updated**: 2025-12-14

---

## Design Philosophy

> **The wallet is a client of the blockchain and mempool — never their owner.**

### Core Principles

1. **Separation of Concerns**
   - Wallet: Keys, coins, user intent
   - Mempool: Policy, relay, mining priority
   - ChainDB: Consensus, block validation

2. **Dependency Direction**
   ```
   Wallet → IMempoolInterface → Mempool
   Wallet → ChainDB (read-only queries)
   Mempool ✗→ Wallet (NO REVERSE DEPENDENCY)
   ```

3. **Offline Capability**
   - Wallet can build unsigned transactions offline
   - Signing can happen in isolated environment
   - Only submission requires network/mempool

---

## Architecture Layers

### Layer 1: HD Wallet Foundation (FROZEN)

**Status**: ✅ Already implemented

**Files**:
- `include/wallet/hd_wallet.h`
- `src/wallet/hd_wallet.cpp`
- `include/crypto/hd_keychain.h`

**Responsibilities**:
- BIP32 master seed handling
- Deterministic key derivation
- Hardened/non-hardened paths
- Gap limit management (default: 20)

**Interface** (frozen):
```cpp
class HDWallet {
public:
    // Restore from mnemonic
    static std::unique_ptr<HDWallet> FromMnemonic(
        const std::string& mnemonic,
        const std::string& passphrase = ""
    );

    // Derive key at path
    std::string deriveAddress(uint32_t account, uint32_t change, uint32_t index);

    // Get next unused address
    std::string getNewAddress(uint32_t account = 0);
};
```

**Guarantees**:
- Same mnemonic → same addresses (deterministic)
- Gap limit prevents address reuse
- Watch-only mode supported (xpub only)

---

### Layer 2: UTXO Model (v0.12.0 - NEW)

**Status**: 🚧 Needs formalization

**Purpose**: Wallet-centric UTXO state independent of ChainDB internals

**UTXO States**:
```cpp
enum class WalletUTXOState {
    CONFIRMED,          // >= 1 confirmation
    UNCONFIRMED,        // In mempool (0-conf)
    SPENT_LOCAL,        // Spent by our own tx (RBF/CPFP tracking)
    CONFLICTED,         // Double-spent / replaced
    LOCKED              // User-locked (e.g., for CoinJoin)
};
```

**Tracked Fields**:
```cpp
struct WalletUTXO {
    OutPoint outpoint;              // txid:vout
    uint64_t amount;                // una
    std::string address;            // Receiving address
    std::string script_pubkey;      // Output script
    uint32_t confirmations;         // Depth in chain
    WalletUTXOState state;          // Current state
    uint32_t ancestor_count;        // Cached from mempool (if unconfirmed)
    bool is_coinbase;               // Maturity rules
};
```

**Data Sources**:
- ChainDB: Confirmed UTXOs
- Mempool: Unconfirmed UTXOs (via IMempoolInterface)
- Wallet DB: Cached state + metadata

**Exit Criteria**:
- ✅ Can list available coins without mempool queries
- ✅ Tracks spent-but-unconfirmed state (RBF tracking)
- ✅ Proper coinbase maturity handling (100 blocks)

---

### Layer 3: Coin Selection Engine (v0.12.0 - ENHANCE)

**Status**: ⚠️ Exists but needs BnB algorithm

**Current Implementation**: Greedy algorithm only
**Files**: `include/wallet/coin_selection.h`

**Algorithms Required**:

#### 3.1 Branch-and-Bound (BnB) - NEW
```cpp
CoinSelectionResult SelectCoinsBnB(
    const std::vector<WalletUTXO>& utxos,
    uint64_t target_amount,
    uint64_t fee_rate
);
```
- **Goal**: Exact match (no change output)
- **Benefit**: Privacy + lower fees
- **Fallback**: Use greedy if no exact match

#### 3.2 Greedy (Existing)
```cpp
CoinSelectionResult SelectCoinsGreedy(
    const std::vector<WalletUTXO>& utxos,
    uint64_t target_amount,
    uint64_t fee_rate
);
```
- **Strategy**: Largest coins first
- **Use case**: BnB fallback, large payments

#### 3.3 Privacy-Aware Selection - NEW
```cpp
CoinSelectionResult SelectCoinsPrivacy(
    const std::vector<WalletUTXO>& utxos,
    uint64_t target_amount,
    uint64_t fee_rate
);
```
- **Avoid**: Address reuse
- **Avoid**: Common-input-ownership heuristic
- **Prefer**: Mixed-age UTXOs

**Selection Rules**:
1. Confirmed-only by default
2. Skip coinbase if immature (<100 confirmations)
3. Respect user-locked coins
4. Check ancestor limits (if unconfirmed parent)

**Exit Criteria**:
- ✅ BnB algorithm implemented
- ✅ Deterministic selection (under fixed seed)
- ✅ No mempool mutation during selection
- ✅ No signing during selection

---

### Layer 4: Transaction Construction (v0.12.0 - FORMALIZE)

**Status**: ⚠️ Exists but needs unsigned build separation

**Current**: `TransactionBuilder` mixes preview + signing

**Required Separation**:

#### 4.1 Unsigned Build
```cpp
struct UnsignedTxResult {
    Transaction tx;                     // Unsigned transaction
    std::vector<WalletUTXO> inputs;     // Selected coins
    uint64_t change_amount;             // Change output amount
    std::string change_address;         // Change address
    uint64_t fee;                       // Total fee
    bool signals_rbf;                   // nSequence < 0xfffffffe
};

UnsignedTxResult buildUnsignedTransaction(
    const std::vector<Recipient>& recipients,
    const BuildOptions& options
);
```

**Responsibilities**:
1. Select coins (via coin selection engine)
2. Create outputs (recipients + change)
3. Set RBF intent (user-controlled)
4. Estimate size and fee
5. Produce **unsigned** transaction

**Does NOT**:
- Sign inputs
- Validate policy (mempool's job)
- Broadcast (wallet service's job)

#### 4.2 Signing (Separate Step)
```cpp
Transaction signTransaction(
    const Transaction& unsigned_tx,
    const std::map<std::string, PrivateKey>& keys
);
```

**Enables**:
- Hardware wallet signing (offline)
- Multisig workflows
- Air-gapped signing

**Exit Criteria**:
- ✅ Can build unsigned tx without keys
- ✅ Size estimation accurate (±5 bytes)
- ✅ RBF signaling correct
- ✅ Change output created if needed

---

### Layer 5: Wallet ↔ Mempool Boundary (v0.12.0 - CRITICAL)

**Status**: 🚧 NEW - This is the FROZEN CONTRACT

**Interface**: `include/wallet/mempool_interface.h`

**Workflow**:

```cpp
// 1. Wallet builds unsigned tx
auto unsigned_result = wallet->buildUnsignedTransaction(recipients, options);

// 2. Wallet tests policy BEFORE signing
TxPolicyResult policy = mempool->testAcceptTransaction(unsigned_result.tx);
if (!policy.would_accept) {
    return Error("Policy rejection: " + policy.rejection_reason);
}

// 3. Wallet signs tx
Transaction signed_tx = wallet->signTransaction(unsigned_result.tx, keys);

// 4. Wallet submits to mempool
SubmitResult result = mempool->submitTransaction(signed_tx, SubmitMode::BROADCAST);

// 5. Wallet updates UTXO state
if (result.status == SubmitResult::Status::ACCEPTED) {
    wallet->markUTXOsSpent(unsigned_result.inputs);
} else if (result.status == SubmitResult::Status::REPLACED) {
    wallet->handleRBFReplacement(result.replaced_txids, signed_tx.GetTxid());
}
```

**Key Guarantees**:
- ✅ Wallet never mutates mempool internals
- ✅ Mempool never touches wallet keys
- ✅ Policy validation happens at boundary
- ✅ Clean error reporting

**Exit Criteria**:
- ✅ Interface header frozen
- ✅ Wallet can react to RBF/rejection
- ✅ CPFP opportunities detected
- ✅ No wallet logic inside mempool

---

### Layer 6: Transaction Batching (v0.12.0 - NEW)

**Status**: 🚧 To be implemented

**Purpose**: Reduce fees, improve UX

**API**:
```cpp
struct BatchRecipient {
    std::string address;
    uint64_t amount;
};

UnsignedTxResult buildBatchTransaction(
    const std::vector<BatchRecipient>& recipients,
    const BuildOptions& options
);
```

**Optimization**:
- Shared inputs across all recipients
- Single change output
- Fee paid once

**Example**:
```
Non-batched: 3 payments = 3 txs × 200 bytes = 600 bytes
Batched:     3 payments = 1 tx × 350 bytes  = 350 bytes
Savings:     42% fee reduction
```

**Exit Criteria**:
- ✅ Batch tx size < N × single tx size
- ✅ Fee savings measurable
- ✅ All recipients in single transaction

---

## Data Flow Diagram

```
┌─────────────────┐
│   HD Wallet     │ (BIP32 keys)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  UTXO Tracker   │ (Wallet state)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Coin Selection  │ (BnB, Greedy, Privacy)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Tx Builder      │ (Unsigned tx)
│  (Unsigned)     │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Policy Test     │───────► IMempoolInterface::testAcceptTransaction()
│                 │         (Dry-run validation)
└────────┬────────┘
         │ (if accepted)
         ▼
┌─────────────────┐
│  Signing        │ (Private keys)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Submit to       │───────► IMempoolInterface::submitTransaction()
│ Mempool         │         (BROADCAST mode)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Update Wallet   │ (Mark UTXOs spent)
│ UTXO State      │
└─────────────────┘
```

---

## Configuration Defaults

```cpp
// Coin Selection
const size_t GAP_LIMIT = 20;                // BIP44 gap limit
const uint64_t DUST_THRESHOLD = 546;        // P2WPKH dust (sats)
const double DEFAULT_FEE_RATE = 1.0;        // sat/byte

// Coinbase Maturity
const uint32_t COINBASE_MATURITY = 100;     // blocks

// Transaction Limits
const size_t MAX_TX_SIZE = 100000;          // 100KB
const size_t MAX_STANDARD_TX_SIZE = 400000; // 400KB
```

---

## Testing Strategy

### Unit Tests
- ✅ HD key derivation (BIP32 test vectors)
- ✅ Coin selection algorithms
- ✅ Transaction size estimation
- ✅ RBF signaling logic

### Integration Tests
- ✅ Build + sign + submit workflow
- ✅ RBF replacement handling
- ✅ CPFP detection
- ✅ Batch transactions

### Regtest Tests
- ✅ End-to-end payment flow
- ✅ Mempool policy rejection handling
- ✅ Change output creation
- ✅ Fee calculation accuracy

---

## Maintenance Policy

**FROZEN INTERFACES**:
- `IMempoolInterface` - Do not modify without v0.13.0+ bump
- `HDWallet` public API - Already stable
- `WalletUTXO` struct - Extend, don't break

**ALLOWED CHANGES**:
- Internal coin selection algorithms (improve BnB)
- Fee estimation hints (future module)
- Privacy heuristics
- Performance optimizations

**FORBIDDEN**:
- Wallet accessing mempool internals
- Mempool touching wallet keys
- Breaking `IMempoolInterface` contract

---

## References

- [BIP32: Hierarchical Deterministic Wallets](https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki)
- [BIP44: Multi-Account Hierarchy](https://github.com/bitcoin/bips/blob/master/bip-0044.mediawiki)
- [Bitcoin Core Coin Selection](https://github.com/bitcoin/bitcoin/blob/master/src/wallet/coinselection.cpp)
- [BIP125: Replace-By-Fee](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki)

---

**Status**: Wallet architecture defined. Interface frozen. Ready for v0.12.0 implementation.
