# Phase C.1: Block Relay Integration
**Wiring P2P → BlockAcceptor → Broadcast**

Date: 2025-12-16
Status: 🔵 **IN PROGRESS**

---

## Goal

Enable a single block to propagate from one node to another through the P2P network.

**Success Criteria**:
- Node A mines block → broadcasts INV to Node B
- Node B requests block via GETDATA → receives BLOCK message
- Node B validates and accepts block via BlockAcceptor
- Node B broadcasts INV to other peers (Node C, D, etc.)

---

## Architecture

### Current State

**P2P Layer** (`NetworkManager`):
- ✅ INV message handling exists
- ✅ GETDATA message handling exists
- ❌ **BLOCK message handling NOT implemented** ← Phase C.1 work
- ✅ Message broadcasting infrastructure exists

**Consensus Layer** (`BlockAcceptor`):
- ✅ `AcceptBlockFromPeer(const Block& block, const std::string& peer_id)` exists
- ✅ Full validation pipeline (PoW, parent link, merkle root, etc.)
- ✅ Calls `ChainManager::ProcessNewBlock()`
- ✅ Returns `AcceptResult` (ok/error, new height, new hash)

**Service Layer** (`ChainstateService`):
- ✅ Owns `ChainManager` and `ChainDB`
- ❌ **No P2P integration method** ← Phase C.1 work

### Integration Path (Phase C.1)

```
┌─────────────────────────────────────────────────────────┐
│ Peer sends BLOCK message                                │
└────────────────┬────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────┐
│ NetworkManager::handleBlockMessage(peer, BlockMessage)  │ ← NEW
│ - Deserializes BlockMessage → Block                     │
│ - Calls ChainstateService::ProcessIncomingBlock()       │
└────────────────┬────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────┐
│ ChainstateService::ProcessIncomingBlock(Block, peer_id) │ ← NEW
│ - Calls BlockAcceptor::AcceptBlockFromPeer()            │
│ - If accepted: calls broadcastNewBlock()                │
│ - If orphan: queues for later (Phase C.1 v2)            │
└────────────────┬────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────┐
│ BlockAcceptor::AcceptBlockFromPeer(Block, peer_id)      │ ← EXISTS
│ - Validates block (PoW, parent, merkle, etc.)           │
│ - Calls ChainManager::ProcessNewBlock()                 │
│ - Returns AcceptResult                                   │
└────────────────┬────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────┐
│ ChainManager::ProcessNewBlock(Block, new_block)         │ ← EXISTS (Phase B)
│ - Applies block to UTXO set                             │
│ - Triggers reorg if needed                              │
│ - Updates chain tip                                      │
└────────────────┬────────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────────────────┐
│ ChainstateService::broadcastNewBlock(block_hash)        │ ← NEW
│ - Creates INV message with block hash                   │
│ - Calls NetworkManager::broadcastInventory()            │
└─────────────────────────────────────────────────────────┘
```

---

## Implementation Plan

### Step 1: Add ChainstateService::ProcessIncomingBlock()

**File**: `include/daemon/services/chainstate_service.h`

**New Method**:
```cpp
public:
    // Phase C.1: P2P block relay integration
    bool ProcessIncomingBlock(const Block& block, const std::string& peer_id);
    void BroadcastNewBlock(const std::string& block_hash);
```

**File**: `src/daemon/services/chainstate_service.cpp`

**Implementation**:
```cpp
bool ChainstateService::ProcessIncomingBlock(const Block& block, const std::string& peer_id) {
    if (!started_) {
        logger_->warning("[ChainstateService] Cannot process block - service not started");
        return false;
    }

    logger_->info("[ChainstateService] Processing incoming block from peer: " + peer_id);

    // Call BlockAcceptor (performs full validation)
    AcceptResult result = BlockAcceptor::AcceptBlockFromPeer(block, peer_id);

    if (result.ok) {
        logger_->info("[ChainstateService] Block accepted: " + result.newHash +
                     " at height " + std::to_string(result.newHeight));

        // Broadcast to other peers
        BroadcastNewBlock(result.newHash);

        // Notify wallets
        notifyBlockConnected(block, result.newHeight);

        return true;
    } else {
        logger_->warning("[ChainstateService] Block rejected: " + result.message +
                        " (code: " + result.code + ")");
        return false;
    }
}

void ChainstateService::BroadcastNewBlock(const std::string& block_hash) {
    // Get network manager from context (will wire in Step 3)
    // For now, just log
    logger_->info("[ChainstateService] Broadcasting new block: " + block_hash);
}
```

### Step 2: Implement NetworkManager::handleBlockMessage()

**File**: `src/daemon/network_manager.cpp`

**Implementation**:
```cpp
bool NetworkManager::handleBlockMessage(std::shared_ptr<PeerConnection> peer, const P2PMessage& message) {
    g_logger.info("Received BLOCK message from peer " + peer->getPeerId());

    // Cast to BlockMessage
    auto block_msg = std::dynamic_pointer_cast<BlockMessage>(
        std::make_shared<P2PMessage>(message)
    );
    if (!block_msg) {
        g_logger.error("Failed to cast message to BlockMessage");
        return false;
    }

    // Deserialize block data to Block struct
    Block block;
    try {
        // TODO: Implement Block deserialization from block_msg->block_data
        // For now, create Block from raw data

        // block = Block::Deserialize(block_msg->block_data);

    } catch (const std::exception& e) {
        g_logger.error("Failed to deserialize block: " + std::string(e.what()));
        return false;
    }

    // Forward to chainstate service
    if (m_blockchain) {
        bool accepted = m_blockchain->ProcessIncomingBlock(block, peer->getPeerId());

        if (accepted) {
            m_stats.blocks_received++;
            g_logger.info("Block accepted from peer " + peer->getPeerId());
        } else {
            g_logger.warning("Block rejected from peer " + peer->getPeerId());
        }

        return accepted;
    } else {
        g_logger.error("Blockchain not initialized - cannot process block");
        return false;
    }
}
```

### Step 3: Wire NetworkManager ↔ ChainstateService

**Challenge**: NetworkManager needs reference to ChainstateService

**Solution**: Add ChainstateService to DaemonContext

**File**: `include/daemon/daemon_context.h` (check if exists)

**Add**:
```cpp
std::shared_ptr<ChainstateService> chainstate;
```

**File**: `src/daemon/services/chainstate_service.cpp`

**Update BroadcastNewBlock()**:
```cpp
void ChainstateService::BroadcastNewBlock(const std::string& block_hash) {
    // Get network manager from context
    auto network_mgr = /* get from context */;

    if (!network_mgr) {
        logger_->warning("[ChainstateService] Cannot broadcast - network manager not available");
        return;
    }

    // Create inventory vector for block
    std::vector<std::string> block_hashes = {block_hash};

    // Broadcast INV message to all peers
    network_mgr->broadcastInventory(block_hashes, "block");

    logger_->info("[ChainstateService] Broadcasted block inv: " + block_hash);
}
```

### Step 4: Handle Orphan Blocks (Phase C.1 v2)

**Orphan**: Block whose parent is not yet known

**Strategy**:
- Maintain in-memory orphan queue
- When new block accepted, check if any orphans can now be connected
- Limit orphan queue size (e.g., 100 blocks max)

**Implementation** (deferred to Phase C.1 v2 for simplicity):
```cpp
private:
    std::map<std::string, Block> orphan_blocks_;  // hash → block
    std::map<std::string, std::vector<std::string>> orphan_by_parent_;  // parent_hash → [child_hashes]
```

---

## Testing Plan

### Manual Test (2-node)

**Setup**:
1. Start Node A (port 20999)
2. Start Node B (port 21000)
3. Connect: Node B → Node A

**Test Steps**:
1. Mine block on Node A (via RPC: `mining.generatetoaddress`)
2. Verify Node A broadcasts INV to Node B
3. Verify Node B requests block via GETDATA
4. Verify Node B receives BLOCK message
5. Verify Node B accepts block (check logs)
6. Verify Node B's height increases
7. Query both nodes: `blockchain.getblockcount` should match

**Expected Logs**:

**Node A**:
```
[Mining] Mined new block: <hash> at height X
[ChainstateService] Broadcasting new block: <hash>
[NetworkManager] Sent INV to peer: 127.0.0.1:21000
```

**Node B**:
```
[NetworkManager] Received INV from peer: 127.0.0.1:20999
[NetworkManager] Requesting block via GETDATA: <hash>
[NetworkManager] Received BLOCK message from peer: 127.0.0.1:20999
[ChainstateService] Processing incoming block from peer: 127.0.0.1:20999
[BlockAcceptor] Block accepted: <hash> at height X
[ChainstateService] Block accepted: <hash> at height X
```

### Automated Test

**File**: `tests/p2p/test_2node_block_relay.sh`

**Script**:
```bash
#!/bin/bash
# Phase C.1: 2-node block relay test

# Start Node A
./dinerod -regtest -datadir=/tmp/node_a -rpcport=20000 -port=20999 -daemon

# Start Node B
./dinerod -regtest -datadir=/tmp/node_b -rpcport=20001 -port=21000 -addnode=127.0.0.1:20999 -daemon

# Wait for connection
sleep 5

# Mine block on Node A
BLOCK_HASH=$(dinero-cli -rpcport=20000 mining.generatetoaddress 1 <addr> | jq -r '.[0]')

# Wait for propagation
sleep 3

# Check Node B received block
HEIGHT_A=$(dinero-cli -rpcport=20000 blockchain.getblockcount)
HEIGHT_B=$(dinero-cli -rpcport=20001 blockchain.getblockcount)

if [ "$HEIGHT_A" == "$HEIGHT_B" ]; then
    echo "✅ Block relay test PASSED"
    exit 0
else
    echo "❌ Block relay test FAILED: height mismatch ($HEIGHT_A != $HEIGHT_B)"
    exit 1
fi
```

---

## Risk Mitigation

### Risk 1: Block Deserialization Complexity

**Issue**: `BlockMessage` contains raw `block_data`, need to deserialize to `Block` struct

**Mitigation**:
- Check if `Block::Deserialize()` exists
- If not, use `BlockAcceptor::ParseBlockFromHex()` (might need hex conversion)
- Worst case: manually parse block header + transactions

### Risk 2: Circular Dependency (NetworkManager ↔ ChainstateService)

**Issue**: Both services need references to each other

**Mitigation**:
- Use DaemonContext as mediator
- NetworkManager gets ChainstateService from context
- ChainstateService gets NetworkManager from context
- Ensure proper initialization order in daemon startup

### Risk 3: Orphan Block Handling

**Issue**: Blocks may arrive out of order (child before parent)

**Mitigation**:
- Phase C.1 v1: Reject orphans with clear log message
- Phase C.1 v2: Implement orphan queue
- Peer will eventually resend missing parent

---

## Success Criteria

- ✅ `NetworkManager::handleBlockMessage()` implemented
- ✅ `ChainstateService::ProcessIncomingBlock()` implemented
- ✅ `ChainstateService::BroadcastNewBlock()` implemented
- ✅ NetworkManager ↔ ChainstateService wired via DaemonContext
- ✅ Manual 2-node test passes
- ✅ Automated test script passes

---

## Timeline

**Day 1 (Today)**:
- Implement ChainstateService methods ✅
- Implement NetworkManager::handleBlockMessage()
- Wire services via DaemonContext

**Day 2 (Tomorrow)**:
- Build and debug integration
- Manual testing (2-node relay)
- Fix any deserialization issues

**Day 3**:
- Write automated test
- Handle edge cases (orphans, invalid blocks)
- Documentation

---

## Next Phase

**Phase C.2**: Header-First Sync
- Implement GETHEADERS / HEADERS message handling
- Download headers before blocks
- Validate header chain

---

**Phase C.1 Status**: 🔵 **IMPLEMENTING**
