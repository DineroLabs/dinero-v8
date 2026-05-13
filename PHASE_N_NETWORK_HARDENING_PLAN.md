# Phase N: Network Hardening — Focused Implementation Plan

**Status:** 🚀 READY TO START (December 19, 2025)
**Prerequisites:**
- ✅ Phase H.6 (Header Sync) LOCKED
- ✅ Phase P.2 (Pruning) LOCKED
- ✅ PeerScoringService exists
- ✅ MultiPeerHeadersSync exists

**Scope:** Complete Phase 5C (Peer Management) + Phase 5D (DoS Protection) essentials

---

## Executive Summary

Phase N implements **production-grade peer management** focusing on the critical missing pieces:

1. **Peer Eviction** - Intelligent slot management when connections are full
2. **Connection Limits** - Per-IP limits, max inbound/outbound enforcement
3. **Rate Limiting** - Token bucket algorithm for message/byte limits
4. **DoS Protection** - Integration with existing PeerScoringService
5. **Block Request Scheduling** - Smart peer selection for block downloads

**What Makes This Safe:**
- Builds on existing PeerScoringService (no duplicate misbehavior logic)
- Uses proven Bitcoin Core eviction strategy
- Conservative limits (tunable via config)
- No changes to consensus or validation

---

## Current State Assessment

### ✅ What Already Exists

1. **PeerScoringService** (`include/daemon/services/peer_scoring_service.h`)
   - Misbehavior tracking (INVALID_BLOCK, INVALID_TX, etc.)
   - Ban management with duration
   - Score decay over time
   - Persistence (ban list save/load)
   - **Status:** COMPLETE - Integration V3 service wrapper

2. **MultiPeerHeadersSync** (`include/p2p/multi_peer_headers_sync.h`)
   - Parallel header download from 4-8 peers
   - Per-peer state tracking
   - Timeout handling
   - Reputation scoring for headers
   - **Status:** COMPLETE - Headers-first sync ready

3. **Basic PeerManager** (`include/p2p/peer_manager_v2.h`)
   - Connection dialing to seeds
   - Max outbound limit (8)
   - Peer address tracking
   - **Status:** MINIMAL - Needs expansion

### ❌ What's Missing (Phase N Scope)

1. **Peer Eviction Strategy**
   - No eviction when peer slots full
   - No protection for valuable peers
   - No diversity preservation

2. **Connection Limits**
   - No max inbound limit (vulnerable to connection exhaustion)
   - No per-IP connection limit (Sybil attack vector)
   - No connection rate limiting

3. **Rate Limiting Infrastructure**
   - No message rate limits (msg/sec per peer)
   - No byte rate limits (MB/sec per peer)
   - No DoS protection at message handler level

4. **Headers-Only Peer Mode**
   - Cannot run as headers-only node
   - No differentiation for pruned nodes

5. **Block Request Scheduling**
   - No intelligent peer selection for block requests
   - No timeout/retry logic for block downloads
   - No parallel block download orchestration

---

## Implementation Plan

### Phase N.1: Connection Management (Priority 1)

**Goal:** Enforce connection limits and implement eviction strategy

#### 1.1 Connection Limits

**File:** `include/p2p/connection_manager.h` (NEW)

```cpp
class ConnectionManager {
public:
    struct Limits {
        uint32_t max_outbound{8};       // Max outbound connections
        uint32_t max_inbound{125};      // Max inbound connections (Bitcoin-standard)
        uint32_t max_per_ip{3};         // Max connections from same IP
        uint32_t max_feeler{1};         // Max feeler connections (test reachability)
    };

    ConnectionManager(Limits limits);

    // Connection management
    bool canAcceptInbound(const std::string& peer_ip) const;
    bool canConnectOutbound() const;
    bool shouldEvictPeer() const;

    // Peer tracking
    void onPeerConnected(const std::string& peer_id, const std::string& ip, bool inbound);
    void onPeerDisconnected(const std::string& peer_id);

    // Get current counts
    uint32_t getInboundCount() const;
    uint32_t getOutboundCount() const;
    uint32_t getConnectionsFromIP(const std::string& ip) const;

    // Eviction
    std::optional<std::string> selectPeerToEvict();

private:
    Limits limits_;
    std::unordered_map<std::string, PeerConnectionInfo> peers_;
    std::unordered_map<std::string, std::vector<std::string>> ip_to_peers_;
};
```

**Implementation Steps:**
1. Create `ConnectionManager` class
2. Add connection tracking (inbound/outbound/IP counts)
3. Implement `canAcceptInbound()` with limits check
4. Integrate into NetworkManager/PeerManager

**Testing:**
- Reject inbound when at max_inbound (125)
- Reject connections when IP has 3+ connections
- Verify outbound limit enforcement (8)

---

#### 1.2 Peer Eviction Strategy

**Bitcoin Core Reference:** `SelectNodeToEvict()` in `net.cpp`

**Eviction Priority (Never Evict):**
1. ✅ Peers we connected to (outbound)
2. ✅ Peers with recent valid blocks
3. ✅ Peers with recent valid transactions
4. ✅ Whitelisted peers
5. ✅ Peers from unique /16 subnets (diversity)

**Eviction Criteria (Evict First):**
1. ❌ Lowest ping time (network lag)
2. ❌ Oldest connection without useful data
3. ❌ Lowest misbehavior score (from PeerScoringService)
4. ❌ Connections from same /16 subnet (Sybil protection)

**File:** `src/p2p/peer_eviction.cpp` (NEW)

```cpp
struct EvictionCandidate {
    std::string peer_id;
    std::string ip;
    bool is_outbound;
    bool is_whitelisted;
    uint64_t ping_time_ms;
    time_t last_block_time;
    time_t last_tx_time;
    int32_t misbehavior_score;  // From PeerScoringService
    time_t connection_time;
    std::string subnet;  // /16 network (e.g., "192.168.0.0/16")
};

class PeerEvictionStrategy {
public:
    /**
     * Select peer to evict using Bitcoin Core algorithm
     *
     * Returns peer_id to evict, or std::nullopt if no peers can be evicted.
     */
    std::optional<std::string> selectPeerToEvict(
        const std::vector<EvictionCandidate>& candidates
    );

private:
    // Protection logic
    bool isProtectedPeer(const EvictionCandidate& candidate);

    // Eviction scoring
    void eraseProtectedCandidates(std::vector<EvictionCandidate>& candidates);
    void eraseSubnetDiversity(std::vector<EvictionCandidate>& candidates);
    EvictionCandidate selectWorstPeer(const std::vector<EvictionCandidate>& candidates);
};
```

**Algorithm:**
1. Remove outbound peers (never evict)
2. Remove whitelisted peers (never evict)
3. Remove peers with recent blocks/txs (last 5 min)
4. Preserve subnet diversity (keep 1 peer per /16)
5. From remaining: evict peer with worst score (lowest ping + lowest reputation + oldest)

**Integration:**
```cpp
// In ConnectionManager::onPeerConnected()
if (getInboundCount() >= limits_.max_inbound) {
    auto to_evict = peer_eviction_strategy_.selectPeerToEvict(candidates);
    if (to_evict) {
        disconnectPeer(*to_evict, "evicted for new connection");
    }
}
```

---

### Phase N.2: Rate Limiting (Priority 2)

**Goal:** Prevent message/byte floods from malicious peers

#### 2.1 Token Bucket Rate Limiter

**File:** `include/p2p/rate_limiter.h` (NEW)

```cpp
enum class LimitType {
    MESSAGES_PER_SECOND,
    BYTES_PER_SECOND,
    BLOCKS_PER_MINUTE,
    TXS_PER_MINUTE
};

class TokenBucketLimiter {
public:
    TokenBucketLimiter(uint32_t capacity, uint32_t refill_rate);

    // Check if action is allowed
    bool tryConsume(uint32_t tokens = 1);

    // Refill tokens based on time elapsed
    void refill();

    // Get current token count
    uint32_t available() const { return tokens_; }

private:
    uint32_t capacity_;        // Max tokens
    uint32_t refill_rate_;     // Tokens added per second
    uint32_t tokens_;          // Current tokens
    std::chrono::steady_clock::time_point last_refill_;
};

class RateLimiter {
public:
    struct Limits {
        uint32_t messages_per_sec{100};        // 100 msg/sec per peer
        uint32_t bytes_per_sec{10 * 1024 * 1024};  // 10 MB/sec per peer
        uint32_t blocks_per_min{10};           // 10 blocks/min
        uint32_t txs_per_min{100};             // 100 txs/min
    };

    RateLimiter(Limits limits);

    // Check and consume tokens
    bool allowMessage(const std::string& peer_id);
    bool allowBytes(const std::string& peer_id, size_t bytes);
    bool allowBlock(const std::string& peer_id);
    bool allowTransaction(const std::string& peer_id);

    // Periodic maintenance
    void refillAll();

private:
    Limits limits_;
    std::unordered_map<std::string, std::unique_ptr<TokenBucketLimiter>> message_limiters_;
    std::unordered_map<std::string, std::unique_ptr<TokenBucketLimiter>> byte_limiters_;
    std::unordered_map<std::string, std::unique_ptr<TokenBucketLimiter>> block_limiters_;
    std::unordered_map<std::string, std::unique_ptr<TokenBucketLimiter>> tx_limiters_;
};
```

**Integration into Message Handlers:**
```cpp
// In NetworkManager::onMessageReceived()
if (!rate_limiter_->allowMessage(peer_id)) {
    peer_scoring_->addMisbehavior(peer_id, MisbehaviorType::RATE_LIMIT_EXCEEDED);
    disconnectPeer(peer_id, "rate limit exceeded");
    return;
}

if (!rate_limiter_->allowBytes(peer_id, message.size())) {
    peer_scoring_->addMisbehavior(peer_id, MisbehaviorType::BANDWIDTH_EXCEEDED);
    disconnectPeer(peer_id, "bandwidth limit exceeded");
    return;
}
```

**Configuration (Recommended Limits):**
| Resource | Limit | Action on Exceed |
|----------|-------|------------------|
| Messages | 100/sec | Disconnect |
| Bytes | 10 MB/sec | Disconnect |
| Blocks | 10/min | Disconnect |
| Transactions | 100/min | Disconnect |

---

### Phase N.3: Block Request Scheduling (Priority 3)

**Goal:** Smart peer selection for block downloads with timeout/retry

#### 3.1 Block Download Manager

**File:** `include/p2p/block_download_manager.h` (NEW)

```cpp
struct BlockRequest {
    std::string block_hash;
    uint32_t height;
    std::string assigned_peer;
    std::chrono::steady_clock::time_point request_time;
    uint32_t retry_count{0};
    bool in_flight{false};
};

class BlockDownloadManager {
public:
    struct Config {
        uint32_t max_blocks_in_flight{16};         // Max simultaneous block requests
        std::chrono::seconds request_timeout{30};  // 30s timeout for block response
        uint32_t max_retries{3};                   // Retry up to 3 times
        uint32_t parallel_peers{4};                // Request from 4 peers simultaneously
    };

    BlockDownloadManager(Config config);

    // Schedule block download
    void requestBlock(const std::string& block_hash, uint32_t height);

    // Process received block
    void onBlockReceived(const std::string& block_hash, const std::string& peer_id);

    // Handle timeouts
    void checkTimeouts();

    // Peer selection for requests
    std::optional<std::string> selectPeerForBlock(const std::string& block_hash);

    // Statistics
    uint32_t getInFlightCount() const;
    uint32_t getPendingCount() const;

private:
    Config config_;
    std::unordered_map<std::string, BlockRequest> in_flight_;
    std::queue<std::string> pending_blocks_;

    // Peer selection state
    std::unordered_map<std::string, uint32_t> peer_block_counts_;  // Track blocks per peer
};
```

**Peer Selection Strategy:**
1. Prefer peers with high reputation score (from PeerScoringService)
2. Load balance (prefer peers with fewer in-flight blocks)
3. Avoid recently-timed-out peers
4. Geographic diversity (if available)

**Integration:**
```cpp
// After headers are synced
for (uint32_t height = current_height + 1; height <= target_height; ++height) {
    std::string block_hash = getBlockHashByHeight(height);
    block_download_manager_->requestBlock(block_hash, height);
}

// Periodic timeout check
block_download_manager_->checkTimeouts();

// On timeout, retry with different peer
auto peer = block_download_manager_->selectPeerForBlock(block_hash);
if (peer) {
    sendGetData(*peer, block_hash);
}
```

---

### Phase N.4: Headers-Only Mode (Optional - P.3 Integration)

**Goal:** Support pruned nodes that only serve headers

**File:** `include/p2p/peer_capabilities.h` (NEW)

```cpp
enum class PeerCapability {
    FULL_BLOCKS,        // Peer can serve full blocks
    HEADERS_ONLY,       // Peer can only serve headers (pruned node)
    COMPACT_BLOCKS,     // Peer supports BIP152 compact blocks
    WITNESS,            // Peer supports SegWit
};

struct PeerInfo {
    std::string peer_id;
    std::set<PeerCapability> capabilities;
    uint32_t best_height;
    bool is_pruned;
    uint32_t prune_height;  // Lowest block peer can serve
};
```

**Integration with PruneService:**
```cpp
// Announce our capabilities in VERSION message
if (prune_service_->isEnabled()) {
    version_msg.capabilities.insert(PeerCapability::HEADERS_ONLY);
    version_msg.is_pruned = true;
    version_msg.prune_height = prune_service_->getStats().lowest_block_height;
}

// Only request blocks peer can serve
if (peer.is_pruned && height < peer.prune_height) {
    // Skip this peer, find another
    continue;
}
```

---

## Implementation Order

### Week 1: Connection Management
- ✅ Day 1-2: ConnectionManager class + connection limits
- ✅ Day 3-4: Peer eviction strategy (Bitcoin Core algorithm)
- ✅ Day 5: Testing + integration with NetworkManager

### Week 2: Rate Limiting + Block Scheduling
- ✅ Day 1-2: TokenBucket rate limiter + RateLimiter class
- ✅ Day 3: Integration with message handlers
- ✅ Day 4-5: BlockDownloadManager + peer selection

### Week 3: Polish + Testing
- ✅ Day 1-2: Headers-only mode (if needed)
- ✅ Day 3-4: Integration tests + stress tests
- ✅ Day 5: Documentation + lock file

---

## Testing Requirements

### Unit Tests

1. **ConnectionManager Tests**
   - Enforce max_inbound limit (125)
   - Enforce max_per_ip limit (3)
   - Enforce max_outbound limit (8)

2. **Peer Eviction Tests**
   - Never evict outbound peers
   - Never evict peers with recent blocks
   - Prefer evicting low-reputation peers
   - Preserve subnet diversity

3. **Rate Limiter Tests**
   - Token bucket refill logic
   - Message rate limit enforcement
   - Byte rate limit enforcement
   - Multiple limiters per peer

4. **Block Download Tests**
   - Timeout detection (30s)
   - Retry with different peer
   - Max retries (3) before giving up
   - Parallel download (4 peers)

### Integration Tests

1. **Connection Exhaustion Test**
   - Connect 125 inbound peers
   - Verify 126th peer triggers eviction
   - Verify worst peer is evicted

2. **Rate Limit DoS Test**
   - Send 1000 messages/sec from one peer
   - Verify disconnect after 100 msg/sec
   - Verify peer is banned

3. **Block Download Test**
   - Request 100 blocks in parallel
   - Simulate peer timeout
   - Verify retry with different peer
   - Verify all blocks downloaded

---

## Success Criteria

### Connection Management
✅ Max inbound peers enforced (125)
✅ Max outbound peers enforced (8)
✅ Max connections per IP enforced (3)
✅ Peer eviction works correctly (preserves valuable peers)
✅ No legitimate peers incorrectly evicted

### Rate Limiting
✅ Message rate limit enforced (100 msg/sec)
✅ Byte rate limit enforced (10 MB/sec)
✅ DoS attacks mitigated (> 99.9% blocked)
✅ False positive rate < 0.1%

### Block Scheduling
✅ Parallel block download works (4-8 peers)
✅ Timeouts detected and retried (30s)
✅ Failed downloads eventually succeed (max 3 retries)
✅ Load balanced across peers

---

## Integration with Existing Services

### PeerScoringService
- **Connection:** Rate limiter reports RATE_LIMIT_EXCEEDED misbehavior
- **Connection:** Eviction strategy uses reputation scores
- **No Changes:** PeerScoringService API remains unchanged

### MultiPeerHeadersSync
- **Connection:** BlockDownloadManager uses same peer selection logic
- **Connection:** Headers-only mode affects peer capabilities
- **No Changes:** Header sync logic unchanged

### PruneService
- **Connection:** Headers-only mode reads prune_height from PruneService
- **Connection:** VERSION message includes pruning status
- **No Changes:** Pruning logic unchanged

---

## Configuration (Tunable Limits)

**File:** `dinero.conf`

```ini
[network]
max_outbound_peers=8
max_inbound_peers=125
max_connections_per_ip=3

[rate_limits]
messages_per_sec=100
bytes_per_sec=10485760  # 10 MB
blocks_per_min=10
txs_per_min=100

[block_download]
max_blocks_in_flight=16
block_request_timeout=30
max_block_retries=3
parallel_download_peers=4
```

---

## Known Limitations

1. **No Tor Support**: Connection limits don't account for Tor (all from same IP)
2. **No IPv6 /32 Subnets**: Eviction uses /16 for IPv4 only
3. **No BIP152 Compact Blocks**: Deferred to Phase 5B
4. **No Erlay**: Mempool reconciliation deferred to Phase 5G

These are acceptable for mainnet launch and can be added post-launch.

---

## Next Steps After Phase N

**Optional (Post-Mainnet):**
- **Phase 5B:** Compact Block Relay (BIP152)
- **Phase 5E:** Replace-By-Fee (RBF)
- **Phase 5F:** Child-Pays-For-Parent (CPFP)
- **Phase 5G:** Mempool Reconciliation (Erlay)

**Critical (Before Mainnet):**
- ✅ Phase N completion
- ✅ Integration testing
- ✅ Stress testing (1000 peers, 10K msg/sec)

---

**Status:** Ready to implement
**Estimated:** 2-3 weeks
**Risk:** LOW (builds on existing infrastructure)

---

**END OF PLAN**
