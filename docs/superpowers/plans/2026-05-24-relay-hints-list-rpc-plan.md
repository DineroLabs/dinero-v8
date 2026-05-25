# Relay Hints List + 24h Counters Implementation Plan (Phase 2b daemon)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development.

**Goal:** Land the daemon-side RPC surface that Phase 2b qt work consumes (`relay_hints.list` method + 2 new fields on `getnetworkinfo.relay`).

**Architecture:** New RPC handler header/impl pair in the same shape as PR #140's `rpc_dynamic_p2p_handlers.{h,cpp}`. Two 24h rolling-bucket counters added: one to `BlockRelayManager` (block sends), one to `P2pManager` (relay-virtual bytes). Counters use the existing `ClockSource` for test determinism.

**Tech Stack:** C++20, `din::Json` / `Json::Value`, `std::atomic` + `std::mutex` for bucket rotation, existing `ClockSource` for time.

**Branch:** `feature/relay-hints-list-rpc` off `dinero-main`. Worktree: `/private/tmp/dinero-v8-phase2b-daemon`.

**Signing:** All commits SSH-signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File map

- Create: `include/network/rolling_24h_counter.h` (~80 lines) — reusable hourly-bucket counter class
- Create: `tests/network/test_rolling_24h_counter.cpp` (~120 lines)
- Create: `include/rpc/rpc_relay_hints_handlers.h` (~25 lines)
- Create: `src/rpc/rpc_relay_hints_handlers.cpp` (~130 lines)
- Create: `tests/network/test_relay_hints_list_rpc.sh` (~110 lines)
- Modify: `include/daemon/block_relay_manager.h` — add Rolling24hCounter member + accessor
- Modify: `src/daemon/block_relay_manager.cpp` — bump counter inside `HandleGetData` after successful block send
- Modify: `src/daemon/p2p_manager.h` — add Rolling24hCounter for relay-virtual bytes + accessor + RecordRelayBytes hook
- Modify: `src/daemon/p2p_manager.cpp` — call RecordRelayBytes from the 2 relay-virtual send/recv funnels
- Modify: `src/daemon/rpc_context_wiring.cpp` — register relay_hints.list method
- Modify: `src/rpc/methods_network_context.cpp` — add 2 new fields to getnetworkinfo.relay
- Modify: `CMakeLists.txt` — add new rpc handler source
- Modify: `tests/CMakeLists.txt` — register new tests

Total: ~450 LOC. Comparable to PR #140.

---

## Task 1: Rolling 24h counter primitive + unit tests

**Files:**
- Create: `include/network/rolling_24h_counter.h`
- Create: `tests/network/test_rolling_24h_counter.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/network/test_rolling_24h_counter.cpp
#include "network/rolling_24h_counter.h"
#include "network/clock_source.h"
#include <gtest/gtest.h>
#include <memory>

using dinero::network::FakeClockSource;
using dinero::network::Rolling24hCounter;

TEST(Rolling24hCounter, EmptyReturnsZero) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    EXPECT_EQ(c.Total24h(), 0u);
}

TEST(Rolling24hCounter, SingleHourAccumulates) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    c.Add(5);
    c.Add(7);
    EXPECT_EQ(c.Total24h(), 12u);
}

TEST(Rolling24hCounter, OldHourBucketsRotateOut) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    c.Add(10);                                                          // hour 0
    clk.Advance(std::chrono::hours(23));
    c.Add(5);                                                           // hour 23
    EXPECT_EQ(c.Total24h(), 15u);
    clk.Advance(std::chrono::hours(2));                                 // hour 25
    c.Add(1);                                                           // hour 25 → bucket 1 (wraparound)
    // The hour-0 record is now 25h old, must be excluded. The hour-23
    // record is 2h old, must remain. The hour-25 record is fresh.
    EXPECT_EQ(c.Total24h(), 6u);
}

TEST(Rolling24hCounter, WraparoundClearsStaleBucket) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    c.Add(100);                                                         // hour 0, bucket 0
    clk.Advance(std::chrono::hours(24));
    c.Add(1);                                                           // hour 24, bucket 0 again (wraparound)
    // The hour-0 record must be cleared (24h+ old). Only the new hour-24 add stays.
    EXPECT_EQ(c.Total24h(), 1u);
}

TEST(Rolling24hCounter, ThreadSafeUnderConcurrentIncrement) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    constexpr int kThreads = 8;
    constexpr int kIncrementsPerThread = 10000;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            for (int j = 0; j < kIncrementsPerThread; ++j) c.Add(1);
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(c.Total24h(),
              static_cast<uint64_t>(kThreads) * kIncrementsPerThread);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /private/tmp/dinero-v8-phase2b-daemon
cmake -S . -B build-p2b 2>&1 | tail -5
cmake --build build-p2b --target test_rolling_24h_counter -j8 2>&1 | tail -5
```

Expected: link/compile error — `rolling_24h_counter.h` doesn't exist yet.

- [ ] **Step 3: Write the header**

```cpp
// include/network/rolling_24h_counter.h
#pragma once

#include "network/clock_source.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace dinero::network {

// 24-hour rolling event counter, hourly bucket granularity. Total24h()
// returns the sum across the trailing 24 hourly buckets. Old buckets
// are zeroed on first touch from a new hour.
//
// Thread-safe: Add() is lock-free in the hot path (atomic increment on
// the current bucket); bucket rotation acquires a mutex.
//
// Time source is injectable for test determinism. SystemClockSource for
// production; FakeClockSource for tests.
class Rolling24hCounter {
public:
    explicit Rolling24hCounter(const ClockSource* clock)
        : clock_(clock), last_touched_hour_index_(SystemHourIndex()) {}

    void Add(uint64_t delta) {
        const uint64_t hour_index = SystemHourIndex();
        const size_t bucket = hour_index % kBuckets;
        RotateIfHourCrossed(hour_index);
        buckets_[bucket].fetch_add(delta, std::memory_order_relaxed);
    }

    uint64_t Total24h() const {
        const uint64_t hour_index = SystemHourIndex();
        // Lazy rotation on read so a counter that's been quiet for >24h
        // returns 0 instead of stale.
        const_cast<Rolling24hCounter*>(this)->RotateIfHourCrossed(hour_index);
        uint64_t total = 0;
        for (auto& b : buckets_) {
            total += b.load(std::memory_order_relaxed);
        }
        return total;
    }

private:
    static constexpr size_t kBuckets = 24;

    uint64_t SystemHourIndex() const {
        const auto now = clock_->SystemNow();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::hours>(
                now.time_since_epoch()).count());
    }

    void RotateIfHourCrossed(uint64_t hour_index) {
        std::lock_guard<std::mutex> lock(rotation_mutex_);
        if (hour_index == last_touched_hour_index_) return;
        const uint64_t span = hour_index - last_touched_hour_index_;
        if (span >= kBuckets) {
            // Counter has been quiet for ≥24h — clear all buckets.
            for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
        } else {
            // Clear the [span] buckets that wrapped past the trailing 24h.
            for (uint64_t i = 1; i <= span; ++i) {
                const size_t idx = (last_touched_hour_index_ + i) % kBuckets;
                buckets_[idx].store(0, std::memory_order_relaxed);
            }
        }
        last_touched_hour_index_ = hour_index;
    }

    const ClockSource* clock_;
    std::array<std::atomic<uint64_t>, kBuckets> buckets_{};
    mutable std::mutex rotation_mutex_;
    uint64_t last_touched_hour_index_;
};

}  // namespace dinero::network
```

- [ ] **Step 4: Register test target in tests/CMakeLists.txt**

Search for the `test_dashboard_rpcs` target added in PR #140 and add alongside:

```cmake
add_executable(test_rolling_24h_counter network/test_rolling_24h_counter.cpp)
target_link_libraries(test_rolling_24h_counter PRIVATE dinero_consensus gtest gtest_main)
target_include_directories(test_rolling_24h_counter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME RollingCounter24h COMMAND test_rolling_24h_counter)
```

(Use the existing test target pattern — if PR #140's test uses different libs, match exactly.)

- [ ] **Step 5: Run tests**

```bash
cmake --build build-p2b --target test_rolling_24h_counter -j8 2>&1 | tail -5
cd build-p2b && ctest -R RollingCounter24h --output-on-failure 2>&1 | tail -10
```

Expected: 5/5 PASS.

- [ ] **Step 6: Commit**

```bash
git add include/network/rolling_24h_counter.h \
        tests/network/test_rolling_24h_counter.cpp \
        tests/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(network): add Rolling24hCounter primitive with hourly buckets

Reusable 24-hour rolling event counter, hourly bucket granularity. The
hot path (Add) is a single atomic increment on the current bucket; only
hour-boundary rotation takes the mutex. ClockSource is injected for
test determinism.

5 gtest cases cover empty, single-hour accumulation, multi-hour
rotation, full-24h wraparound clears the stale bucket, and 8-thread
concurrent increment stays consistent.

Will be used by BlockRelayManager (blocks_served_24h) and P2pManager
(bytes_relayed_24h) for the Phase 2b dashboard surface.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: BlockRelayManager blocks_served_24h counter

**Files:**
- Modify: `include/daemon/block_relay_manager.h`
- Modify: `src/daemon/block_relay_manager.cpp`

- [ ] **Step 1: Read existing BlockRelayManager**

```bash
grep -n "class BlockRelayManager\|HandleGetData\|public:\|private:" include/daemon/block_relay_manager.h
sed -n '220,260p' src/daemon/block_relay_manager.cpp
```

- [ ] **Step 2: Add the counter to the header**

In `include/daemon/block_relay_manager.h`, add include:

```cpp
#include "network/clock_source.h"
#include "network/rolling_24h_counter.h"
```

In the public section, add accessor:

```cpp
    // Phase 2b — rolling 24h count of successful block sends in response
    // to inv/getdata. Surfaced via getnetworkinfo.relay.blocks_served_24h.
    uint64_t BlocksServed24h() const { return blocks_served_24h_.Total24h(); }
```

In the constructor parameter list (or member init), accept a `const dinero::network::ClockSource* clock`. If the existing constructor doesn't already take one, add it (default to a long-lived `SystemClockSource` singleton owned at construction site). Add member:

```cpp
    dinero::network::Rolling24hCounter blocks_served_24h_;
```

Initialize in constructor:

```cpp
    BlockRelayManager(..., const dinero::network::ClockSource* clock)
        : ..., blocks_served_24h_(clock) {}
```

- [ ] **Step 3: Bump the counter inside HandleGetData on successful send**

In `src/daemon/block_relay_manager.cpp::HandleGetData`, locate the point AFTER `send_message_callback_` is invoked successfully with the serialized block (after the existing `"Block retrieved successfully"` log around line 305). Add:

```cpp
    blocks_served_24h_.Add(1);
```

- [ ] **Step 4: Wire the clock-source dependency at construction site**

Find where `BlockRelayManager` is instantiated (likely `daemon_app.cpp` or `rpc_context_wiring.cpp`). Pass an existing ClockSource pointer (the one already wired into `P2pManager` from RELAY_HINTS work — pass the same one for consistency).

- [ ] **Step 5: Compile**

```bash
cd /private/tmp/dinero-v8-phase2b-daemon
cmake --build build-p2b --target dinerod -j8 2>&1 | tail -15
```

Expected: clean compile.

- [ ] **Step 6: Commit**

```bash
git add include/daemon/block_relay_manager.h src/daemon/block_relay_manager.cpp \
        src/daemon/daemon_app.cpp  # or wherever the ctor is called
git commit -S -m "$(cat <<'EOF'
feat(daemon): BlockRelayManager tracks blocks_served_24h

Rolling 24h count of successful block sends in response to inv/getdata
peer requests. Surfaced in the next commit via getnetworkinfo.relay.

Counter increments inside HandleGetData after the send callback fires
on a retrieved block. ClockSource is injected for test determinism.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: P2pManager bytes_relayed_24h counter

**Files:**
- Modify: `src/daemon/p2p_manager.h`
- Modify: `src/daemon/p2p_manager.cpp`

- [ ] **Step 1: Locate the relay-virtual data funnels**

```bash
grep -n "send_relay_data_to_virtual_peer\|handle_relay_data\|relay_virtual" src/daemon/p2p_manager.cpp | head -10
```

Identify the 2 call sites: the outbound funnel (`send_relay_data_to_virtual_peer`) and the inbound decode funnel.

- [ ] **Step 2: Add counter to header**

In `src/daemon/p2p_manager.h`, add include `"network/rolling_24h_counter.h"` near the existing `"network/clock_source.h"`. Add public accessor:

```cpp
    uint64_t BytesRelayed24h() const { return bytes_relayed_24h_.Total24h(); }
```

Add private member (alongside `clock_`):

```cpp
    dinero::network::Rolling24hCounter bytes_relayed_24h_;
```

Initialize in constructor: `..., bytes_relayed_24h_(clock_.get()), ...`. Verify the construction order is safe (clock_ initialized first).

- [ ] **Step 3: Add a private helper + call from 2 funnels**

In the public/private section appropriate to the class, add:

```cpp
    // Phase 2b — call from the 2 relay-virtual funnels to track total
    // bytes carried over relay-virtual peers in the trailing 24h.
    void RecordRelayBytes(uint64_t bytes) {
        bytes_relayed_24h_.Add(bytes);
    }
```

Then in `src/daemon/p2p_manager.cpp`:
- In `send_relay_data_to_virtual_peer`, after the successful send, add `RecordRelayBytes(payload.size());` (use the actual payload-size variable from context).
- In the inbound decode path (find via grep), add the same call with the decoded payload's byte count.

- [ ] **Step 4: Compile**

```bash
cmake --build build-p2b --target dinerod -j8 2>&1 | tail -15
```

- [ ] **Step 5: Commit**

```bash
git add src/daemon/p2p_manager.h src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
feat(daemon): P2pManager tracks bytes_relayed_24h

Rolling 24h total of bytes carried over relay-virtual peers, both
directions. Counted at the 2 relay-virtual funnels (send/recv).
Surfaced in the next commit via getnetworkinfo.relay.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: relay_hints.list RPC handler

**Files:**
- Create: `include/rpc/rpc_relay_hints_handlers.h`
- Create: `src/rpc/rpc_relay_hints_handlers.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Header**

```cpp
// include/rpc/rpc_relay_hints_handlers.h
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "din_json.h"

namespace dinero { class P2PService; }

namespace dinero::rpc {

// Implements the relay_hints.list JSON-RPC method.
//
// Returns the contents of P2pManager::relay_hints_by_target_ as
//   { "targets": [ { target_node_id_hex, endpoints: [...] }, ... ],
//     "total_targets": N, "ttl_seconds": ..., "max_failures": ... }
//
// Empty cache returns total_targets:0 + targets:[]; never errors.
din::Json HandleRelayHintsList(dinero::P2PService* p2p_service);

}  // namespace dinero::rpc
```

- [ ] **Step 2: Implementation**

```cpp
// src/rpc/rpc_relay_hints_handlers.cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/rpc_relay_hints_handlers.h"

#include "daemon/services/p2p_service.h"
#include "daemon/p2p_manager.h"
#include "p2p/network_type.h"

#include <chrono>
#include <vector>

namespace dinero::rpc {

namespace {

const char* NetworkTypeToString(dinero::p2p::NetworkType n) {
    switch (n) {
        case dinero::p2p::NetworkType::IPV4: return "ipv4";
        case dinero::p2p::NetworkType::IPV6: return "ipv6";
        default:                             return "unknown";
    }
}

std::string EncodeAddr(dinero::p2p::NetworkType net,
                       const std::vector<uint8_t>& bytes) {
    // Use the same canonical text encoding the dashboard already expects:
    // IPv4 dotted-quad, IPv6 colon-hex. P2pManager::FormatAddr() (if it
    // exists) is preferred; fall back to a hand-rolled formatter.
    if (net == dinero::p2p::NetworkType::IPV4 && bytes.size() == 4) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      bytes[0], bytes[1], bytes[2], bytes[3]);
        return buf;
    }
    // (IPv6 formatter — same approach as dinerod's existing peer-addr
    // string serializer; verify the helper name in p2p_manager.cpp.)
    return "";  // implementer: wire to the existing helper instead
}

}  // namespace

din::Json HandleRelayHintsList(dinero::P2PService* p2p_service) {
    din::Json out;
    out["rpc_schema"]    = "din.rpc.v1";
    out["targets"]       = din::Json(Json::arrayValue);
    out["total_targets"] = 0;
    out["ttl_seconds"]   = 0;
    out["max_failures"]  = 0;

    if (!p2p_service) return out;
    auto* p2p = p2p_service->Manager();   // verify method name vs PR #140 pattern
    if (!p2p) return out;

    const auto snapshot = p2p->SnapshotRelayHintsForRpc();   // new helper, Task 4 step 3
    out["ttl_seconds"]  = static_cast<Json::UInt>(snapshot.ttl_seconds);
    out["max_failures"] = snapshot.max_failures;

    for (const auto& entry : snapshot.entries) {
        din::Json target;
        target["target_node_id_hex"] = entry.target_hex;
        din::Json endpoints(Json::arrayValue);
        for (const auto& ep : entry.endpoints) {
            din::Json e;
            e["net"]            = NetworkTypeToString(ep.net);
            e["addr"]           = EncodeAddr(ep.net, ep.addr);
            e["port"]           = ep.port;
            e["age_seconds"]    = static_cast<Json::UInt>(ep.age_seconds);
            e["dial_failures"]  = ep.dial_failures;
            e["near_eviction"]  = ep.near_eviction;
            endpoints.append(e);
        }
        target["endpoints"] = endpoints;
        out["targets"].append(target);
    }
    out["total_targets"] = static_cast<Json::UInt>(snapshot.entries.size());
    return out;
}

}  // namespace dinero::rpc
```

- [ ] **Step 3: Add the snapshot helper to P2pManager**

In `src/daemon/p2p_manager.h` (public section), add:

```cpp
    struct RelayHintRpcEndpoint {
        dinero::p2p::NetworkType net;
        std::vector<uint8_t>     addr;
        uint16_t                 port;
        uint64_t                 age_seconds;
        int                      dial_failures;
        bool                     near_eviction;
    };
    struct RelayHintRpcTarget {
        std::string                          target_hex;
        std::vector<RelayHintRpcEndpoint>    endpoints;
    };
    struct RelayHintRpcSnapshot {
        std::vector<RelayHintRpcTarget>      entries;
        uint64_t                             ttl_seconds;
        int                                  max_failures;
    };
    RelayHintRpcSnapshot SnapshotRelayHintsForRpc() const;
```

In `src/daemon/p2p_manager.cpp`:

```cpp
RelayHintRpcSnapshot P2pManager::SnapshotRelayHintsForRpc() const {
    RelayHintRpcSnapshot out;
    out.ttl_seconds = std::chrono::duration_cast<std::chrono::seconds>(kHintTtl).count();
    out.max_failures = kHintMaxFailures;
    const auto now = clock_->SteadyNow();
    std::lock_guard<std::mutex> lock(relay_hints_mutex_);
    out.entries.reserve(relay_hints_by_target_.size());
    for (const auto& [target_hex, records] : relay_hints_by_target_) {
        RelayHintRpcTarget t;
        t.target_hex = target_hex;
        t.endpoints.reserve(records.size());
        for (const auto& r : records) {
            RelayHintRpcEndpoint ep;
            ep.net          = r.net;
            ep.addr         = r.relay_addr;
            ep.port         = r.relay_port;
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - r.learned_at).count();
            ep.age_seconds  = age < 0 ? 0 : static_cast<uint64_t>(age);
            ep.dial_failures = r.consecutive_dial_failures;
            const bool age_critical = ep.age_seconds * 10 >= out.ttl_seconds * 8;
            const bool fail_critical = r.consecutive_dial_failures >= kHintMaxFailures - 1;
            ep.near_eviction = age_critical || fail_critical;
            t.endpoints.push_back(std::move(ep));
        }
        out.entries.push_back(std::move(t));
    }
    return out;
}
```

- [ ] **Step 4: Register in rpc_context_wiring.cpp**

```bash
grep -n "dynamic_p2p.observe\|RegisterRpcMethod\|dispatch" src/daemon/rpc_context_wiring.cpp | head -10
```

Add the new method registration alongside the dynamic_p2p.observe registration that PR #140 added. Match the exact registration shape used there.

- [ ] **Step 5: Add the new cpp to CMake**

In `CMakeLists.txt`, find `rpc_dynamic_p2p_handlers.cpp` and add `src/rpc/rpc_relay_hints_handlers.cpp` to the same source list.

- [ ] **Step 6: Compile + smoke**

```bash
cmake --build build-p2b --target dinerod -j8 2>&1 | tail -10
ls -la build-p2b/dinerod 2>&1 | head -3
```

- [ ] **Step 7: Commit**

```bash
git add include/rpc/rpc_relay_hints_handlers.h \
        src/rpc/rpc_relay_hints_handlers.cpp \
        src/daemon/rpc_context_wiring.cpp \
        src/daemon/p2p_manager.h src/daemon/p2p_manager.cpp \
        CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(rpc): expose relay_hints.list for the dashboard DiscoverySection

New JSON-RPC method returning the contents of P2pManager's hint cache
as a structured list (one entry per target node id, up to 4 endpoint
records each). Each endpoint includes age_seconds, dial_failures, and a
near_eviction flag that fires when age ≥ 0.8×TTL OR failures reach
kHintMaxFailures-1.

The dashboard polls this every 5s to render the freshness bar +
stoplight glyph + fade-on-evict UX from the design spec.

P2pManager gains a public SnapshotRelayHintsForRpc() that takes the
relay_hints_mutex_ once, copies into a value-type snapshot, releases.
JSON construction happens lock-free outside the snapshot.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Surface 2 new fields on getnetworkinfo.relay

**Files:**
- Modify: `src/rpc/methods_network_context.cpp`

- [ ] **Step 1: Locate the relay sub-object builder**

```bash
grep -n "\"relay\"\|relay_obj\|relay\\..*active\\|registrants_count" src/rpc/methods_network_context.cpp
```

- [ ] **Step 2: Add the 2 new fields**

In the builder that constructs `out["relay"]`, after `registrants_count`:

```cpp
    relay["blocks_served_24h"] = static_cast<Json::UInt>(
        block_relay_manager->BlocksServed24h());
    relay["bytes_relayed_24h"] = static_cast<Json::UInt64>(
        p2p_manager->BytesRelayed24h());
```

Ensure `block_relay_manager` and `p2p_manager` pointers are already in scope (they should be; if not, plumb them through the existing context struct).

- [ ] **Step 3: Compile**

```bash
cmake --build build-p2b --target dinerod -j8 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add src/rpc/methods_network_context.cpp
git commit -S -m "$(cat <<'EOF'
feat(rpc): surface blocks_served_24h + bytes_relayed_24h on getnetworkinfo.relay

Two additive uint64 fields. The dashboard's Decentralization Score uses
both as real inputs in place of the Phase 2a proxies (which extrapolated
from 5min sparkline samples and hard-coded zero, respectively).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Shell integration test

**Files:**
- Create: `tests/network/test_relay_hints_list_rpc.sh`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test**

Model exactly on `tests/network/test_dashboard_rpcs.sh` (PR #140). Pseudocode:

```bash
#!/usr/bin/env bash
# tests/network/test_relay_hints_list_rpc.sh
set -euo pipefail

DINEROD=${DINEROD:?must be set by ctest wrapper to $<TARGET_FILE:dinerod>}
DINERO_CLI=${DINERO_CLI:?must be set by ctest wrapper to $<TARGET_FILE:dinero-cli>}

TMPDIR=$(mktemp -d)
trap "kill 0 2>/dev/null; rm -rf $TMPDIR" EXIT

# Spin up regtest A (relay), B (target), C (origin)
# Wait for handshake + RELAY_HINTS exchange (~5s)
# Call relay_hints.list on C
RESULT=$("$DINERO_CLI" -regtest -datadir=$TMPDIR/C relay_hints.list)
echo "$RESULT" | jq -e '.targets | length >= 1'
echo "$RESULT" | jq -e '.targets[0].endpoints[0].dial_failures == 0'
echo "$RESULT" | jq -e '.targets[0].endpoints[0].age_seconds < 30'
echo "$RESULT" | jq -e '.targets[0].endpoints[0].near_eviction == false'

# Mine + serve a block; assert blocks_served_24h advances
BEFORE=$("$DINERO_CLI" -regtest -datadir=$TMPDIR/A getnetworkinfo | jq -r .relay.blocks_served_24h)
"$DINERO_CLI" -regtest -datadir=$TMPDIR/A generatetoaddress 1 ...
sleep 2
AFTER=$("$DINERO_CLI" -regtest -datadir=$TMPDIR/A getnetworkinfo | jq -r .relay.blocks_served_24h)
test "$AFTER" -gt "$BEFORE"

echo "PASS"
```

(Implementer: model the regtest spin-up boilerplate on `test_dashboard_rpcs.sh` — same datadir setup, same `addnode` glue.)

- [ ] **Step 2: Register in tests/CMakeLists.txt with DINEROD env injection**

Per the project rule (memory: "Graduated tests need DINEROD env injection"):

```cmake
add_test(NAME RelayHintsListRpc
         COMMAND ${CMAKE_COMMAND} -E env
                 "DINEROD=$<TARGET_FILE:dinerod>"
                 "DINERO_CLI=$<TARGET_FILE:dinero-cli>"
                 ${CMAKE_CURRENT_SOURCE_DIR}/network/test_relay_hints_list_rpc.sh)
set_tests_properties(RelayHintsListRpc PROPERTIES
    LABELS "network;rpc;dashboard"
    TIMEOUT 60)
```

- [ ] **Step 3: Run**

```bash
cmake --build build-p2b --target dinerod dinero-cli -j8 2>&1 | tail -5
cd build-p2b && ctest -R RelayHintsListRpc --output-on-failure 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```bash
git add tests/network/test_relay_hints_list_rpc.sh tests/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(rpc): integration test for relay_hints.list + blocks_served_24h

3-node regtest scenario (relay A, target B, origin C). Asserts C's
relay_hints.list returns a fresh entry for B's node_id and that A's
blocks_served_24h advances after generatetoaddress.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Full daemon build + ctest

**Files:** none (verification).

- [ ] **Step 1: Full build**

```bash
cmake --build build-p2b -j8 2>&1 | tail -5
```

- [ ] **Step 2: Run dashboard + rpc + network labeled ctest**

```bash
cd build-p2b && ctest -L "dashboard|rpc|network" --output-on-failure 2>&1 | tail -25
```

- [ ] **Step 3: Sanity-check CLI manually**

```bash
TMPDIR=$(mktemp -d)
build-p2b/dinerod -regtest -datadir=$TMPDIR -daemon -rpcuser=u -rpcpassword=p
sleep 3
build-p2b/dinero-cli -regtest -datadir=$TMPDIR -rpcuser=u -rpcpassword=p relay_hints.list
build-p2b/dinero-cli -regtest -datadir=$TMPDIR -rpcuser=u -rpcpassword=p getnetworkinfo | jq .relay
build-p2b/dinero-cli -regtest -datadir=$TMPDIR -rpcuser=u -rpcpassword=p stop
sleep 2
rm -rf $TMPDIR
```

Expected: `relay_hints.list` returns `{"targets": [], "total_targets": 0, "ttl_seconds": 900, "max_failures": 3}`. `getnetworkinfo.relay` shows `blocks_served_24h: 0` and `bytes_relayed_24h: 0`.

- [ ] **Step 4: Commit sanity log**

```bash
cat > docs/superpowers/plans/2026-05-24-relay-hints-list-rpc-sanity.md << EOF
# Relay Hints List RPC — Sanity Log

**Date:** $(date -u +%FT%TZ)
**Branch:** \`feature/relay-hints-list-rpc\`

| Check | Result |
|---|---|
| Full dinerod build | PASS |
| ctest -L dashboard\|rpc\|network | PASS |
| Standalone CLI: relay_hints.list returns valid shape on empty cache | PASS |
| Standalone CLI: getnetworkinfo.relay has new fields | PASS |
EOF
git add docs/superpowers/plans/2026-05-24-relay-hints-list-rpc-sanity.md
git commit -S -m "docs: relay_hints.list RPC sanity log

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Push + draft PR

```bash
git push -u origin feature/relay-hints-list-rpc
gh pr create --draft --title "feat(rpc): relay_hints.list + 24h relay counters for dashboard Phase 2b" --body "$(cat <<'EOF'
## Summary

Adds the daemon-side RPC surface that [MyNodeDashboard Phase 2b qt PR](TBD) consumes:

1. New method **`relay_hints.list`** — returns the per-target relay-hint cache for the new DiscoverySection
2. New field **`getnetworkinfo.relay.blocks_served_24h`** — rolling 24h block-send counter from BlockRelayManager
3. New field **`getnetworkinfo.relay.bytes_relayed_24h`** — rolling 24h relay-virtual byte counter from P2pManager

Reusable `Rolling24hCounter` primitive (24 atomic hourly buckets, mutex-guarded rotation) shipped in `include/network/`.

## What's NOT in this PR

- Per-source-peer tracking inside RelayHintRecord (would require a RELAY_HINTS protocol/wire change — out of scope)
- `relayhints.dial` action RPC — Phase 3
- 24h counter persistence across restart — counters are in-memory only

## Test plan

- [x] 5 unit cases for Rolling24hCounter (empty, accumulation, rotation, wraparound, thread safety)
- [x] 3-node regtest shell test asserts relay_hints.list payload + blocks_served_24h increments
- [x] Full daemon build + dashboard|rpc|network ctest lanes green
- [ ] CI: all lanes

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Then poll CI and mark ready, same as Phase 2a's Task 8.

---

## Coverage map (self-review)

| Phase 2b daemon requirement | Task |
|---|---|
| Rolling 24h counter primitive + tests | 1 |
| BlockRelayManager.blocks_served_24h | 2 |
| P2pManager.bytes_relayed_24h | 3 |
| relay_hints.list RPC method | 4 |
| Surface 2 new fields on getnetworkinfo.relay | 5 |
| Integration test | 6 |
| Full build + sanity | 7 |
| Draft PR + CI | 8 |
