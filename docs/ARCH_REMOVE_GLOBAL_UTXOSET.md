# Architecture Fix: Remove GlobalUTXOSet

**Date:** 2025-12-14
**Status:** Planning Phase
**Severity:** Critical - State Authority Violation

## Problem Statement

GlobalUTXOSet was introduced on 2025-12-04 as a "Bitcoin Core-compatible separation" but actually created a **state bifurcation**:

```
ConnectBlock()
    │
    ├─> ChainDB (undo, height, tip)          ← Authority #1
    └─> GlobalUTXOSet (UTXOs, RocksDB)       ← Authority #2 ❌ SPLIT STATE
```

**Violations:**
- ❌ Two authoritative UTXO stores
- ❌ GlobalUTXOSet writes bypass ChainWriteToken
- ❌ Reorg logic can't see GlobalUTXOSet state
- ❌ Wallet/ChainDB/GlobalUTXOSet can desync

## Bitcoin-Correct Architecture

ChainDB IS the chainstate. Single authority:

```
ConnectBlock()
    │
    └─> ChainDB (undo, height, tip, UTXOs)   ← Single Authority ✅
         │
         └─> Wallets query via getCoin/forEachUTXO
```

**ChainDB already has UTXO support:**
```cpp
// include/storage/chain_db.h
Status putCoin(const ChainWriteToken& token, const uint256& txid, uint32_t vout, const Coin& coin);
Status deleteCoin(const ChainWriteToken& token, const uint256& txid, uint32_t vout);
StatusOr<Coin> getCoin(const uint256& txid, uint32_t vout) const;
Status forEachUTXO(std::function<bool(...)> callback) const;
```

## Where GlobalUTXOSet Exists

### Created
**File:** `src/daemon/services/chainstate_service.cpp`
```cpp
global_utxos = std::make_shared<GlobalUTXOSet>(blockchain_dir / "utxo_global");
```

### Written
**File:** `src/daemon/block_acceptor.cpp`
```cpp
// Line ~1239: ConnectBlock
global_utxos->addUTXO(utxo);

// DisconnectBlock (undo)
global_utxos->deleteUTXO(txid, vout);
```

### Read
**File:** `src/rpc/methods_wallet_context.cpp`
```cpp
// Line ~2441: wallet.rescanblockchain
auto all_utxos = global_utxos->getAllUTXOs(start_height, stop_height);
```

**File:** `src/daemon/mempool.cpp` (likely - needs verification)

### Files to Delete
1. `include/consensus/global_utxo_set.h` (243 lines)
2. `src/consensus/global_utxo_set.cpp` (463 lines)
3. `include/wallet/wallet_utxo_tracker.h` (if GlobalUTXOSet-specific)
4. `src/wallet/wallet_utxo_tracker.cpp` (if GlobalUTXOSet-specific)

## ChainDB Replacement Mapping

| GlobalUTXOSet Call | ChainDB Equivalent |
|-------------------|-------------------|
| `addUTXO(utxo)` | `putCoin(token, txid, vout, coin, wb)` |
| `deleteUTXO(txid, vout)` | `deleteCoin(token, txid, vout, wb)` |
| `getUTXO(txid, vout)` | `getCoin(txid, vout)` |
| `getAllUTXOs(height_range)` | `forEachUTXO(callback)` |

**Note:** ChainDB calls require `ChainWriteToken` for mutations - this is **correct** (ensures reorg safety).

## Removal Plan (Step-by-Step)

### Phase 1: Document Current State ✅
- [x] Identify all GlobalUTXOSet usage
- [x] Map to ChainDB equivalents
- [x] Verify ChainDB UTXO support exists

### Phase 2: Route ConnectBlock/DisconnectBlock to ChainDB
**File:** `src/daemon/block_acceptor.cpp`

**ConnectBlock (add UTXOs):**
```cpp
// OLD (delete):
// global_utxos->addUTXO(utxo);

// NEW:
Coin coin;
coin.amount = tx.vout[vout].value;
coin.scriptPubKey = tx.vout[vout].scriptPubKey;
coin.height = height;
coin.is_coinbase = (tx_idx == 0);

auto status = chaindb_->putCoin(write_token, txid, vout, coin, &batch);
if (!status.ok()) {
    error = "Failed to add UTXO: " + status.message();
    return false;
}
```

**DisconnectBlock (remove UTXOs from undo):**
```cpp
// OLD (delete):
// global_utxos->deleteUTXO(txid, vout);

// NEW:
auto status = chaindb_->deleteCoin(write_token, txid, vout, &batch);
if (!status.ok()) {
    error = "Failed to remove UTXO: " + status.message();
    return false;
}
```

### Phase 3: Route Wallet Rescan to ChainDB
**File:** `src/rpc/methods_wallet_context.cpp`

**wallet.rescanblockchain:**
```cpp
// OLD (delete):
// auto all_utxos = global_utxos->getAllUTXOs(start_height, stop_height);

// NEW:
std::vector<WalletUTXO> matching_utxos;
auto status = chaindb->forEachUTXO([&](const uint256& txid, uint32_t vout, const Coin& coin) {
    // Filter by height range
    if (coin.height < start_height || coin.height > stop_height) {
        return true; // continue
    }

    // Convert scriptPubKey to hex for matching
    std::string script_hex = bytes_to_hex(coin.scriptPubKey);

    // Check if wallet owns this scriptPubKey
    auto it = scriptpubkey_to_address.find(script_hex);
    if (it != scriptpubkey_to_address.end()) {
        // Add to wallet
        wallet.addUTXO(txid_hex, vout, coin.amount, it->second, script_hex, coin.height, coin.is_coinbase);
        utxos_added++;
    }

    return true; // continue
});
```

### Phase 4: Delete GlobalUTXOSet Infrastructure
1. Remove GlobalUTXOSet creation in `chainstate_service.cpp`
2. Delete `include/consensus/global_utxo_set.h`
3. Delete `src/consensus/global_utxo_set.cpp`
4. Remove from CMakeLists.txt
5. Remove any config paths referencing `utxo_global/`

### Phase 5: Verification
**Success Criteria (all must pass):**
1. ✅ Build succeeds
2. ✅ Genesis block loads
3. ✅ Can mine 1 block
4. ✅ Can mine 110 blocks (coinbase maturity)
5. ✅ Wallet shows correct balance after rescan
6. ✅ **100-block deep reorg succeeds** ← Cryptographic checksum
7. ✅ No references to GlobalUTXOSet in codebase

## Invariants That Must Hold

### Before Removal (Broken State)
```
ConnectBlock writes:
  - ChainDB: undo data, height, tip
  - GlobalUTXOSet: UTXOs

Wallet reads:
  - GlobalUTXOSet: UTXOs

Reorg sees:
  - ChainDB: undo data ✅
  - GlobalUTXOSet: ❌ (separate database, not reorg-aware)
```

### After Removal (Correct State)
```
ConnectBlock writes:
  - ChainDB: undo data, height, tip, UTXOs (atomic with ChainWriteToken)

Wallet reads:
  - ChainDB: UTXOs via forEachUTXO()

Reorg sees:
  - ChainDB: undo data + UTXOs ✅ (single atomic state)
```

## Risk Assessment

**Low Risk** because:
- ✅ ChainDB UTXO support already exists and is tested
- ✅ ChainWriteToken already enforces single-writer
- ✅ Reorg tests already validate ChainDB correctness
- ✅ This is **subtractive** (deleting duplicate state)
- ✅ Net deletion of ~1500 lines of duplicated logic

**High Risk if NOT done:**
- ❌ State split will cause silent desyncs
- ❌ Wallet bugs will multiply as symptoms
- ❌ Reorg safety is compromised
- ❌ Future protocol changes require dual-maintenance

## Philosophical Anchor

> "UTXOs live in ChainDB; wallets only recognize a subset of them."

This is Bitcoin-correct. ChainDB = chainstate. Wallets are observers, not owners.

## Next Steps

1. Review this plan
2. Execute Phase 2 (ConnectBlock/DisconnectBlock)
3. Execute Phase 3 (Wallet rescan)
4. Execute Phase 4 (Delete GlobalUTXOSet)
5. Execute Phase 5 (Verification including deep reorg)

**No temporary bridges. No gradual migration. Clean removal.**
