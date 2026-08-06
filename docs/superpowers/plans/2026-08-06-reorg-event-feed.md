# Reorg Event Feed (Sub-project A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record chain reorganisations inside the daemon and expose them over RPC as `reorg.status`, so a consumer can see reorgs that begin and resolve between external polls.

**Architecture:** One self-contained `ReorgLog` class (a bounded ring, a process-lifetime counter, and a boot identity) owned by `ChainstateService`. It is fed by a single call at the existing reorg site and read by one RPC handler registered alongside `safemode.status`.

**Tech Stack:** C++17, the repository's existing `din::Json`, gtest for unit tests, and the existing Python/shell integration harness under `tests/integration/`.

**Spec:** `docs/superpowers/specs/2026-08-06-reorg-event-feed-design.md`. Read it before Task 1.

## Global Constraints

- **Recording must never throw, never block, and never affect consensus.** It observes a decision already made. If any of the three cannot be guaranteed, the code does not go in — observability is not worth a consensus risk.
- **A reorg is `!disconnect_path.empty()` alone.** A connect-only advance is an ordinary new block. The adjacent log line fires on `disconnect || connect` and must NOT be reused as the trigger.
- **Event payload is fact and depth only:** `seq`, `timestamp`, `disconnected`, `connected`. No fork-point hash, no tip hashes, no per-block paths.
- **Ring size is 64**, fixed. `total` is the process-lifetime count and is NOT the ring length — a consumer detects overflow by comparing them.
- **`reorg.status` is registered in `register_daemon_status_rpc_methods()`** (`src/rpc/methods_daemon_status.cpp`), reached from `RegisterDiagnosticsRPC(ctx)` at `src/rpc/rpc_init.cpp:32`. Do NOT create a new registration function: three handler sets in this repository are written, compiled, and unreachable because their registration function is called from nowhere (#526).
- **Tests use gtest.** Do NOT use bare `assert()` — it compiles to nothing under `NDEBUG`, so an assert-based test passes vacuously in a release build. Some existing test files in this repo do this; do not copy them.
- **Compilation is not a gate.** Every dead subsystem found in this repository compiled cleanly.

## File Structure

```
include/daemon/reorg_log.h        ReorgLog: ring, counter, boot id. Header-only, no deps
                                  beyond <mutex>/<deque>/<string>. Self-contained so it can
                                  be unit-tested without linking the daemon.
tests/test_reorg_log.cpp          gtest unit tests for ReorgLog in isolation.
include/daemon/services/chainstate_service.h   +1 member, +1 accessor.
src/daemon/services/chainstate_service.cpp     +1 recording call at the reorg site.
src/rpc/methods_daemon_status.cpp              +1 handler, +1 registration.
tests/integration/test_reorg_event_feed.sh     the three gates from the spec.
CMakeLists.txt / tests/CMakeLists.txt          target wiring.
```

`ReorgLog` is deliberately header-only and dependency-free: the riskiest file in this change is
`chainstate_service.cpp`, and keeping the logic out of it means the logic can be tested without
building the daemon.

---

### Task 1: ReorgLog

**Files:**
- Create: `include/daemon/reorg_log.h`
- Test: `tests/test_reorg_log.cpp`
- Modify: `tests/CMakeLists.txt` (add the `test_reorg_log` target)

**Interfaces:**
- Consumes: nothing.
- Produces: `dinero::ReorgLog` with `void Record(uint32_t disconnected, uint32_t connected) noexcept`, `ReorgLog::Snapshot Take() const` (the accessor callers should use — counter and ring under one lock), `uint64_t Total() const`, `std::vector<ReorgLogEvent> Events() const`, `const std::string& BootId() const`; `struct Snapshot { std::string boot_id; uint64_t total; std::vector<ReorgLogEvent> events; }`; and `struct ReorgLogEvent { uint64_t seq; std::string timestamp; uint32_t disconnected; uint32_t connected; }`. Used by Tasks 2 and 3.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_reorg_log.cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "daemon/reorg_log.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using dinero::ReorgLog;

TEST(ReorgLogTest, starts_empty_with_a_boot_id) {
    ReorgLog log;
    EXPECT_EQ(log.Total(), 0u);
    EXPECT_TRUE(log.Events().empty());
    EXPECT_FALSE(log.BootId().empty());
}

TEST(ReorgLogTest, two_instances_have_different_boot_ids) {
    // The boot id is how a consumer knows seq and total have reset.
    ReorgLog a;
    ReorgLog b;
    EXPECT_NE(a.BootId(), b.BootId());
}

TEST(ReorgLogTest, records_depth_and_assigns_sequence_from_one) {
    ReorgLog log;
    log.Record(3, 4);
    // Hoist the snapshot into a local. `Events()` returns BY VALUE, so
    // `log.Events().front()` would bind a reference into a temporary that dies
    // at the end of the full expression — lifetime extension does not apply
    // through a member call. Clang catches it with -Wdangling-gsl; at runtime
    // it reads garbage.
    const auto events = log.Events();
    ASSERT_EQ(events.size(), 1u);
    const auto& e = events.front();
    EXPECT_EQ(e.seq, 1u);
    EXPECT_EQ(e.disconnected, 3u);
    EXPECT_EQ(e.connected, 4u);
    EXPECT_FALSE(e.timestamp.empty());
}

TEST(ReorgLogTest, sequence_is_monotonic) {
    ReorgLog log;
    log.Record(1, 1);
    log.Record(1, 1);
    log.Record(1, 1);
    const auto events = log.Events();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].seq, 1u);
    EXPECT_EQ(events[1].seq, 2u);
    EXPECT_EQ(events[2].seq, 3u);
}

TEST(ReorgLogTest, ring_keeps_the_newest_and_total_keeps_climbing) {
    // The whole point of exposing total separately: a consumer that sees total
    // outrun the events it can account for knows the ring overflowed, rather
    // than silently under-reporting.
    ReorgLog log;
    for (int i = 0; i < 100; ++i) log.Record(1, 1);
    const auto events = log.Events();
    EXPECT_EQ(events.size(), ReorgLog::kCapacity);
    EXPECT_EQ(log.Total(), 100u);
    EXPECT_EQ(events.front().seq, 100u - ReorgLog::kCapacity + 1u);
    EXPECT_EQ(events.back().seq, 100u);
}

TEST(ReorgLogTest, events_returns_a_snapshot_not_a_reference) {
    ReorgLog log;
    log.Record(1, 1);
    auto snapshot = log.Events();
    log.Record(2, 2);
    EXPECT_EQ(snapshot.size(), 1u) << "a caller's snapshot must not grow underneath it";
}

TEST(ReorgLogTest, take_reads_the_counter_and_ring_under_one_lock) {
    // Reading Total() and Events() separately lets a Record() land between
    // them, so the total would outrun the accountable events with no overflow —
    // a false positive on the design's only overflow signal.
    ReorgLog log;
    for (int i = 0; i < 5; ++i) log.Record(1, 1);
    const auto snapshot = log.Take();
    EXPECT_EQ(snapshot.total, 5u);
    EXPECT_EQ(snapshot.events.size(), 5u);
    EXPECT_EQ(snapshot.boot_id, log.BootId());
    EXPECT_EQ(snapshot.events.back().seq, snapshot.total);
}

TEST(ReorgLogTest, take_stays_self_consistent_while_recording) {
    // The property that matters: within one snapshot, the newest seq never
    // exceeds the total. A torn read breaks exactly this.
    ReorgLog log;
    std::thread writer([&log] {
        for (int i = 0; i < 2000; ++i) log.Record(1, 1);
    });
    for (int i = 0; i < 2000; ++i) {
        const auto snapshot = log.Take();
        if (!snapshot.events.empty()) {
            ASSERT_LE(snapshot.events.back().seq, snapshot.total)
                << "torn read: a recorded event is newer than the total";
        }
    }
    writer.join();
}

TEST(ReorgLogTest, timestamp_is_rfc3339_utc) {
    // Nothing tested the format, so a regression would ship straight into the
    // JSON a consumer parses.
    ReorgLog log;
    log.Record(1, 1);
    const auto events = log.Events();
    ASSERT_EQ(events.size(), 1u);
    const std::string& ts = events.front().timestamp;
    ASSERT_EQ(ts.size(), 20u) << "expected YYYY-MM-DDTHH:MM:SSZ, got: " << ts;
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[7], '-');
    EXPECT_EQ(ts[10], 'T');
    EXPECT_EQ(ts[13], ':');
    EXPECT_EQ(ts[16], ':');
    EXPECT_EQ(ts[19], 'Z');
    EXPECT_NE(ts.substr(0, 4), "1900") << "gmtime failed and left a zeroed tm";
}

TEST(ReorgLogTest, concurrent_record_loses_nothing) {
    // Record() runs on the chain-activation path; Events() runs on an RPC
    // thread. Losing an event to a race would be a silent under-report.
    ReorgLog log;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&log] {
            for (int i = 0; i < 100; ++i) log.Record(1, 1);
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(log.Total(), 800u);
}
```

- [ ] **Step 2: Register the test target**

The target must exist before it can be built, so this comes before the
confirm-it-fails step. Append to `tests/CMakeLists.txt`, following the
`test_clock_source` pattern already in that file:

```cmake
add_executable(test_reorg_log
  tests/test_reorg_log.cpp
)
add_dependencies(test_reorg_log gtest gtest_main)
target_include_directories(test_reorg_log BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_reorg_log PRIVATE
  GTest::gtest_main
)
add_test(NAME test_reorg_log COMMAND test_reorg_log)
set_tests_properties(test_reorg_log PROPERTIES TIMEOUT 60)
```

- [ ] **Step 3: Run it to confirm it fails**

Run: `cmake --build build --target test_reorg_log 2>&1 | tail -20`
Expected: FAIL — `fatal error: daemon/reorg_log.h: No such file or directory`.
If instead you see "no rule to make target", the CMake change has not been
picked up; re-run `cmake -S . -B build` and try again.

- [ ] **Step 4: Implement ReorgLog**

```cpp
// include/daemon/reorg_log.h
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DINERO_DAEMON_REORG_LOG_H
#define DINERO_DAEMON_REORG_LOG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif

namespace dinero {

/// Named ReorgLogEvent, not ReorgEvent: dinero::ReorgEvent already exists in
/// include/wallet/reorg_event.h. The collision is invisible until some
/// translation unit includes both headers, which first happens when this is
/// wired into ChainstateService — an ODR clash discovered at the worst moment.
struct ReorgLogEvent {
    uint64_t seq = 0;
    std::string timestamp;   // ISO 8601, UTC
    uint32_t disconnected = 0;
    uint32_t connected = 0;
};

/**
 * In-memory record of chain reorganisations.
 *
 * Deliberately self-contained and header-only: the call site is inside
 * chainstate_service.cpp, the riskiest file in the daemon, and keeping the
 * logic here means it can be tested without building the daemon at all.
 *
 * Record() is called on the chain-activation path. It must never throw and
 * never block on anything slow — it takes a mutex, pushes four integers and a
 * short string, and returns. It observes a decision already made and can never
 * change one.
 *
 * Total() is the process-lifetime count and is NOT the ring length. A consumer
 * that sees Total() advance further than the events it can account for knows
 * the ring overflowed. That is the only overflow signal, which is why the
 * counter is exposed separately.
 */
class ReorgLog {
public:
    static constexpr size_t kCapacity = 64;

    ReorgLog() : boot_id_(MakeBootId()) {}

    void Record(uint32_t disconnected, uint32_t connected) noexcept {
        try {
            // Format the timestamp BEFORE taking the lock. put_time does
            // locale/facet work through an ostringstream, which is by far the
            // slowest thing here, and this mutex is held against the chain-
            // activation path. Nothing inside the critical section may be slow.
            std::string stamp = NowIso8601();

            std::lock_guard<std::mutex> lock(mutex_);
            ReorgLogEvent event;
            event.seq = ++total_;
            event.timestamp = std::move(stamp);
            event.disconnected = disconnected;
            event.connected = connected;
            events_.push_back(std::move(event));
            while (events_.size() > kCapacity) {
                events_.pop_front();
            }
        } catch (...) {
            // A failure to record an observation must never disturb chain
            // activation. Dropping the event is the correct outcome; Total()
            // then outruns the ring, which is exactly how a consumer learns
            // something was lost.
        }
    }

    /// The counter and the ring read together, under ONE lock.
    ///
    /// This is the accessor callers should use. Reading Total() and Events()
    /// separately allows a Record() to land between them, so the total would
    /// exceed the events a consumer can account for with no overflow having
    /// occurred — and that comparison is the design's ONLY overflow signal, so
    /// the false positive lands exactly on the thing it exists to detect.
    struct Snapshot {
        std::string boot_id;
        uint64_t total = 0;
        std::vector<ReorgLogEvent> events;
    };

    Snapshot Take() const {
        Snapshot snapshot;
        snapshot.boot_id = boot_id_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.total = total_;
            snapshot.events.assign(events_.begin(), events_.end());
        }
        return snapshot;
    }

    uint64_t Total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

    std::vector<ReorgLogEvent> Events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<ReorgLogEvent>(events_.begin(), events_.end());
    }

    const std::string& BootId() const { return boot_id_; }

private:
    static std::string NowIso8601() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    static std::string MakeBootId() {
        // random_device alone is not enough. [rand.device] explicitly permits
        // an implementation to substitute a deterministic engine when it cannot
        // produce non-deterministic values — and the property actually required
        // here is distinctness ACROSS PROCESSES, so that a consumer can tell a
        // restart from data loss. Mixing in the pid and a high-resolution clock
        // read makes that hold even where random_device does not.
        std::random_device rd;
        const auto pid = static_cast<uint64_t>(DINERO_GETPID());
        const auto tick = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << (pid ^ tick);
        for (int i = 0; i < 2; ++i) {
            out << std::hex << std::setw(8) << std::setfill('0') << rd();
        }
        return out.str();
    }

    mutable std::mutex mutex_;
    std::deque<ReorgLogEvent> events_;
    uint64_t total_ = 0;
    const std::string boot_id_;
};

}  // namespace dinero

#endif  // DINERO_DAEMON_REORG_LOG_H
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --target test_reorg_log && ./build/test_reorg_log`
Expected: PASS, 7 tests

- [ ] **Step 6: Prove the tests gate**

Temporarily change `while (events_.size() > kCapacity)` to `while (events_.size() > 1000)` and re-run.
Expected: `ring_keeps_the_newest_and_total_keeps_climbing` FAILS. Restore, re-run, confirm green.

- [ ] **Step 7: Commit**

```bash
git add include/daemon/reorg_log.h tests/test_reorg_log.cpp tests/CMakeLists.txt
git commit -m "feat(daemon): ReorgLog — bounded reorg ring with overflow-detectable counter"
```

---

### Task 2: Wire ReorgLog into ChainstateService

**Files:**
- Modify: `include/daemon/services/chainstate_service.h` (near the `IsInSafeMode`/`GetSafeModeReason` accessors at ~line 323)
- Modify: `src/daemon/services/chainstate_service.cpp` (the reorg site, immediately after the `"[ActivateBestChain] REORG DETECTED"` log block)

**Interfaces:**
- Consumes: `dinero::ReorgLog` (Task 1).
- Produces: `const ReorgLog& GetReorgLog() const` on `ChainstateService`. Used by Task 3.

- [ ] **Step 1: Add the member and accessor**

In `include/daemon/services/chainstate_service.h`, add the include near the other daemon includes:

```cpp
#include "daemon/reorg_log.h"
```

Immediately after the existing safe-mode accessors (~line 324), add:

```cpp
    /// Read-only observability: reorganisations recorded this process lifetime.
    /// Exposed over RPC as reorg.status. Never consulted by consensus logic.
    const ReorgLog& GetReorgLog() const { return reorg_log_; }
```

And in the private member area, alongside the other mutable state:

```cpp
    ReorgLog reorg_log_;
```

- [ ] **Step 2: Add the recording call**

In `src/daemon/services/chainstate_service.cpp`, immediately after the existing block that logs
`"[ActivateBestChain] REORG DETECTED"`, add:

```cpp
    // Read-only observability. Keyed on disconnect_path ALONE, deliberately not
    // on the ||-condition the log above uses: a connect-only advance is an
    // ordinary new block, and counting those would make every downstream reorg
    // rate meaningless. Record() is noexcept and takes only a short mutex, so
    // it cannot disturb activation.
    if (!disconnect_path.empty()) {
        reorg_log_.Record(static_cast<uint32_t>(disconnect_path.size()),
                          static_cast<uint32_t>(connect_path.size()));
    }
```

- [ ] **Step 3: Build the daemon**

Run: `cmake --build build --target dinerod -j8`
Expected: BUILD succeeds, 0 errors.

- [ ] **Step 4: Confirm the symbol actually shipped**

Run: `nm -C build/dinerod | grep -c 'ReorgLog'`
Expected: non-zero.

This step exists because this repository contains multiple subsystems that compile and never
reach a binary. A successful build is not evidence that code shipped.

- [ ] **Step 5: Commit**

```bash
git add include/daemon/services/chainstate_service.h src/daemon/services/chainstate_service.cpp
git commit -m "feat(daemon): record reorganisations at the chain-activation site"
```

---

### Task 3: The `reorg.status` RPC method

**Files:**
- Modify: `src/rpc/methods_daemon_status.cpp` (handler near `rpc_safemode_status` at ~line 169; registration inside `register_daemon_status_rpc_methods` near the `safemode.status` block at ~line 404)

**Interfaces:**
- Consumes: `ChainstateService::GetReorgLog()` (Task 2).
- Produces: the RPC method `reorg.status`. Used by Task 4 and by the fleet watcher.

- [ ] **Step 1: Add the handler**

Next to `rpc_safemode_status`, add:

```cpp
// reorg.status — read-only record of reorganisations this process lifetime.
//
// `total` is the process-lifetime count, NOT the ring length. A consumer that
// sees total outrun the events it can account for knows the ring overflowed and
// can report a gap instead of silently under-reporting. `boot_id` changes on
// restart, so a consumer can tell a reset apart from data loss.
static din::Json rpc_reorg_status(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "chainstate_not_initialized";
        return result;
    }
    // ONE snapshot, taken under a single lock. Calling BootId()/Total()/Events()
    // separately would let a Record() land between them, so total could outrun
    // the events in the same reply — a fabricated overflow on the one signal a
    // consumer uses to detect real ones.
    const auto snapshot = daemon_ctx->chainstate->GetReorgLog().Take();
    result["boot_id"] = snapshot.boot_id;
    result["total"] = static_cast<Json::UInt64>(snapshot.total);

    din::Json events(Json::arrayValue);
    for (const auto& event : snapshot.events) {
        din::Json item(Json::objectValue);
        item["seq"] = static_cast<Json::UInt64>(event.seq);
        item["timestamp"] = event.timestamp;
        item["disconnected"] = static_cast<Json::UInt>(event.disconnected);
        item["connected"] = static_cast<Json::UInt>(event.connected);
        events.append(item);
    }
    result["events"] = events;
    return result;
}
```

- [ ] **Step 2: Register it on the proven-live path**

Inside `register_daemon_status_rpc_methods`, immediately after the `safemode.status` registration:

```cpp
    // reorg.status — read-only reorganisation record.
    //
    // Registered HERE, alongside safemode.status, deliberately. This function is
    // reached from RegisterDiagnosticsRPC(ctx) at src/rpc/rpc_init.cpp:32 and is
    // empirically live — safemode.status answers on production nodes. Creating a
    // new registration function is exactly how getchaintips, getchainwork and
    // getreorgstatus became unreachable: they are defined, compiled, and called
    // from nowhere.
    RpcMethodMeta reorg_status_meta;
    reorg_status_meta.name = "status";
    reorg_status_meta.ns = "reorg";
    reorg_status_meta.description =
        "Read the in-memory record of chain reorganisations for this process: "
        "a bounded ring of recent events plus a process-lifetime total.";
    reorg_status_meta.result.type = "object";
    reorg_status_meta.result.desc =
        "{ boot_id: string, total: uint, events: [{seq, timestamp, disconnected, connected}] }";
    registry.registerHandler("reorg.status", rpc_reorg_status, reorg_status_meta, "Reorg");
```

- [ ] **Step 3: Build**

Run: `cmake --build build --target dinerod -j8`
Expected: BUILD succeeds, 0 errors.

- [ ] **Step 4: Confirm the method actually answers**

Start a daemon against a throwaway datadir, then:

```bash
curl -sS -X POST http://127.0.0.1:${RPC_PORT}/ -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":"t","method":"reorg.status","params":[]}'
```

Expected: a result object with `boot_id`, `total: 0`, and `events: []`.
**A `-32601 Method not found` here means the registration did not take, which is the single
most likely way this work is wasted.** Do not proceed until this returns a result.

- [ ] **Step 5: Commit**

```bash
git add src/rpc/methods_daemon_status.cpp
git commit -m "feat(rpc): reorg.status, registered alongside safemode.status"
```

---

### Task 4: The three gates

**Files:**
- Create: `tests/integration/test_reorg_event_feed.sh`
- Modify: `tests/integration/CMakeLists.txt` (register the script, following the existing reorg-test entries)

**Interfaces:**
- Consumes: the `reorg.status` method (Task 3).
- Produces: nothing consumed by later tasks. This is the final task.

- [ ] **Step 1: Write the integration test**

Model the reorg-forcing sequence on the existing harness in `tests/integration/` —
`build_reorg_guard.sh` and `test_csn_reorg_churn_restart_soak.sh` already start nodes and force
a reorganisation; reuse that machinery rather than inventing a new way to fork a chain.

```sh
#!/bin/sh
# tests/integration/test_reorg_event_feed.sh
#
# Three gates for the reorg event feed. Compilation is NOT one of them: every
# dead subsystem found in this repository compiled cleanly.
set -eu

. "$(dirname "$0")/reorg_harness.sh"     # start_node, force_reorg, stop_node, rpc

fail() { echo "FAIL: $*" >&2; exit 1; }

# ── Gate 1: the method answers at all ───────────────────────────────────────
start_node
answer=$(rpc reorg.status)
echo "$answer" | grep -q '"boot_id"' \
  || fail "reorg.status did not return a result: $answer"
echo "$answer" | grep -q '\-32601' \
  && fail "reorg.status is not registered"
echo "$answer" | grep -q '"total" *: *0' \
  || fail "a fresh node should report total 0: $answer"

# ── Gate 2: a real reorg produces a matching event ──────────────────────────
force_reorg --disconnect 2 --connect 3
answer=$(rpc reorg.status)
echo "$answer" | grep -q '"total" *: *1' \
  || fail "expected exactly one recorded reorg: $answer"
echo "$answer" | grep -q '"disconnected" *: *2' \
  || fail "recorded depth does not match the forced reorg: $answer"
echo "$answer" | grep -q '"connected" *: *3' \
  || fail "recorded connect depth does not match: $answer"

first_boot=$(echo "$answer" | sed -n 's/.*"boot_id" *: *"\([^"]*\)".*/\1/p')

# ── Gate 3: a restart records nothing ───────────────────────────────────────
# ChainstateService exposes no initial-block-download flag, so whether chain
# activation replays through the reorg path on startup CANNOT be settled by
# reading the code. This settles it. A failure here means the recorder needs a
# replay guard — a finding worth having before anyone trusts this feed.
stop_node
start_node
answer=$(rpc reorg.status)
echo "$answer" | grep -q '"total" *: *0' \
  || fail "restart manufactured phantom reorgs: $answer"
echo "$answer" | grep -q '"events" *: *\[\]' \
  || fail "restart left events in the ring: $answer"

second_boot=$(echo "$answer" | sed -n 's/.*"boot_id" *: *"\([^"]*\)".*/\1/p')
[ "$first_boot" != "$second_boot" ] \
  || fail "boot_id did not change across a restart, so a consumer cannot tell a reset from data loss"

stop_node
echo "OK: all three gates passed"
```

- [ ] **Step 2: Run it and confirm it passes**

Run: `sh tests/integration/test_reorg_event_feed.sh`
Expected: `OK: all three gates passed`

- [ ] **Step 3: Prove gate 2 gates**

Temporarily change the recording condition in `chainstate_service.cpp` from
`if (!disconnect_path.empty())` to `if (false)` and re-run.
Expected: FAIL at "expected exactly one recorded reorg". Restore and re-run green.

- [ ] **Step 4: Register the test**

Add to `tests/integration/CMakeLists.txt`, following the existing reorg-test entries:

```cmake
add_test(NAME test_reorg_event_feed
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_reorg_event_feed.sh)
```

- [ ] **Step 5: Run the whole suite**

Run: `ctest --test-dir build -R 'reorg' --output-on-failure`
Expected: all reorg-related tests pass, including the pre-existing ones.

- [ ] **Step 6: Commit**

```bash
git add tests/integration/test_reorg_event_feed.sh tests/integration/CMakeLists.txt
git commit -m "test(integration): three gates for the reorg event feed"
```

---

## Self-Review

**Spec coverage.** Recording keyed on `disconnect_path` alone (Task 2); noexcept, non-blocking,
consensus-neutral (Task 1 `Record`, Task 2 comment); event payload of exactly `seq`/`timestamp`/
`disconnected`/`connected` (Task 1); ring of 64 with a separate process-lifetime total (Task 1);
`boot_id` (Task 1); `reorg.status` returning the whole ring (Task 3); registration on the
proven-live path (Task 3 Step 2); all three spec gates (Task 4); overflow unit-tested (Task 1).

**Placeholders.** None. Every step carries runnable code or an exact command.

**Type consistency.** `ReorgLogEvent`'s four fields are defined once in Task 1 and read unchanged in
Task 3. `GetReorgLog()` is produced in Task 2 and consumed in Task 3. `kCapacity` is referenced
by name in the Task 1 tests rather than duplicating 64.

**Known dependency, stated rather than hidden:** Task 4 assumes a `reorg_harness.sh` exposing
`start_node`, `force_reorg`, `stop_node` and `rpc`. The existing `build_reorg_guard.sh` and
`test_csn_reorg_churn_restart_soak.sh` already force reorganisations; the implementer must
extract that machinery into the shared harness rather than duplicating it. If those scripts turn
out not to be factorable, that is a real finding — report it rather than writing a second
chain-forking implementation.

**Sequencing note.** The spec says the ring size and this feed's justification should both be
confirmed by watcher data before this is built. That remains true: this plan is ready to execute,
but executing it before the fleet watcher has run against a real fleet means guessing at a number
the watcher was built to measure.
