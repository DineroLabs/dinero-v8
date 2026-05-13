# Phase 5: Network & P2P Hardening

**Status**: 🚧 IN PROGRESS
**Start Date**: November 11, 2025
**Branch**: `feat/phase5-network-hardening`
**Prerequisites**: ✅ Phase 4B Complete (Architecture V3 in main)

---

## 🎯 Executive Summary

Phase 5 focuses on hardening the network and P2P layer of DineroCoin, implementing production-grade peer management, synchronization optimizations, and advanced mempool features. This phase builds upon the service architecture foundation from Phase 3 and ensures the network can scale efficiently and resist attacks.

### Goals
1. **Faster Synchronization** - Headers-first sync for rapid initial blockchain download (IBD)
2. **Bandwidth Optimization** - Compact block relay to reduce network overhead
3. **Robust Peer Management** - Intelligent peer selection, reputation tracking, DoS protection
4. **Advanced Mempool Features** - RBF, CPFP, and mempool reconciliation
5. **Production Security** - Rate limiting, attack mitigation, peer banning

---

## 📋 Phase 5 Breakdown

### Phase 5A: Headers-First Sync Optimization

**Goal**: Implement headers-first synchronization for faster initial blockchain download.

**Background**:
Currently, DineroCoin downloads full blocks sequentially. Headers-first sync downloads block headers first (lightweight), validates the chain, then downloads blocks in parallel. This dramatically speeds up IBD.

**Bitcoin Core Reference**: [headers-first implementation](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp)

#### Components to Implement

1. **Header Chain Management** (`include/p2p/header_chain.h`)
   - Store headers separately from full blocks
   - Validate header chain (PoW, timestamps, chainwork)
   - Track best header vs. best block

   ```cpp
   class HeaderChain {
   public:
       bool addHeader(const BlockHeader& header);
       bool isHeaderValid(const BlockHeader& header);
       BlockHeader getBestHeader() const;
       uint64_t getBestHeaderHeight() const;
       bool hasHeader(const uint256& hash) const;
   };
   ```

2. **Header Download Protocol** (`src/p2p/header_sync.cpp`)
   - Request headers from peers (`getheaders` message)
   - Download headers in batches (2000 headers per message)
   - Validate headers incrementally
   - Switch to block download once headers are synced

3. **Parallel Block Download** (`src/p2p/block_download_manager.cpp`)
   - Download blocks in parallel from multiple peers
   - Prioritize blocks based on height
   - Handle out-of-order block arrival
   - Verify blocks against header chain

#### Implementation Steps
1. Add `HeaderChain` class to store header-only blockchain
2. Modify P2P message handlers for `getheaders`/`headers` messages
3. Implement header validation logic (PoW, timestamps, chainwork)
4. Add parallel block download from multiple peers
5. Update sync state machine to use headers-first flow
6. Add metrics for header sync progress

#### Performance Targets
- Header download: 100,000 headers in < 10 seconds
- Block download parallelism: 4-8 concurrent peers
- IBD improvement: 3-5x faster than sequential sync

---

### Phase 5B: Compact Block Relay (BIP152-Style)

**Goal**: Reduce bandwidth for block propagation by sending only transaction IDs.

**Background**:
When a miner finds a new block, instead of sending the full block (with all transaction data), compact block relay sends:
- Block header
- Short transaction IDs (8 bytes instead of full transaction)
- Missing transactions (if not in mempool)

Nodes reconstruct the full block from their mempool + the short IDs.

**Bitcoin BIP**: [BIP152 Compact Blocks](https://github.com/bitcoin/bips/blob/master/bip-0152.mediawiki)

#### Components to Implement

1. **Compact Block Structure** (`include/primitives/compact_block.h`)
   ```cpp
   struct CompactBlock {
       BlockHeader header;
       uint64_t nonce;  // For short ID calculation
       std::vector<ShortTxID> short_ids;  // 8-byte tx IDs
       std::vector<Transaction> prefilled_txs;  // Coinbase + others
   };

   struct ShortTxID {
       uint64_t short_id;  // SipHash-2-4 of txid
   };
   ```

2. **Short ID Calculation** (`src/primitives/compact_block.cpp`)
   - Use SipHash-2-4 for short transaction IDs
   - Nonce prevents collisions across different blocks
   - 8-byte short IDs (vs. 32-byte full txids)

3. **Block Reconstruction** (`src/p2p/compact_block_processor.cpp`)
   ```cpp
   class CompactBlockProcessor {
   public:
       // Try to reconstruct full block from compact block + mempool
       ReconstructionResult reconstructBlock(
           const CompactBlock& compact,
           const Mempool& mempool
       );

       // Request missing transactions if reconstruction fails
       std::vector<uint32_t> getMissingTxIndices();
   };
   ```

4. **P2P Protocol Changes** (`src/p2p/compact_block_handler.cpp`)
   - Add `cmpctblock` message type
   - Add `getblocktxn` message (request missing txs)
   - Add `blocktxn` message (send missing txs)
   - High-bandwidth mode: send compact blocks unsolicited
   - Low-bandwidth mode: announce blocks, wait for `getdata`

#### Implementation Steps
1. Define CompactBlock and ShortTxID structures
2. Implement SipHash-2-4 for short ID calculation
3. Add compact block serialization/deserialization
4. Implement block reconstruction from mempool
5. Add P2P message handlers (`cmpctblock`, `getblocktxn`, `blocktxn`)
6. Add high-bandwidth/low-bandwidth mode negotiation
7. Add metrics for compact block efficiency

#### Performance Targets
- Bandwidth savings: 90-95% for blocks with mempool transactions
- Reconstruction success rate: > 98%
- Block propagation time: < 1 second

---

### Phase 5C: Peer Connection Management

**Goal**: Intelligent peer selection, connection management, and reputation tracking.

#### Components to Implement

1. **Peer Reputation System** (`include/p2p/peer_reputation.h`)
   ```cpp
   struct PeerReputation {
       uint64_t peer_id;
       int reputation_score;  // -100 to +100

       // Positive events
       uint64_t blocks_provided;
       uint64_t valid_txs_relayed;
       uint64_t headers_provided;

       // Negative events
       uint64_t invalid_blocks;
       uint64_t invalid_txs;
       uint64_t connection_failures;
       uint64_t slow_response_count;

       time_t last_seen;
       time_t first_seen;
   };
   ```

2. **Peer Selection Strategy** (`src/p2p/peer_selector.cpp`)
   - Prioritize high-reputation peers
   - Geographic diversity (don't connect to same AS)
   - Version diversity (support older clients)
   - Connection limits (max inbound/outbound)
   - Eviction strategy for full peer slots

3. **Connection Manager** (`src/p2p/connection_manager.cpp`)
   ```cpp
   class ConnectionManager {
   public:
       void connectToPeer(const PeerAddress& addr);
       void disconnectPeer(uint64_t peer_id, const std::string& reason);
       void banPeer(const std::string& ip, uint64_t duration_seconds);
       void updatePeerReputation(uint64_t peer_id, int delta);

       std::vector<Peer> selectPeersForRequest(int count);
       void handlePeerMisbehavior(uint64_t peer_id, MisbehaviorType type);
   };
   ```

4. **Peer Address Manager** (`src/p2p/addr_manager.cpp`)
   - Store peer addresses with timestamps
   - Prioritize recently-seen addresses
   - Avoid stale addresses
   - Support DNS seeds for bootstrapping

#### Implementation Steps
1. Add PeerReputation structure and database storage
2. Implement reputation scoring algorithm
3. Add peer selection strategy (reputation, diversity, limits)
4. Implement connection eviction policy
5. Add peer banning mechanism (IP-based)
6. Add addr_manager for peer address storage
7. Add metrics for peer health and reputation

#### Performance Targets
- Max outbound peers: 8-12
- Max inbound peers: 125
- Peer selection time: < 100ms
- Ban duration: 1 hour (minor) to 24 hours (major)

---

### Phase 5D: DoS Protection and Rate Limiting

**Goal**: Protect the network from denial-of-service attacks and resource exhaustion.

#### Attack Vectors to Address

1. **Excessive Message Spam**
   - Limit messages per peer per second
   - Disconnect peers exceeding rate limits

2. **Large Message Attacks**
   - Limit message sizes
   - Disconnect peers sending oversized messages

3. **Invalid Block/Transaction Floods**
   - Track invalid messages per peer
   - Ban peers sending too many invalid messages

4. **Connection Exhaustion**
   - Limit connections per IP
   - Rate-limit new connections
   - Evict low-reputation peers when full

5. **Mempool Flooding**
   - Limit transactions per peer per second
   - Limit mempool size (evict low-fee transactions)

#### Components to Implement

1. **Rate Limiter** (`include/p2p/rate_limiter.h`)
   ```cpp
   class RateLimiter {
   public:
       bool allowMessage(uint64_t peer_id, MessageType type);
       bool allowBytes(uint64_t peer_id, size_t bytes);
       void recordMessage(uint64_t peer_id, MessageType type);
       void recordBytes(uint64_t peer_id, size_t bytes);
   };
   ```

2. **DoS Protection** (`src/p2p/dos_protection.cpp`)
   ```cpp
   class DoSProtection {
   public:
       void recordMisbehavior(uint64_t peer_id, MisbehaviorType type);
       bool shouldBanPeer(uint64_t peer_id);
       bool shouldDisconnectPeer(uint64_t peer_id);

       // Misbehavior scoring
       int getMisbehaviorScore(uint64_t peer_id);
       void decayMisbehaviorScores();  // Called periodically
   };
   ```

3. **Connection Rate Limiting** (`src/p2p/connection_rate_limiter.cpp`)
   - Limit new connections per IP per minute
   - Whitelist trusted IPs (local network, known nodes)
   - Temporary bans for excessive connection attempts

#### Rate Limits (Recommended)

| Resource | Limit | Action on Exceed |
|----------|-------|------------------|
| Messages per peer | 100/second | Disconnect |
| Bytes per peer | 10 MB/second | Disconnect |
| Invalid blocks | 3 per hour | Ban 1 hour |
| Invalid transactions | 10 per hour | Ban 1 hour |
| Connections per IP | 3 | Reject new connections |
| New connections per IP/minute | 5 | Temporary ban (10 min) |

#### Implementation Steps
1. Add RateLimiter class with token bucket algorithm
2. Implement DoSProtection with misbehavior scoring
3. Add connection rate limiting per IP
4. Integrate rate limiter into P2P message handlers
5. Add peer banning for excessive misbehavior
6. Add metrics for rate limiting and bans

#### Performance Targets
- Rate limit overhead: < 1ms per message
- False positive rate: < 0.1%
- Effective attack mitigation: > 99.9%

---

### Phase 5E: Replace-By-Fee (RBF) Support

**Goal**: Allow users to replace unconfirmed transactions with higher-fee versions.

**Background**:
RBF allows a user to "bump" the fee of a stuck transaction by broadcasting a replacement transaction with a higher fee. This is useful when network fees increase or a transaction is taking too long to confirm.

**Bitcoin BIP**: [BIP125 Opt-in RBF](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki)

#### Components to Implement

1. **RBF Signaling** (`include/primitives/transaction.h`)
   ```cpp
   class Transaction {
   public:
       bool isRBFSignaled() const;  // Check if nSequence < 0xfffffffe
       bool canReplaceTransaction(const Transaction& original) const;
   };
   ```

2. **Mempool Replacement Logic** (`src/mempool/rbf_handler.cpp`)
   ```cpp
   class RBFHandler {
   public:
       // Check if replacement transaction is valid
       bool canReplace(
           const Transaction& original,
           const Transaction& replacement,
           std::string& error
       );

       // Replace transaction in mempool
       bool replaceTrans action(
           Mempool& mempool,
           const Transaction& original,
           const Transaction& replacement
       );
   };
   ```

3. **RBF Rules** (BIP125)
   - Replacement tx must signal RBF (or original signaled RBF)
   - Replacement tx must pay higher fee (absolute, not just fee rate)
   - Replacement tx must pay for its own bandwidth (min fee increment)
   - Replacement tx cannot add new unconfirmed parents
   - Replacement tx must evict all conflicting transactions

#### Implementation Steps
1. Add RBF signaling check (nSequence < 0xfffffffe)
2. Implement RBFHandler with BIP125 rules
3. Add mempool replacement logic
4. Update P2P relay to handle replacement transactions
5. Add wallet support for creating replacement transactions
6. Add RPC method: `bumpfee <txid> <new_fee_rate>`
7. Add metrics for RBF usage

#### Performance Targets
- RBF validation time: < 10ms
- Mempool replacement time: < 50ms
- Min fee increment: 0.00001 DIN

---

### Phase 5F: Child-Pays-For-Parent (CPFP)

**Goal**: Allow child transactions to pay for low-fee parent transactions.

**Background**:
CPFP allows a transaction recipient to "rescue" a low-fee transaction by spending its output in a high-fee child transaction. Miners include both transactions together because the combined fee rate is profitable.

#### Components to Implement

1. **Ancestor/Descendant Tracking** (`include/mempool/mempool.h`)
   ```cpp
   struct MempoolEntry {
       Transaction tx;
       uint64_t fee;
       size_t size;

       // CPFP tracking
       std::vector<uint256> ancestors;    // Parent transactions
       std::vector<uint256> descendants;  // Child transactions
       uint64_t ancestor_fee_total;
       size_t ancestor_size_total;

       double getAncestorFeeRate() const {
           return (double)ancestor_fee_total / ancestor_size_total;
       }
   };
   ```

2. **Package Fee Rate Calculation** (`src/mempool/package_calculator.cpp`)
   ```cpp
   class PackageCalculator {
   public:
       double calculatePackageFeeRate(
           const Mempool& mempool,
           const uint256& txid
       );

       // Get all ancestors of a transaction
       std::vector<uint256> getAncestors(
           const Mempool& mempool,
           const uint256& txid
       );
   };
   ```

3. **Mining Template with CPFP** (`src/daemon/mining_template_builder.cpp`)
   - Sort transactions by package fee rate (not individual fee rate)
   - Include low-fee parents if high-fee children exist
   - Respect ancestor/descendant limits

#### Implementation Steps
1. Add ancestor/descendant tracking to MempoolEntry
2. Implement package fee rate calculation
3. Update mempool to track transaction relationships
4. Modify mining template builder to use package fee rates
5. Add limits (max ancestors, max descendants)
6. Add metrics for CPFP usage

#### Performance Targets
- Package fee rate calculation: < 10ms
- Max ancestors: 25
- Max descendants: 25
- Max package size: 100 KB

---

### Phase 5G: Mempool Reconciliation

**Goal**: Synchronize mempools between peers to reduce orphan blocks and improve propagation.

**Background**:
When nodes have different mempools (due to network partitions, different fee policies, etc.), they may reject blocks because they don't have the transactions. Mempool reconciliation exchanges transaction inventories to keep mempools synchronized.

**Bitcoin Implementation**: [Erlay (BIP330)](https://github.com/bitcoin/bips/blob/master/bip-0330.mediawiki)

#### Components to Implement

1. **Mempool Inventory** (`include/mempool/mempool_inventory.h`)
   ```cpp
   class MempoolInventory {
   public:
       // Create inventory of mempool transactions
       std::vector<uint256> getInventory() const;

       // Get transactions not in peer's inventory
       std::vector<Transaction> getMissingTransactions(
           const std::vector<uint256>& peer_inventory
       );
   };
   ```

2. **Mempool Sync Protocol** (`src/p2p/mempool_sync.cpp`)
   ```cpp
   class MempoolSync {
   public:
       // Request peer's mempool inventory
       void requestMempoolInventory(uint64_t peer_id);

       // Compare inventories and request missing transactions
       void reconcileMempool(
           uint64_t peer_id,
           const std::vector<uint256>& peer_inventory
       );

       // Send missing transactions to peer
       void sendMissingTransactions(
           uint64_t peer_id,
           const std::vector<uint256>& missing_txids
       );
   };
   ```

3. **Bloom Filters for Efficient Sync** (`src/p2p/mempool_bloom.cpp`)
   - Use Bloom filters to represent mempool inventory
   - Reduce bandwidth (instead of sending all txids)
   - Acceptable false positive rate (1%)

#### Implementation Steps
1. Add mempool inventory generation
2. Implement Bloom filter for mempool representation
3. Add P2P messages for mempool sync (`getmempool`, `mempoolinv`)
4. Implement mempool reconciliation algorithm
5. Add periodic mempool sync (every 10 minutes)
6. Add metrics for mempool sync efficiency

#### Performance Targets
- Mempool sync bandwidth: < 100 KB per sync
- Sync frequency: Every 10 minutes
- False positive rate: < 1%
- Sync latency: < 5 seconds

---

## 🔒 Security Considerations

### Phase 5A (Headers-First)
- **Attack**: Fake header chain with more chainwork
- **Mitigation**: Validate PoW, verify checkpoints, require peer consensus

### Phase 5B (Compact Blocks)
- **Attack**: Short ID collision (malicious node sends fake short IDs)
- **Mitigation**: Random nonce per block, SipHash collision resistance

### Phase 5C (Peer Management)
- **Attack**: Sybil attack (attacker controls many peers)
- **Mitigation**: Reputation system, geographic diversity, connection limits

### Phase 5D (DoS Protection)
- **Attack**: Message flood, connection exhaustion
- **Mitigation**: Rate limiting, banning, connection limits

### Phase 5E (RBF)
- **Attack**: RBF pinning (prevent replacement)
- **Mitigation**: BIP125 rules, min fee increment

### Phase 5F (CPFP)
- **Attack**: Long dependency chains (DoS)
- **Mitigation**: Ancestor/descendant limits (25 each)

### Phase 5G (Mempool Sync)
- **Attack**: Bloom filter fingerprinting (privacy leak)
- **Mitigation**: Bloom filter rotation, random false positives

---

## 📊 Success Metrics

### Performance Metrics
- **IBD Time**: 3-5x faster with headers-first sync
- **Block Propagation**: < 1 second with compact blocks
- **Bandwidth Savings**: 90-95% for block relay
- **Mempool Hit Rate**: > 98% for compact block reconstruction

### Security Metrics
- **DoS Mitigation**: > 99.9% attack prevention
- **False Positive Rate**: < 0.1% for legitimate peers
- **Peer Uptime**: > 99% for high-reputation peers
- **Ban Accuracy**: > 99% correct bans (no false positives)

### Network Metrics
- **Peer Count**: 8-12 outbound, up to 125 inbound
- **Geographic Diversity**: Peers from 10+ countries
- **Average Peer Reputation**: > 50 (scale: -100 to +100)
- **Connection Success Rate**: > 95%

---

## 🧪 Testing Strategy

### Unit Tests
- Header validation logic
- Compact block reconstruction
- Rate limiter algorithms
- Reputation scoring
- RBF/CPFP rules

### Integration Tests
- Headers-first sync flow
- Compact block relay end-to-end
- Peer selection and eviction
- DoS attack simulations
- Mempool reconciliation

### Stress Tests
- 10,000 blocks headers-first sync
- 1000 transactions/second compact block relay
- 100 simultaneous peer connections
- DoS attack (10,000 messages/second)
- Mempool with 100,000 transactions

---

## 📚 References

- [Bitcoin Core: Headers-First Sync](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp)
- [BIP152: Compact Block Relay](https://github.com/bitcoin/bips/blob/master/bip-0152.mediawiki)
- [BIP125: Opt-in RBF](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki)
- [BIP330: Erlay (Transaction Reconciliation)](https://github.com/bitcoin/bips/blob/master/bip-0330.mediawiki)
- [Bitcoin Core: Peer Management](https://github.com/bitcoin/bitcoin/blob/master/src/net.cpp)

---

## 🔖 Git Branch

**Branch**: `feat/phase5-network-hardening`
**Base**: `main` (after Phase 4B merge)
**Target**: Incremental merges for each sub-phase (5A, 5B, etc.)

---

**Document Version**: 1.0
**Last Updated**: November 11, 2025
**Status**: 🚧 In Progress
