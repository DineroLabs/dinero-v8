# Phase N.2 Step 2A: Timeout Tracking + Stall Detection - COMPLETE ✅

**Date**: 2025-12-21
**Status**: All implementations complete, tests passing

---

## What Was Implemented

### 1. Time Accounting Fields (PeerHeaderInfo)

Added Bitcoin Core-pattern timeout tracking to each peer:

```cpp
struct PeerHeaderInfo {
    // ... existing fields ...
    uint64_t sync_start_time;       // When sync started with this peer (ms)
    uint64_t timeout_deadline;      // When peer will be considered stalled (ms)
    uint32_t expected_headers_remaining; // Estimated headers remaining
    bool is_outbound;               // True if outbound connection (prefer for sync)
};
```

**Location**: `include/consensus/header_sync.h:65-90`

---

### 2. Bitcoin Core Timeout Constants

Replaced generic timeouts with exact Bitcoin Core values:

```cpp
// Bitcoin Core timeout constants (from net_processing.cpp)
static constexpr uint64_t HEADERS_DOWNLOAD_TIMEOUT_BASE_MS = 15 * 60 * 1000;  // 15 minutes
static constexpr uint64_t HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER_MS = 1;        // 1ms per header
```

**Formula**: `timeout = 15 minutes + (expected_headers * 1ms)`

**Rationale**:
- Prevents slow-drip DOS attacks
- Allows slow networks reasonable time
- Matches Bitcoin Core production behavior

**Location**: `include/consensus/header_sync.h:247-249`

---

### 3. New State: STALLED

Added explicit stalled state for observability:

```cpp
enum class HeaderSyncState {
    IDLE,
    REQUESTING_HEADERS,
    PROCESSING_HEADERS,
    STALLED,               // Active sync peer has stalled (timeout exceeded)
    CAUGHT_UP
};
```

**Location**: `include/consensus/header_sync.h:41-47`

---

### 4. Peer Switch Signal Mechanism

Implemented callback-based peer switching (policy separate from execution):

```cpp
enum class PeerSwitchReason {
    STALL_TIMEOUT,         // Peer exceeded header download timeout
    INVALID_HEADERS,       // Peer sent headers that failed validation
    PEER_DISCONNECT,       // Peer disconnected voluntarily
    SYNC_COMPLETE,         // Received <2000 headers, caught up with peer
    NO_PROGRESS           // Peer not making progress on sync
};

using PeerSwitchCallback = std::function<void(uint64_t old_peer_id, PeerSwitchReason reason)>;

void SetPeerSwitchCallback(PeerSwitchCallback callback);
```

**Key insight**: State machine emits **intent** to switch, P2P layer **executes** the switch.

**Location**: `include/consensus/header_sync.h:53-59, 212-228`

---

### 5. Timeout Calculation (Bitcoin Core Exact)

```cpp
uint64_t CalculateTimeout(uint32_t expected_headers) const {
    // Bitcoin Core formula: 15min + (expected_headers * 1ms)
    return HEADERS_DOWNLOAD_TIMEOUT_BASE_MS +
           (expected_headers * HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER_MS);
}
```

**Example**:
- Syncing 800,000 headers: timeout = 15 min + 800 sec ≈ 28 minutes
- Syncing 1,000 headers: timeout = 15 min + 1 sec ≈ 15 minutes
- Syncing 0 headers (caught up): timeout = 15 min

**Location**: `src/consensus/header_sync.cpp:456-460`

---

### 6. UpdateSyncTimeout() - Dynamic Recalculation

```cpp
void UpdateSyncTimeout(uint64_t peer_id) {
    // Calculate expected headers remaining
    uint32_t our_height = (best ? best->height : 0);
    uint32_t expected_headers = (info.best_height > our_height)
                              ? (info.best_height - our_height)
                              : 0;

    // Update timeout deadline
    info.expected_headers_remaining = expected_headers;
    uint64_t timeout_duration = CalculateTimeout(expected_headers);
    info.timeout_deadline = now + timeout_duration;
}
```

**Called when**:
- Starting sync with new peer
- Receiving headers (reset timeout, recalculate remaining)
- Switching peers

**Location**: `src/consensus/header_sync.cpp:462-487`

---

### 7. CheckForStall() - Timeout Detection

```cpp
bool CheckForStall(uint64_t now_ms) {
    if (active_sync_peer_ == 0) {
        return false;  // No active sync peer
    }

    const PeerHeaderInfo& info = /* get peer info */;

    // Check if timeout deadline exceeded
    if (info.timeout_deadline > 0 && now_ms > info.timeout_deadline) {
        return true;  // Peer has stalled
    }

    return false;
}
```

**Location**: `src/consensus/header_sync.cpp:489-509`

---

### 8. Stall Detection in Tick()

```cpp
void Tick() {
    uint64_t now = GetCurrentTimeMs();

    // Check for stalls in all states except IDLE/CAUGHT_UP
    if (state_ != HeaderSyncState::IDLE && state_ != HeaderSyncState::CAUGHT_UP) {
        if (CheckForStall(now)) {
            TransitionTo(HeaderSyncState::STALLED);

            // Only request peer switch if we have alternatives
            if (CountAvailablePeers() > 0) {
                RequestPeerSwitch(PeerSwitchReason::STALL_TIMEOUT);
                TransitionTo(HeaderSyncState::IDLE);
            }
            // else: keep current peer, no alternatives available
        }
    }

    // ... rest of state machine
}
```

**Bitcoin Core pattern**: Don't disconnect if no alternative peers available.

**Location**: `src/consensus/header_sync.cpp:40-94`

---

### 9. CountAvailablePeers() - Last Peer Protection

```cpp
size_t CountAvailablePeers() const {
    size_t count = 0;

    for (const auto& pair : peers_) {
        // Skip stalled or misbehaving peers
        if (info.is_stalled || info.is_misbehaving) {
            continue;
        }

        // Skip current sync peer
        if (pair.first == active_sync_peer_) {
            continue;
        }

        count++;
    }

    return count;
}
```

**Purpose**: Prevents disconnecting last peer (better slow peer than no peer).

**Location**: `src/consensus/header_sync.cpp:528-548`

---

### 10. Outbound Peer Preference

Updated SelectBestPeer() to prefer outbound connections:

```cpp
uint64_t SelectBestPeer() const {
    uint64_t best_peer = 0;
    uint32_t best_height = 0;
    bool best_is_outbound = false;

    for (const auto& pair : peers_) {
        // ...

        // Bitcoin Core pattern: prefer outbound peers for eclipse resistance
        if (info.best_height > best_height) {
            // Higher height always wins
            should_replace = true;
        } else if (info.best_height == best_height) {
            // Same height: prefer outbound over inbound
            if (info.is_outbound && !best_is_outbound) {
                should_replace = true;
            }
        }
    }

    return best_peer;
}
```

**Rationale**: Outbound connections initiated by node → harder to eclipse.

**Location**: `src/consensus/header_sync.cpp:177-212`

---

### 11. ProcessHeaders() Integration

Updated header processing to reset timeout when headers arrive:

```cpp
bool ProcessHeaders(uint64_t peer_id, const std::vector<BlockHeader>& headers) {
    // ...

    // Update peer's last response time and timeout deadline
    peer_it->second.last_response_time = GetCurrentTimeMs();
    if (peer_id == active_sync_peer_) {
        UpdateSyncTimeout(peer_id);  // Reset timeout - peer is still responding
    }

    // ...

    if (headers.size() >= MAX_HEADERS_PER_MSG) {
        // Full batch - request more
        UpdateSyncTimeout(peer_id);  // Recalculate for remaining headers
    } else {
        // Partial batch - caught up
        RequestPeerSwitch(PeerSwitchReason::SYNC_COMPLETE);
    }
}
```

**Bitcoin Core pattern**:
- Reset timeout on each batch received
- Signal SYNC_COMPLETE for verification with other peers

**Location**: `src/consensus/header_sync.cpp:218-292`

---

## Key Design Decisions

### 1. Policy vs Execution Separation

**What we did**: State machine emits `RequestPeerSwitch(reason)`, doesn't execute it

**Why**:
- State machine testable without sockets
- P2P layer handles actual network disconnect
- Clean separation of concerns

### 2. Timeout Recalculation

**What we did**: Recalculate timeout after every batch received

**Why**:
- Expected headers decreases as sync progresses
- Prevents false timeouts near end of sync
- Matches Bitcoin Core behavior

### 3. Last Peer Protection

**What we did**: Check `CountAvailablePeers() > 0` before disconnecting

**Why**:
- Better slow peer than no peer
- Prevents getting stuck with zero peers
- Bitcoin Core production pattern

### 4. Outbound Preference

**What we did**: Prefer outbound peers when heights are equal

**Why**:
- Eclipse attack resistance
- Outbound = we initiated → not controlled by attacker
- Bitcoin Core security pattern

---

## What This Unlocks

With timeout tracking + stall detection complete, we now have:

✅ **Pure logic** timeout enforcement (no network dependencies)
✅ **Bitcoin Core-exact** timeout formula (15min + 1ms/header)
✅ **Stall detection** with last-peer protection
✅ **Peer switch signals** (intent, not execution)
✅ **Outbound peer preference** (eclipse resistance)
✅ **Dynamic timeout recalculation** (as sync progresses)

---

## Next: Phase N.2 Step 2B

**Stall & Lie Simulation Tests**

Before wiring P2P messages, we need tests that simulate:

1. **Peer advertises height 500k, sends 10 headers, then stalls**
   - Verify timeout triggers after 15min + 10ms
   - Verify RequestPeerSwitch(STALL_TIMEOUT) called
   - Verify transition to STALLED → IDLE

2. **Peer sends headers too slowly (one every 10 seconds)**
   - Should NOT timeout if making progress
   - Should timeout if no progress for full timeout period

3. **Peer switches fork mid-sync**
   - Invalid headers → RequestPeerSwitch(INVALID_HEADERS)
   - Already-accepted headers remain valid

4. **No alternative peers available**
   - Should NOT disconnect stalled peer
   - Should remain in STALLED state

5. **Timeout recalculation**
   - Receive 100k headers, verify timeout recalculates for remaining
   - Next batch should have shorter timeout

6. **Outbound vs inbound preference**
   - Two peers, same height, one outbound → outbound wins
   - Two peers, inbound has higher height → inbound wins

---

## Test Status

**Existing tests**: ✅ All 7 state machine tests still pass
**New tests needed**: Stall simulation tests (Phase N.2 Step 2B)

---

## Files Modified

1. `include/consensus/header_sync.h`
   - Added `STALLED` state
   - Added `PeerSwitchReason` enum
   - Added time accounting fields to `PeerHeaderInfo`
   - Added peer switch callback mechanism
   - Added timeout calculation methods

2. `src/consensus/header_sync.cpp`
   - Implemented `CalculateTimeout()`
   - Implemented `UpdateSyncTimeout()`
   - Implemented `CheckForStall()`
   - Implemented `RequestPeerSwitch()`
   - Implemented `CountAvailablePeers()`
   - Updated `Tick()` with stall detection
   - Updated `SelectBestPeer()` with outbound preference
   - Updated `ProcessHeaders()` with timeout reset
   - Added `MarkPeerOutbound()`

---

## References

1. [Bitcoin Core HEADERS_DOWNLOAD_TIMEOUT_BASE](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp)
2. [Bitcoin Core PR Review #25720](https://bitcoincore.reviews/25720)
3. [Bitcoin Core PR Review #25880 - Adaptive timeout](https://bitcoincore.reviews/25880)
4. [Bitcoin Core PR #10345 - Timeout for headers sync](https://github.com/bitcoin/bitcoin/pull/10345)
5. [BITCOIN_CORE_HEADER_SYNC_PATTERNS.md](./BITCOIN_CORE_HEADER_SYNC_PATTERNS.md)
