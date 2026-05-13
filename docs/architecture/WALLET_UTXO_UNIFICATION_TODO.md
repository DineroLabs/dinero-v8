# Wallet UTXO Type Unification (Post Phase M.1)

**Status**: Deferred until after Phase M.1 completion
**Priority**: Medium (does not block consensus/mempool work)
**Created**: 2025-12-26

## Problem Statement

Multiple wallet-layer UTXO representations exist with incompatible field names:

### 1. `dinero::WalletUTXO` (utxo_index.h - Database layer)
```cpp
struct WalletUTXO {
    uint256 txid;
    uint32_t vout;
    int64_t value;
    std::vector<uint8_t> spk;           // ← Field name
    std::string path;                    // ← HD wallet derivation
    int height;
    std::optional<int> spend_height;
    bool is_coinbase;
};
```

### 2. `HDWalletUTXO` (hd_wallet.h - HD wallet layer)
```cpp
struct HDWalletUTXO {
    dinero::uint256 txid;
    uint32_t vout;
    uint64_t value;
    std::string address;                 // ← Different field
    uint32_t confirmations;              // ← Different field
    bool is_coinbase;
    std::vector<uint8_t> scriptPubKey;   // ← Different field name
};
```

### 3. `WalletManager::WalletUTXO` (wallet_manager.h - RPC layer)
```cpp
struct WalletUTXO {
    std::string txid;                    // ← std::string not uint256
    uint32_t vout;
    uint64_t amount_una;
    double amount_din;
    std::string address;
    int confirmations;
    uint32_t height;
    bool spendable;
    bool is_coinbase;
    bool is_mature;
    std::string label;
    std::string script_pubkey;           // ← std::string not vector<uint8_t>
    bool is_spent;
};
```

## Impact

**Current state**:
- ✅ Consensus layer: Clean, uses `consensus::UTXOEntry`
- ✅ Mempool layer: Uses `ChainStateView` abstraction
- ⚠️ Wallet layer: Three incompatible UTXO types
- ❌ Wallet binary: Temporarily unbuildable

**What still builds**:
- ✅ Consensus (libdinero_consensus.a)
- ✅ Mempool validation
- ✅ Mining (dinero-miner)
- ✅ Reindex operations
- ❌ dinerod (requires wallet)
- ❌ Wallet RPC tests

## Why This Is Deferred

**Phase M.1 scope**:
- Mempool foundation (ChainStateView abstraction) ✅
- Mempool refactor (CoinsViewMemPool) ← Stage 2
- WalletUTXO uint256 migration ← Stage 3 (wallet-specific)
- RPC boundary conversions ← Stage 4 (wallet-specific)

**Rationale**:
1. Mempool does NOT depend on wallet
2. Consensus does NOT depend on wallet
3. Mining safety already proven
4. Reindex already validated
5. Fixing this now would blur wallet ↔ consensus boundaries
6. Phase M.1 can complete without wallet binary

## Proposed Solution (Post M.1)

### Step 1: Canonical Type
Define ONE wallet UTXO type in `include/wallet/wallet_utxo.h`:

```cpp
namespace dinero {
namespace wallet {

struct WalletUTXO {
    uint256 txid;                        // Phase M.0: uint256 identity
    uint32_t vout;
    uint64_t value;                      // una (consensus unit)
    std::vector<uint8_t> scriptPubKey;   // Consensus field name

    // Wallet-specific metadata
    std::string derivation_path;         // "m/84'/1447'/0'/0/12"
    uint32_t height;                     // Block height
    std::optional<uint32_t> spend_height; // Spending height (nullopt = unspent)
    bool is_coinbase;

    // Computed fields (not stored)
    uint32_t confirmations;              // current_height - height + 1
    std::string address;                 // Derived from scriptPubKey
    bool is_mature;                      // coinbase maturity check
};

} // namespace wallet
} // namespace dinero
```

### Step 2: Eliminate Duplicates
- Delete `HDWalletUTXO` (hd_wallet.h)
- Delete `WalletManager::WalletUTXO` (wallet_manager.h)
- Update all wallet code to use canonical `wallet::WalletUTXO`

### Step 3: RPC Boundary
RPC layer converts to JSON, never creates intermediate UTXO structs:
```cpp
// RPC method
Json::Value listunspent() {
    auto utxos = wallet->ListUnspent();  // Returns vector<wallet::WalletUTXO>

    Json::Value result(Json::arrayValue);
    for (const auto& utxo : utxos) {
        Json::Value item;
        item["txid"] = utxo.txid.GetHex();  // ← uint256 → string at boundary
        item["vout"] = utxo.vout;
        item["amount"] = utxo.value / 1e8;
        item["address"] = utxo.address;
        item["confirmations"] = utxo.confirmations;
        result.append(item);
    }
    return result;
}
```

## Action Items (Post M.1)

- [ ] Create canonical `wallet::WalletUTXO` in new header
- [ ] Migrate `UTXOIndex` to use canonical type
- [ ] Migrate `HDWallet` to use canonical type
- [ ] Migrate `WalletManager` to use canonical type
- [ ] Update `TransactionBuilder` signatures
- [ ] Update `BIP143Signer` signatures
- [ ] Update `TaprootTxSigner` signatures
- [ ] Update `CoinSelector` signatures
- [ ] Remove conversion glue
- [ ] Verify wallet builds
- [ ] Verify wallet RPC tests pass

## DO NOT

❌ Do NOT add conversion functions between types
❌ Do NOT create `ToHDWalletUTXO()` helpers
❌ Do NOT spread conversion logic across files
❌ Do NOT rush this to "unblock" the build

The wallet being temporarily unbuildable is **intentional** and **acceptable** during Phase M.1.

## Related Work

- Phase M.0: UTXO eradication (consensus layer) ✅ Complete
- Phase M.1: Mempool foundation ← In progress
- Phase M.2: Batch UTXO APIs (performance)
- Phase M.3: Wallet UTXO unification ← This document

---

**Last Updated**: 2025-12-26
**Owner**: Phase M implementation team
**Blocks**: Nothing (wallet orthogonal to Phase M.1)
