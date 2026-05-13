# Blockchain Reorg Implementation Plan

## Status Overview

### Completed ✅

1. **Data Structures** (include/consensus/undo.h)
   - `SpentCoin`: Records spent UTXO info (txid, vout, value, scriptPubKey, is_coinbase, height)
   - `CreatedOut`: Records created outputs (txid, vout)
   - `UndoRecord`: Container with vectors of spent/created coins

2. **Serialization** (src/consensus/undo.cpp)
   - `UndoRecord::Serialize()`: Binary format for RocksDB storage
   - `UndoRecord::Deserialize()`: Reads back from RocksDB

3. **Schema Cleanup**
   - Removed incorrect SQLite block_undo table
   - Confirmed RocksDB-only approach for undo storage

4. **BuildUndoForBlock() Function** (src/daemon/block_acceptor.cpp:622-691)
   - ✅ Captures FULL spent UTXO data (value, script, height, coinbase flag)
   - ✅ Tracks created outputs
   - ✅ Queries ChainDB UTXO set for each spent input

5. **ConnectBlock() UTXO Updates** (src/daemon/block_acceptor.cpp:754-811)
   - ✅ Deletes spent UTXOs from database
   - ✅ Adds created UTXOs to database
   - ✅ Validates all inputs exist (double-spend protection)
   - ✅ Uses RocksDB WriteBatch for atomic updates

6. **ConnectBlock() Undo Storage** (src/daemon/block_acceptor.cpp:734-752)
   - ✅ Builds undo record BEFORE modifying UTXO set
   - ✅ Stores undo records atomically with UTXO changes
   - ✅ Key pattern: "U:<blockhash>"

7. **ApplyTipInvalidation() Function** (src/daemon/block_acceptor.cpp:1084-1260)
   - ✅ Loads undo data from RocksDB
   - ✅ DELETES all outputs created by disconnected block
   - ✅ RESTORES all outputs spent by disconnected block
   - ✅ Updates chain tip atomically
   - ✅ Full UTXO restoration working

7. **invalidateblock RPC Handler** (src/daemon/main.cpp:1754-1836)
   - Registered in RPC server
   - Regtest-only safety check
   - Validates parameters and current tip
   - Returns detailed success/error information

### Remaining Work ⏳

## 1. Comprehensive Testing

**Priority Test Scenarios**:

### Test 1: Basic Tip Invalidation
```bash
#!/bin/bash
# test_tip_invalidation.sh

DATADIR="/tmp/reorg-test"
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

# Start daemon
./build/dinerod --regtest --datadir="$DATADIR" &
DPID=$!
sleep 3

COOKIE=$(cat "$DATADIR/.cookie")
RPC="curl -s -X POST http://127.0.0.1:20998 -u \"$COOKIE\" -H \"Content-Type: application/json\""

# Generate 5 blocks
for i in {1..5}; do
    eval "$RPC -d '{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"generatetoaddress\",\"params\":[1,\"rdin1q5jlf85f9ntwd8mzvej93jgfxfydyx59uxm2rjz\"]}'"
    sleep 1
done

# Get current height
HEIGHT=$(eval "$RPC -d '{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getblockchaininfo\"}'" | jq -r '.result.blocks')
echo "Height before invalidate: $HEIGHT"

# Get tip hash
TIP=$(eval "$RPC -d '{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getbestblockhash\"}'" | jq -r '.result')
echo "Tip hash: ${TIP:0:16}..."

# Invalidate tip
eval "$RPC -d '{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"invalidateblock\",\"params\":[\"'$TIP'\"]}'"

# Verify height decreased
NEW_HEIGHT=$(eval "$RPC -d '{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"getblockchaininfo\"}'" | jq -r '.result.blocks')
echo "Height after invalidate: $NEW_HEIGHT"

if [ "$NEW_HEIGHT" -eq $((HEIGHT - 1)) ]; then
    echo "✅ SUCCESS: Height decreased from $HEIGHT to $NEW_HEIGHT"
else
    echo "❌ FAIL: Expected height $((HEIGHT - 1)), got $NEW_HEIGHT"
fi

kill $DPID
```

### Test 2: Idempotent Calls
Verify that calling `invalidateblock` twice on same hash is safe

### Test 3: Persistence
Verify that undo data persists across daemon restarts

## 2. Wallet Reorg Notifications (Future Enhancement)

**Optional Wallet Integration**:

1. **WalletNotify::OnReorg()** - Notify wallet of disconnected blocks
   - Mark transactions in disconnected blocks as unconfirmed
   - Trigger rescan of affected address ranges
   - Update balances based on UTXO changes

## Next Steps

1. ✅ ~~Implement `BuildUndoForBlock()`~~ - **DONE** (full UTXO capture)
2. ✅ ~~Modify `ConnectBlock()` to update UTXO set~~ - **DONE**
3. ✅ ~~Store undo records atomically~~ - **DONE**
4. ✅ ~~Implement `ApplyTipInvalidation()`~~ - **DONE** (full UTXO restoration)
5. ✅ ~~Create `invalidateblock` RPC handler~~ - **DONE**
6. ⏳ **Write comprehensive tests** - **READY FOR TESTING**
7. ⏳ Wire up wallet notifications for reorgs - **OPTIONAL**

## Key Design Principles

- **Atomicity**: All undo operations use RocksDB WriteBatch
- **Regtest-only**: `invalidateblock` is restricted to regtest mode
- **Tip-only**: Only allow invalidating the current chain tip
- **Safety First**: Comprehensive error handling and logging
- **Testability**: All operations are testable and reversible

## References

- Undo structures: `include/consensus/undo.h`
- Serialization: `src/consensus/undo.cpp`
- Block connection: `src/daemon/block_acceptor.cpp:622`
- RocksDB storage: Key pattern "U:<blockhash>"
