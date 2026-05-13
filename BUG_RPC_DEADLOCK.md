# 🐛 CRITICAL: RPC Server Deadlock During Mining

**Date:** October 6, 2025  
**Priority:** 🔴 CRITICAL - Blocks Mainnet Launch  
**Status:** 🚨 REPRODUCIBLE

---

## Problem

**RPC server deadlocks after processing mined blocks, making daemon completely unresponsive.**

### Symptoms:
- ✅ Miner finds blocks successfully
- ✅ First block processes and broadcasts
- ❌ **Daemon RPC stops responding** after 1-2 blocks
- ❌ Miner hash rate drops to 0.00 MH/s (stuck waiting for `getblocktemplate`)
- ❌ GUI stops updating
- ❌ All RPC calls timeout
- ⚠️ Daemon process still running but frozen

---

## Reproduction Steps

1. Start daemon with seed nodes:
   ```bash
   ./build/dinerod -datadir=./data -testnet -rpcport=20998 \
     -addnode=172.93.160.131:20999 \
     -addnode=96.9.226.98:20999 \
     -addnode=173.249.195.59:20999
   ```

2. Start miner:
   ```bash
   ./build/dinero-miner -a <address> -t 8
   ```

3. **RESULT:** After 1-2 blocks found, daemon freezes

---

## Evidence

### Miner Output:
```
✅ Connected to daemon, height: 1
🎉 BLOCK FOUND! Nonce: 27848585
✅ Block submitted successfully!
✅ Connected to daemon, height: 2
🎉 BLOCK FOUND! Nonce: 14657826
⛏️  4.31 MH/s | Total: 43 MH | Blocks: 1
⛏️  0.00 MH/s | Total: 43 MH | Blocks: 1  ❌ STUCK HERE
⛏️  0.00 MH/s | Total: 43 MH | Blocks: 1
```

### Daemon Logs:
```
Added block 1 (0eb097a6...)
New block mined! Height: 1, Hash: 0eb097a60626464b...
Broadcasting new block to 3 peers
[P2P] Sending block 0eb097a60626464b... height=1 (0 txs) to 173.249.195.59:20999
[NO MORE OUTPUT - FROZEN]
```

### RPC Test:
```bash
$ timeout 5 curl http://127.0.0.1:20998/ --cookie ./data/.cookie \
    -d '{"method":"getblockcount"}'
[TIMEOUT - No response]
```

### Port Status:
```bash
$ lsof -i :20998
dinerod   37841  ... TCP localhost:20998 (LISTEN)
dinerod   37841  ... TCP localhost:20998->localhost:51000 (ESTABLISHED)
dinero-qt 38063  ... TCP localhost:51249->localhost:20998 (SYN_SENT) ❌
dinero-mi 38280  ... TCP localhost:51004->localhost:20998 (ESTABLISHED)
```

**Note:** `SYN_SENT` indicates RPC client is trying to connect but daemon not accepting.

---

## Root Cause Analysis

### Likely Issues:

1. **Deadlock in RPC Event Loop**
   - Location: `src/daemon/main.cpp` RPC handlers
   - Problem: Mutex contention between block processing and RPC serving
   - Suspects:
     - `submitblock` RPC handler (line ~1000)
     - `getblocktemplate` RPC handler (line ~970)
     - Blockchain mutex locks during `add_block()`

2. **Synchronous P2P Broadcasting**
   - Warning in logs: `Using deprecated synchronous broadcast_message()`
   - Issue: P2P broadcast may block RPC thread
   - File: Block broadcasting code called from `submitblock`

3. **UTXO Index Locking**
   - `UTXOIndex` may be locked during block processing
   - Blocks subsequent RPC calls needing UTXO data
   - File: `src/wallet/utxo_index.cpp`

---

## Potential Fixes

### Option A: Async Block Processing (Recommended)
```cpp
// In submitblock RPC handler
rpc_server->register_method("submitblock", [&blockchain](const Json::Value& params) {
    // Parse block hex (fast, no locks)
    Block block = parse_block(block_hex);
    
    // Queue block for processing in background thread
    block_processing_queue.push(block);
    
    // Return immediately
    return success_result;  // Don't wait for validation
});

// Separate worker thread processes blocks
void block_processor_thread() {
    while (running) {
        Block block = block_processing_queue.pop();
        blockchain->add_block(block);  // Process with locks
    }
}
```

### Option B: RPC Thread Pool
```cpp
// Use multiple threads for RPC processing
HttpServer rpc_server(
    "127.0.0.1", 20998,
    thread_pool_size: 4  // Multiple RPC handlers
);
```

### Option C: Fine-Grained Locking
```cpp
// In SimpleBlockchain::add_block()
{
    std::unique_lock<std::mutex> lock(blockchain_mutex_);
    // ... critical section only ...
    lock.unlock();  // Release ASAP
}

// Don't hold lock during:
// - P2P broadcasting
// - UTXO index updates
// - Log writes
```

---

## Workaround (Current)

**Manual restart after every 1-2 blocks:**
```bash
# Kill hung daemon
pkill -9 dinerod

# Restart
cd /Users/haydarevich/Documents/DineroCoin
nohup ./build/dinerod -datadir=./data -testnet -rpcport=20998 \
  -addnode=172.93.160.131:20999 \
  -addnode=96.9.226.98:20999 \
  -addnode=173.249.195.59:20999 > daemon.log 2>&1 &
```

---

## Impact

🔴 **CRITICAL - Blocks Mainnet Launch**

- ❌ Daemon unusable for sustained mining
- ❌ Cannot process more than 1-2 blocks before hanging
- ❌ Network stalls when all nodes deadlock
- ❌ Users must manually restart daemon constantly
- ⚠️ Data corruption risk from kill -9

---

## Testing Required

After fix:
1. Mine 100+ consecutive blocks without restart
2. Stress test with 10 concurrent miners
3. Monitor RPC response times under load
4. Verify no memory leaks or lock contention
5. Test with GUI + miner + multiple RPC clients

---

## References

- Daemon: `src/daemon/main.cpp`
- Block processing: `src/daemon/simple_blockchain.cpp`
- UTXO index: `src/wallet/utxo_index.cpp`
- RPC handlers: `submitblock`, `getblocktemplate`

---

**This MUST be fixed before mainnet launch!** 🚨

