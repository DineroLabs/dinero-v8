# Phase G.1.4: Inventory Exchange (inv/getdata) - Implementation Plan

**Date:** 2025-12-17
**Status:** Planning
**Scope:** Minimal Bitcoin-style inventory exchange (pure networking, NO validation)

---

## Why Now?

**Historical progression (Bitcoin Core):**
1. ✅ Socket connections
2. ✅ version/verack handshake
3. ✅ Peer lifecycle management
4. ✅ ping/pong keepalive
5. **← inv/getdata (YOU ARE HERE)**
6. ⏭️ block/tx relay
7. ⏭️ headers-first sync

**This is the correct layering.**

---

## Why Dinero Benefits More Than Bitcoin Did

### 1. Utreexo / Compact State
**Problem:** Must be selective about what to request.

**inv/getdata enables:**
- Request headers only
- Delay block bodies
- Avoid downloading things you can't validate yet
- Request proofs separately from blocks

### 2. Multi-Asset & Lightning Roadmap
**Future announcements:**
- Blocks
- Transactions
- Channel updates
- Asset commitments

**inv allows:**
- "I have something of type X with hash Y"
- Without committing to sending it
- Recipient decides what to request

### 3. Deterministic Testing
**Your test harness is perfect for:**
- Who announces first
- Who requests what
- What happens when peers ignore inv
- What happens when peers spam inv

**Bitcoin Core bolted this on later — you won't.**

---

## What inv/getdata IS

**Gossip backbone of Bitcoin-style P2P networks.**

**Purpose:**
1. Announce availability of objects (blocks, transactions)
2. Request specific objects by hash
3. Handle "not found" cases
4. Enable selective propagation

**Key Properties:**
- Stateless (no validation coupling)
- Asynchronous (requests queued, not immediate)
- Selective (announce availability, don't push)
- DoS-resistant (bounded request queues)

---

## What inv/getdata is NOT

❌ **It is NOT:**
- Block validation
- Mempool logic
- Fork choice
- Consensus
- Transaction relay policy

**Those stay completely untouched.**

**This is still pure networking.**

---

## Minimal Correct Scope (Do NOT Overbuild)

### Messages to Implement

#### 1. `inv` (Inventory)
```cpp
struct InventoryVector {
    uint32_t type;      // MSG_BLOCK, MSG_TX, etc.
    uint256 hash;       // Object hash
};

struct InvMessage {
    std::vector<InventoryVector> inventory;
};
```

**Types:**
- `MSG_BLOCK` (1) - Block announcement
- `MSG_TX` (2) - Transaction announcement
- ~~MSG_FILTERED_BLOCK~~ (defer to G.3 - Bloom filters)
- ~~MSG_CMPCT_BLOCK~~ (defer to G.2 - Compact blocks)

#### 2. `getdata` (Get Data)
```cpp
struct GetDataMessage {
    std::vector<InventoryVector> inventory;  // Same format as inv
};
```

**Purpose:** Request objects announced via inv

#### 3. `notfound` (Not Found)
```cpp
struct NotFoundMessage {
    std::vector<InventoryVector> inventory;  // Objects we don't have
};
```

**Purpose:** Tell peer we don't have requested objects

### Behavior Rules (Simple, Bitcoin-Style)

#### Node Receives `inv`
```
1. Check local cache:
   - Already have it? → Ignore
   - Don't have it? → Maybe request (based on policy)

2. If requesting:
   - Add to getdata queue
   - Send getdata (batched, not immediate)

3. Track in-flight requests:
   - Hash → (Peer, Timestamp)
   - Prevent duplicate requests
   - Timeout stale requests (60 seconds)
```

#### Node Receives `getdata`
```
1. Check if we have the object:
   - Have it? → Send block/tx message
   - Don't have it? → Send notfound

2. Rate limit:
   - Max objects per peer per second
   - Prevent resource exhaustion
```

#### Node Receives `notfound`
```
1. Remove from in-flight tracking
2. Maybe request from different peer
3. Don't retry immediately (backoff)
```

---

## Critical Architectural Rule

**⚠️ inv/getdata must NEVER trigger validation directly**

**Correct flow:**
```
inv received → Queue for consideration
getdata sent → Queue request
block received → Queue for validation (separate thread)
```

**Validation happens elsewhere.**

**This keeps networking stateless and safe.**

---

## Implementation Steps

### Step 1: Message Structures (1 day)

**Files to create:**
- `include/p2p/inventory.h` - InventoryVector, message structures
- `src/p2p/inventory.cpp` - Serialization/deserialization

**What to implement:**
```cpp
namespace dinero::p2p {

// Inventory types
constexpr uint32_t MSG_TX = 1;
constexpr uint32_t MSG_BLOCK = 2;

struct InventoryVector {
    uint32_t type;
    uint256 hash;

    std::vector<uint8_t> serialize() const;
    static InventoryVector deserialize(const std::vector<uint8_t>& data, size_t& offset);
};

struct InvMessage {
    std::vector<InventoryVector> inventory;

    std::vector<uint8_t> serialize() const;
    static InvMessage deserialize(const std::vector<uint8_t>& data);
};

struct GetDataMessage {
    std::vector<InventoryVector> inventory;

    std::vector<uint8_t> serialize() const;
    static GetDataMessage deserialize(const std::vector<uint8_t>& data);
};

struct NotFoundMessage {
    std::vector<InventoryVector> inventory;

    std::vector<uint8_t> serialize() const;
    static NotFoundMessage deserialize(const std::vector<uint8_t>& data);
};

} // namespace dinero::p2p
```

### Step 2: In-Flight Request Tracking (1 day)

**Files to create:**
- `include/p2p/inventory_tracker.h`
- `src/p2p/inventory_tracker.cpp`

**What to implement:**
```cpp
class InventoryTracker {
public:
    // Track requested inventory
    void markRequested(const uint256& hash, const std::string& peer);
    void markReceived(const uint256& hash);

    // Query state
    bool isRequested(const uint256& hash) const;
    bool isRequestedFrom(const uint256& hash, const std::string& peer) const;

    // Timeout management
    void cleanupStaleRequests(std::chrono::seconds timeout);

    // DoS protection
    size_t getRequestCountFrom(const std::string& peer) const;

private:
    struct RequestInfo {
        std::string peer;
        std::chrono::steady_clock::time_point timestamp;
    };

    std::unordered_map<uint256, RequestInfo> in_flight_;
    std::mutex mutex_;

    static constexpr size_t MAX_REQUESTS_PER_PEER = 1000;
    static constexpr std::chrono::seconds REQUEST_TIMEOUT{60};
};
```

### Step 3: Message Handlers (1 day)

**Files to modify:**
- `src/daemon/p2p_manager.cpp` - Add inv/getdata/notfound handlers

**What to implement:**
```cpp
void P2PManager::handle_inv(const std::string& peer_address, const P2PMessage& message) {
    // Parse inv message
    InvMessage inv = InvMessage::deserialize(message.payload);

    // For each inventory item
    for (const auto& item : inv.inventory) {
        // Check if we already have it (via callback)
        if (have_object_callback_ && have_object_callback_(item.type, item.hash)) {
            continue;  // Already have it
        }

        // Check if already requested
        if (inventory_tracker_.isRequested(item.hash)) {
            continue;  // Already requesting from someone
        }

        // Queue for requesting (batched later)
        pending_requests_.push_back(item);
        inventory_tracker_.markRequested(item.hash, peer_address);
    }

    // Send batched getdata (not immediate)
    flush_pending_requests();
}

void P2PManager::handle_getdata(const std::string& peer_address, const P2PMessage& message) {
    // Parse getdata message
    GetDataMessage getdata = GetDataMessage::deserialize(message.payload);

    std::vector<InventoryVector> not_found;

    // For each requested item
    for (const auto& item : getdata.inventory) {
        // Check if we have it (via callback)
        if (get_object_callback_) {
            auto obj = get_object_callback_(item.type, item.hash);
            if (obj) {
                // Send the object
                send_object(peer_address, item.type, *obj);
            } else {
                not_found.push_back(item);
            }
        }
    }

    // Send notfound for missing items
    if (!not_found.empty()) {
        NotFoundMessage msg;
        msg.inventory = not_found;
        send_to_peer(peer_address, P2PMessage::create_notfound(msg));
    }
}

void P2PManager::handle_notfound(const std::string& peer_address, const P2PMessage& message) {
    // Parse notfound message
    NotFoundMessage notfound = NotFoundMessage::deserialize(message.payload);

    // For each not-found item
    for (const auto& item : notfound.inventory) {
        // Remove from in-flight tracking
        inventory_tracker_.markReceived(item.hash);  // Mark as "resolved"

        // Maybe request from different peer (via callback)
        if (retry_request_callback_) {
            retry_request_callback_(item.type, item.hash);
        }
    }
}
```

### Step 4: Callbacks (Separation of Concerns)

**Add to P2PManager:**
```cpp
class P2PManager {
public:
    // Callbacks (set by application layer, NOT P2P layer)
    using HaveObjectCallback = std::function<bool(uint32_t type, const uint256& hash)>;
    using GetObjectCallback = std::function<std::optional<std::vector<uint8_t>>(uint32_t type, const uint256& hash)>;
    using RetryRequestCallback = std::function<void(uint32_t type, const uint256& hash)>;

    void set_have_object_callback(HaveObjectCallback cb) { have_object_callback_ = cb; }
    void set_get_object_callback(GetObjectCallback cb) { get_object_callback_ = cb; }
    void set_retry_request_callback(RetryRequestCallback cb) { retry_request_callback_ = cb; }

private:
    HaveObjectCallback have_object_callback_;
    GetObjectCallback get_object_callback_;
    RetryRequestCallback retry_request_callback_;
};
```

**Why callbacks?**
- P2P layer doesn't know about blocks/transactions
- Application layer (blockchain, mempool) provides the logic
- Clean separation: networking vs. validation

### Step 5: Tests (2 days)

**Files to create:**
- `tests/p2p/test_inventory_exchange.cpp`

**Test scenarios:**

#### Test 1: Basic inv/getdata flow
```
1. Alice announces block to Bob (inv)
2. Bob requests block (getdata)
3. Alice sends block
4. Bob receives block
```

#### Test 2: Duplicate inv handling
```
1. Alice announces block to Bob
2. Charlie announces same block to Bob
3. Bob only requests once (from Alice)
4. Bob ignores Charlie's inv
```

#### Test 3: notfound handling
```
1. Alice announces block to Bob
2. Bob requests block
3. Alice doesn't have it (sends notfound)
4. Bob marks request as failed
```

#### Test 4: Request timeout
```
1. Alice announces block to Bob
2. Bob requests block
3. Alice doesn't respond
4. After 60 seconds, Bob's request times out
5. Bob can request from different peer
```

#### Test 5: Rate limiting
```
1. Malicious peer sends 10,000 inv messages
2. Bob's request queue is bounded
3. Bob doesn't exhaust memory
```

---

## What You Gain Immediately

Once inv/getdata exists:

1. **Simulate block propagation**
   - Test announcement latency
   - Test request batching
   - Test duplicate suppression

2. **Test spam resistance**
   - Bounded request queues
   - Timeout handling
   - Rate limiting

3. **Test partial connectivity graphs**
   - Who hears about blocks first
   - How announcements propagate
   - Dead-end detection

4. **Unlock headers-first sync later**
   - With no redesign needed
   - inv can announce headers
   - getdata can request headers

---

## Success Criteria

After Phase G.1.4:

- ✅ inv message serialization/deserialization
- ✅ getdata message serialization/deserialization
- ✅ notfound message serialization/deserialization
- ✅ In-flight request tracking (hash → peer + timestamp)
- ✅ Duplicate request suppression
- ✅ Request timeout handling (60 seconds)
- ✅ Rate limiting (bounded queues)
- ✅ Callback-based object lookup (no validation coupling)
- ✅ All 5 test scenarios passing

---

## Timeline

**Total: 5 days**

- Day 1: Message structures + serialization
- Day 2: In-flight request tracking
- Day 3: Message handlers + callbacks
- Day 4: Test scenarios 1-3
- Day 5: Test scenarios 4-5 + integration

---

## What's NOT Included (Explicitly Deferred)

**Deferred to later phases:**

1. **Block relay logic** (G.2)
   - Actual block sending
   - Block validation
   - Blockchain integration

2. **Transaction relay policy** (G.4)
   - Mempool integration
   - Fee-based relay
   - RBF handling

3. **Bloom filters** (G.3)
   - MSG_FILTERED_BLOCK type
   - SPV client support
   - Merkle proof construction

4. **Compact blocks** (G.2)
   - MSG_CMPCT_BLOCK type
   - BIP 152 implementation
   - Bandwidth optimization

---

## Architectural Guarantees

**What inv/getdata MUST maintain:**

1. **No validation coupling**
   - inv/getdata only queues requests
   - Validation happens elsewhere (via callbacks)
   - Networking stays stateless

2. **Bounded resource usage**
   - Max requests per peer: 1,000
   - Request timeout: 60 seconds
   - Cleanup stale requests periodically

3. **Deterministic testing**
   - All behavior testable via test harness
   - No hidden state
   - Clear message flow

4. **Clean separation**
   - P2P layer: announcements + requests
   - Application layer: "do I have this?" logic
   - Validation layer: "is this valid?" logic

---

## Clear Answer to Your Questions

**"What is inv/getdata?"**
→ The gossip backbone of Bitcoin-style P2P networks.

**"Would Dinero benefit from it?"**
→ Yes — fundamentally and long-term, especially with:
- Utreexo (selective requests)
- Lightning (channel updates)
- Multi-asset plans (asset announcements)
- Deterministic testing (spam resistance)

**"Is now the right time?"**
→ **Yes.** You are at the exact historical and architectural moment.

**"What's the minimal correct scope?"**
→ inv, getdata, notfound with MSG_BLOCK and MSG_TX types.
→ Callback-based object lookup (no validation coupling).
→ In-flight tracking with timeout and rate limiting.

---

## Next Steps After G.1.4

Once inv/getdata is complete:

1. **G.2: Block Propagation**
   - Use inv to announce blocks
   - Use getdata to request blocks
   - Integrate with blockchain validation

2. **G.3: Headers-First Sync**
   - Announce headers via inv
   - Request headers via getdata
   - Parallel header download

3. **G.4: Transaction Relay**
   - Announce transactions via inv
   - Request transactions via getdata
   - Integrate with mempool

**inv/getdata is the foundation for all of these.**

---

## Conclusion

**Phase G.1.4 is the right next step.**

This is:
- ✅ The correct historical progression
- ✅ The right architectural layer
- ✅ The perfect timing (test harness ready, no blockchain coupling)
- ✅ Foundational for Utreexo, Lightning, multi-asset
- ✅ Minimal and correct (no overbuild)

**After G.1.4, DineroCoin will have:**
- Complete gossip backbone
- Selective object request capability
- DoS-resistant announcement mechanism
- Foundation for all future P2P features

**This unlocks everything else without redesign.**

Let's implement it.
