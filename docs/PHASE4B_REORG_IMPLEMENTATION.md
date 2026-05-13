# Phase 4B: Blockchain Reorg Implementation

**Status**: ✅ COMPLETE
**Completion Date**: November 11, 2025
**Commits**: `e95eda25e`, `f91ea2e32`

---

## Overview

Phase 4B implements complete blockchain reorganization (reorg) support for DineroCoin, including automatic wallet UTXO rollback during chain reorganizations. This ensures wallet balances remain accurate when the blockchain switches to a different chain with more cumulative work.

## Architecture

### Event Pipeline

```
User RPC Command (invalidateblock)
         ↓
BlockAcceptor::ApplyTipInvalidation()
    - Loads undo record from RocksDB
    - Reverses UTXO changes atomically
    - Updates chain tip to parent block
    - Loads full block from database
         ↓
ChainstateService::notifyBlockDisconnected(block, height)
         ↓
WalletManager::onBlockDisconnected(block, height)
    - Deletes UTXOs where height == disconnected_height
    - Scans block inputs to restore spent UTXOs
    - Updates wallet blockchain height
         ↓
Wallet Balance Updated
```

## Components

### 1. BlockAcceptor::DisconnectBlock()

**Location**: `src/daemon/block_acceptor.cpp:1194-1392`
**Purpose**: Reverses blockchain state changes for a disconnected block

**Algorithm**:
1. Load undo record from RocksDB (key: `"U:<blockhash>"`)
2. Deserialize UndoRecord (contains spent/created UTXOs)
3. Restore spent UTXOs from `undo.spent` vector
4. Delete created UTXOs from `undo.created` vector
5. Update chain tip to parent block
6. Delete undo record
7. Trigger wallet notifications

**Key Features**:
- Atomic RocksDB WriteBatch operations
- Proper error handling and logging
- Calculates parent chainwork from bits
- Event-driven wallet notifications

### 2. WalletManager::onBlockDisconnected()

**Location**: `src/wallet/wallet_manager.cpp:3151-3227`
**Purpose**: Rolls back wallet UTXO state during reorg

**Algorithm**:
1. Delete all UTXOs where `height = disconnected_height`
2. Scan block transactions to find spent inputs
3. Mark spent UTXOs as unspent (restore them)
4. Update wallet blockchain height to `height - 1`

**SQL Operations**:
```sql
-- Delete created UTXOs
DELETE FROM utxos
WHERE wallet_id = ? AND height = ?

-- Restore spent UTXOs
UPDATE utxos
SET is_spent = 0
WHERE wallet_id = ? AND txid = ? AND vout = ? AND is_spent = 1
```

### 3. WalletManager::removeUTXO()

**Location**: `src/wallet/wallet_manager.cpp:2661-2683`
**Purpose**: Helper method to delete specific UTXO from database

**Signature**:
```cpp
bool WalletManager::removeUTXO(const std::string& txid, int vout);
```

### 4. Enhanced ApplyTipInvalidation()

**Location**: `src/daemon/block_acceptor.cpp:1913-1948`
**Purpose**: Integrates DisconnectBlock() with invalidateblock RPC

**Changes**:
- Now loads full block from RocksDB
- Calls `ChainstateService::notifyBlockDisconnected()`
- Triggers proper wallet UTXO rollback
- Graceful fallback if block not found

## Data Structures

### UndoRecord

**Location**: `include/consensus/undo.h`

```cpp
struct UndoRecord {
    std::vector<SpentCoin> spent;     // UTXOs spent by this block
    std::vector<CreatedOut> created;  // UTXOs created by this block
};

struct SpentCoin {
    std::string prev_txid;
    uint32_t prev_vout;
    uint64_t value;              // Amount in una
    std::string scriptPubKey;
    bool is_coinbase;
    uint32_t height;
};

struct CreatedOut {
    std::string txid;
    uint32_t vout;
};
```

## Testing

### Manual Testing

```bash
# 1. Start daemon in regtest mode
./build/bin/dinerod --regtest --datadir=/tmp/reorg-test

# 2. Create wallet and mine blocks
./build/bin/dinero-cli --regtest wallet.create "test"
ADDR=$(./build/bin/dinero-cli --regtest wallet.getnewaddress)
./build/bin/dinero-cli --regtest generatetoaddress 105 "$ADDR"

# 3. Check initial balance
./build/bin/dinero-cli --regtest wallet.getbalance

# 4. Trigger reorg by invalidating block 100
BLOCK=$(./build/bin/dinero-cli --regtest getblockhash 100)
./build/bin/dinero-cli --regtest invalidateblock "$BLOCK"

# 5. Verify wallet balance updated correctly
./build/bin/dinero-cli --regtest wallet.getbalance
# Expected: Balance reduced by 6 block rewards (blocks 100-105)
```

### Automated Testing

**Test Suite**: `tests/regression/test_phase4b_reorg.cpp`

**Test Coverage**:
- ✅ Single block reorg
- ✅ Multi-block reorg (6 blocks)
- ✅ Deep reorg (100+ blocks)
- ✅ Wallet balance accuracy
- ✅ UTXO count verification
- ✅ Coinbase maturity handling
- ✅ Wallet notification pipeline
- ✅ Atomic rollback verification
- ✅ Error handling

## Performance

### Benchmarks

| Reorg Depth | Time (expected) | Memory Usage |
|-------------|-----------------|--------------|
| 1 block     | < 10ms          | Minimal      |
| 10 blocks   | < 100ms         | Low          |
| 100 blocks  | < 1s            | Moderate     |
| 1000 blocks | < 10s           | High         |

### Optimization Notes

- All database operations use RocksDB WriteBatch (atomic)
- UTXO rollback is SQL-optimized (single DELETE, bulk UPDATE)
- Undo records are deleted after use (space efficient)
- Block headers kept for historical queries

## Security Considerations

### Attack Vectors Addressed

1. **Double-spend during reorg**: ✅ UTXOs properly rolled back
2. **Balance inconsistency**: ✅ Wallet notifications ensure accuracy
3. **Partial rollback**: ✅ Atomic WriteBatch prevents partial states
4. **Concurrent reorgs**: ✅ Single-threaded block processing

### Edge Cases Handled

- Missing undo record: Fails gracefully with error
- Block not in database: Fallback to basic height update
- Wallet service unavailable: Skips notifications (headless mode)
- Orphaned transactions: Properly handled via input scanning

## Limitations

### Known Limitations

1. **No RBF support**: Replace-by-fee transactions not yet supported
2. **No CPFP support**: Child-pays-for-parent not implemented
3. **Deep reorg performance**: 1000+ block reorgs may be slow
4. **Memory usage**: Large undo records consume memory

### Future Improvements

- [ ] Optimize deep reorg performance with parallel processing
- [ ] Add RBF support for better fee management
- [ ] Implement CPFP for stuck transactions
- [ ] Compress undo records for space efficiency
- [ ] Add reorg metrics to monitoring

## API Changes

### New RPC Methods

None (uses existing `invalidateblock` RPC)

### Modified Behavior

**`invalidateblock <blockhash>`**
- **Before**: Invalidated block, updated height only
- **After**: Full wallet UTXO rollback, proper balance updates

## Database Schema Changes

### No schema changes required

Reorg implementation uses existing tables:
- `utxos` table (wallet UTXOs)
- RocksDB undo records (key: `"U:<blockhash>"`)
- Block headers (kept for historical queries)

## Deployment Notes

### Rolling Out Phase 4B

1. **Build**: Compile with Phase 4B changes
2. **Test**: Run regression tests
3. **Deploy**: Update daemon binary
4. **Monitor**: Watch for reorg events in logs

### Monitoring

**Log Messages to Watch**:
```
🔄 Disconnecting block at height X
✅ Block disconnected atomically at height X
✅ Wallet disconnect notifications dispatched for block X
WalletManager: 🔄 Processing block disconnect at height X
WalletManager: ✅ Block X disconnected - removed Y UTXOs, restored Z UTXOs
```

### Rollback Plan

If issues detected:
1. Stop daemon
2. Restore from backup
3. Revert to previous binary
4. Investigate logs

## Changelog

### Version 0.1.0 (Phase 4B)

**Added**:
- BlockAcceptor::DisconnectBlock() method
- WalletManager::onBlockDisconnected() event handler
- WalletManager::removeUTXO() helper method
- Proper wallet notifications in ApplyTipInvalidation()

**Changed**:
- ApplyTipInvalidation() now triggers wallet notifications
- Wallet balances updated automatically during reorgs

**Fixed**:
- Wallet balance inconsistency after reorgs
- UTXO set corruption during chain switches

## References

- Bitcoin Core reorg handling: https://github.com/bitcoin/bitcoin/blob/master/src/validation.cpp
- UndoRecord specification: `include/consensus/undo.h`
- WalletNotifier interface: `include/interfaces/wallet_notifier.h`

## Contributors

- Phase 4B: Claude Code (November 2025)
- Architecture Review: Architecture V3 (Phase 3)
- Testing: Regression test suite

---

**Document Version**: 1.0
**Last Updated**: November 11, 2025
**Status**: Complete ✅
