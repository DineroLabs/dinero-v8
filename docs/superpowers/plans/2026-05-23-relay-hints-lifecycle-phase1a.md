# RELAY_HINTS Lifecycle — Phase 1a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make RELAY_HINTS reliable at the discovery layer by adding TTL + per-hint failure-counter eviction on the receiver, 5-minute periodic self-resend from the existing `keepalive_loop`, 90s grace on the fleet relay directory, and registrant reconnect re-register — all gated through a `ClockSource` abstraction so tests can advance time deterministically.

**Architecture:** No new threads. All periodic work folds into the existing `keepalive_loop` (30s cadence). Wire format unchanged (Phase 1a is behavior-only; Phase 1b/1c add wire changes). Receiver cache (`relay_hints_by_target_`) extends with two new fields; fleet directory (`RelayRegistry`) extends with a grace-pending flag. Time access goes through `ClockSource` so unit tests use `FakeClockSource::AdvanceSteady(...)` instead of `std::this_thread::sleep_for`.

**Tech Stack:** C++20, gtest (existing), CMake (`tests/CMakeLists.txt` registration pattern matches `test_addrman_relay_select`).

**Spec:** `docs/superpowers/specs/2026-05-23-relay-hints-lifecycle-design.md`

**Branch:** `feature/relay-hints-lifecycle-1a` off `dinero-main`. All commits signed as `Dinero Labs <team@dinerolabs.org>` (verified at end of plan setup).

---

## File map (lock decomposition before tasks)

| File | Status | Responsibility |
|---|---|---|
| `include/network/clock_source.h` | **Create** | `ClockSource` interface + `SystemClockSource` + `FakeClockSource` |
| `tests/network/test_clock_source.cpp` | **Create** | Unit tests for `FakeClockSource` (the real one is trivial) |
| `include/network/relay_hints_eviction.h` | **Create** | Pure helper functions: `ShouldEvictByTtl`, `ShouldEvictByFailure`, `HintMetricsCounters` struct |
| `src/network/relay_hints_eviction.cpp` | **Create** | Implementation of above |
| `tests/network/test_relay_hints_eviction.cpp` | **Create** | Unit tests for the helpers + fake clock |
| `src/daemon/p2p_manager.h` | Modify | Add fields to `RelayHintRecord`; add `clock_` member; declare new sweep + resend methods; declare hint counters atomic struct |
| `src/daemon/p2p_manager.cpp` | Modify | Call sweep + resend from `keepalive_loop`; increment failure counter in orchestrator callback; refresh on duplicate hint ingest |
| `include/network/relay_registry.h` | Modify | Add `grace_expires_at` to `Registration`; add `MarkGracePending(node_id, grace_until)` method |
| `src/network/relay_registry.cpp` | Modify | Implement `MarkGracePending`; teach `Sweep()` to evict grace-expired entries |
| `tests/network/test_relay_registry_grace.cpp` | **Create** | Tests for the grace path |
| `tests/CMakeLists.txt` | Modify | Register 4 new test targets |

---

## Conventions used in every task

- **Constants in `src/daemon/p2p_manager.h`** (declared `static constexpr`):
  - `kHintTtl = std::chrono::minutes(15)`
  - `kHintMaxFailures = 3`
  - `kHintResendPeriod = std::chrono::minutes(5)`
  - `kRelayDirectoryGracePeriod = std::chrono::seconds(90)`
- **Time type:** all TTL/expiry/learned_at fields are `std::chrono::steady_clock::time_point` (already what `RelayHintRecord::learned_at` uses).
- **Test naming:** gtest fixtures `RelayHintsEvictionTest`, `ClockSourceTest`, `RelayRegistryGraceTest`. Test names are snake_case after fixture.
- **Commit signing:** `git commit -S -m "..."` — verify with `git log --show-signature -1` after first commit.
- **Commit author:** `Dinero Labs <team@dinerolabs.org>` (per-repo config already set; see `feedback project_commit_signing`).
- **Commit message prefix:** `feat(relay-hints): ...` for behavior, `test(relay-hints): ...` for test-only, `refactor(relay-hints): ...` for moves.

---

## Task 0: Branch setup + signing verification

**Files:** none (branch operation only)

- [ ] **Step 1: Create branch off dinero-main**

```bash
git fetch origin dinero-main
git checkout -b feature/relay-hints-lifecycle-1a origin/dinero-main
git status
```

Expected: `On branch feature/relay-hints-lifecycle-1a. Your branch is up to date with 'origin/dinero-main'. nothing to commit, working tree clean`

- [ ] **Step 2: Verify signing config is wired for this repo**

```bash
git config --get user.signingkey
git config --get user.email
git config --get gpg.format
git config --get commit.gpgsign
```

Expected output:
```
/Users/haydarevich/.ssh/id_ed25519_dinero_signing.pub
team@dinerolabs.org
ssh
true
```

If any line is missing or different, STOP and reconfigure per the repo's signing convention before proceeding. Every commit on this branch must be signed.

---

## Task 1: ClockSource header + trivial impls

**Files:**
- Create: `include/network/clock_source.h`

- [ ] **Step 1: Write the header**

Create `include/network/clock_source.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <chrono>
#include <memory>

namespace dinero::network {

// Abstraction over time sources so TTL / expiry tests can advance time
// deterministically without std::this_thread::sleep_for. Scope is
// intentionally limited to the RELAY_HINTS lifecycle code path; do not
// retrofit unrelated time reads (YAGNI).
class ClockSource {
public:
    virtual ~ClockSource() = default;
    virtual std::chrono::steady_clock::time_point SteadyNow() const = 0;
    virtual std::chrono::system_clock::time_point SystemNow() const = 0;
};

class SystemClockSource final : public ClockSource {
public:
    std::chrono::steady_clock::time_point SteadyNow() const override {
        return std::chrono::steady_clock::now();
    }
    std::chrono::system_clock::time_point SystemNow() const override {
        return std::chrono::system_clock::now();
    }
};

// Test-only. Holds its own "current time" that callers advance manually.
class FakeClockSource final : public ClockSource {
public:
    FakeClockSource()
        : steady_(std::chrono::steady_clock::time_point{}),
          system_(std::chrono::system_clock::time_point{}) {}

    std::chrono::steady_clock::time_point SteadyNow() const override {
        return steady_;
    }
    std::chrono::system_clock::time_point SystemNow() const override {
        return system_;
    }

    void AdvanceSteady(std::chrono::nanoseconds delta) { steady_ += delta; }
    void AdvanceSystem(std::chrono::nanoseconds delta) { system_ += delta; }

private:
    std::chrono::steady_clock::time_point steady_;
    std::chrono::system_clock::time_point system_;
};

}  // namespace dinero::network
```

- [ ] **Step 2: Commit**

```bash
git add include/network/clock_source.h
git commit -S -m "$(cat <<'EOF'
feat(relay-hints): add ClockSource abstraction for testable time

Foundation for RELAY_HINTS Phase 1a. SystemClockSource is the
production default; FakeClockSource lets unit tests advance steady_
and system_clock deterministically. Scope limited to hints code path
per YAGNI — no retrofit of unrelated time reads.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Verify signature**

```bash
git log --show-signature -1 --pretty=format:"%h %GS"
```

Expected: contains `team@dinerolabs.org` and "Good signature".

---

## Task 2: ClockSource unit tests + CMake registration

**Files:**
- Create: `tests/network/test_clock_source.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/network/test_clock_source.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/clock_source.h"

#include <gtest/gtest.h>

using dinero::network::FakeClockSource;
using dinero::network::SystemClockSource;
using std::chrono::milliseconds;
using std::chrono::minutes;
using std::chrono::seconds;

TEST(ClockSourceTest, system_clock_steady_now_is_monotonic) {
    SystemClockSource c;
    const auto a = c.SteadyNow();
    const auto b = c.SteadyNow();
    EXPECT_LE(a, b);
}

TEST(ClockSourceTest, fake_clock_starts_at_epoch) {
    FakeClockSource c;
    EXPECT_EQ(c.SteadyNow().time_since_epoch().count(), 0);
    EXPECT_EQ(c.SystemNow().time_since_epoch().count(), 0);
}

TEST(ClockSourceTest, fake_clock_advance_steady_moves_only_steady) {
    FakeClockSource c;
    const auto sys0 = c.SystemNow();
    c.AdvanceSteady(minutes(5));
    EXPECT_EQ(c.SteadyNow().time_since_epoch(), minutes(5));
    EXPECT_EQ(c.SystemNow(), sys0);
}

TEST(ClockSourceTest, fake_clock_advance_system_moves_only_system) {
    FakeClockSource c;
    const auto steady0 = c.SteadyNow();
    c.AdvanceSystem(seconds(90));
    EXPECT_EQ(c.SystemNow().time_since_epoch(), seconds(90));
    EXPECT_EQ(c.SteadyNow(), steady0);
}
```

- [ ] **Step 2: Add CMake registration**

In `tests/CMakeLists.txt`, after the existing `test_addrman_relay_select` block (around line 48), append:

```cmake
add_executable(test_clock_source
  tests/network/test_clock_source.cpp
)
target_include_directories(test_clock_source PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_clock_source PRIVATE GTest::gtest_main)
add_test(NAME ClockSource COMMAND test_clock_source)
set_tests_properties(ClockSource PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 5
)
```

- [ ] **Step 3: Configure and build the test**

```bash
cmake -S . -B build-rc14-quic
cmake --build build-rc14-quic --target test_clock_source -j8
```

Expected: builds cleanly. No warnings.

- [ ] **Step 4: Run the test**

```bash
cd build-rc14-quic && ctest -R ClockSource --output-on-failure && cd ..
```

Expected: `4 tests passed`.

- [ ] **Step 5: Commit**

```bash
git add tests/network/test_clock_source.cpp tests/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(relay-hints): unit tests for ClockSource (system + fake)

Asserts FakeClockSource starts at epoch and advances Steady/System
independently. SystemClockSource is exercised for monotonicity.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Wire ClockSource into P2PManager (no behavior change)

**Files:**
- Modify: `src/daemon/p2p_manager.h`
- Modify: `src/daemon/p2p_manager.cpp` (constructor only)

- [ ] **Step 1: Add include + member**

In `src/daemon/p2p_manager.h`, add near the top with other network includes:

```cpp
#include "network/clock_source.h"
```

In the `P2PManager` class private section, near other members (just before `relay_hints_mutex_` around line 825), add:

```cpp
// Time source for TTL/expiry logic in the hints subsystem.
// Defaults to SystemClockSource; tests inject a FakeClockSource.
std::unique_ptr<dinero::network::ClockSource> clock_;
```

- [ ] **Step 2: Add constructor parameter with default**

Locate the existing `P2PManager` constructor declaration in `p2p_manager.h`. Add a second constructor (keep the existing one untouched for source-compat):

```cpp
// Existing constructor stays untouched — defaults clock_ to SystemClockSource.
// Test-only constructor: inject a FakeClockSource for deterministic TTL tests.
explicit P2PManager(std::unique_ptr<dinero::network::ClockSource> clock);
```

In `src/daemon/p2p_manager.cpp`, locate the existing constructor body. After all existing member initialization, add:

```cpp
if (!clock_) {
    clock_ = std::make_unique<dinero::network::SystemClockSource>();
}
```

Then add the new test-only constructor (delegates to the default constructor for everything else, then overrides clock_):

```cpp
P2PManager::P2PManager(std::unique_ptr<dinero::network::ClockSource> clock)
    : P2PManager() {  // delegate to default ctor
    clock_ = std::move(clock);
    if (!clock_) {
        clock_ = std::make_unique<dinero::network::SystemClockSource>();
    }
}
```

(If the default `P2PManager()` constructor takes args, mirror the args in the new ctor and pass them through. The point is: no behavior change for existing callers, just a `clock_` field that defaults to system time.)

- [ ] **Step 3: Build full daemon**

```bash
cmake --build build-rc14-quic --target dinerod -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinerod`. Zero new warnings.

- [ ] **Step 4: Verify clock_ is read nowhere yet**

```bash
grep -n "clock_->" src/daemon/p2p_manager.cpp
```

Expected: no matches. (We are only PLACING the field; consumers land in later tasks.)

- [ ] **Step 5: Commit**

```bash
git add src/daemon/p2p_manager.h src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
refactor(p2p): inject ClockSource into P2PManager (no behavior change)

Adds clock_ member defaulting to SystemClockSource and a test-only
constructor that accepts a FakeClockSource. No consumers yet — wired
in subsequent tasks (TTL sweep, periodic resend).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Extend RelayHintRecord + add eviction helpers

**Files:**
- Modify: `src/daemon/p2p_manager.h` (struct + constants)
- Create: `include/network/relay_hints_eviction.h`
- Create: `src/network/relay_hints_eviction.cpp`

- [ ] **Step 1: Extend RelayHintRecord**

In `src/daemon/p2p_manager.h` at line ~819, find:

```cpp
struct RelayHintRecord {
    std::array<uint8_t, 20> target_node_id{};
    dinero::p2p::NetworkType net{dinero::p2p::NetworkType::IPV4};
    std::vector<uint8_t> relay_addr;
    uint16_t relay_port{0};
    std::chrono::steady_clock::time_point learned_at;
};
```

Append a new field:

```cpp
    // Phase 1a: per-hint failure counter for eviction.
    // Incremented when a dial via this hint fails (RELAY_CONNECT error
    // OR QUIC handshake timeout on the resulting circuit). Reset to 0
    // on any successful handshake or on receipt of a fresh duplicate
    // hint. Drop when >= kHintMaxFailures.
    int consecutive_dial_failures{0};
```

- [ ] **Step 2: Add constants near existing kRelayDialBackoff**

In `src/daemon/p2p_manager.h`, near `static constexpr std::chrono::seconds kRelayDialBackoff{60};` (around line 836), append:

```cpp
    static constexpr std::chrono::minutes kHintTtl{15};
    static constexpr int kHintMaxFailures{3};
    static constexpr std::chrono::minutes kHintResendPeriod{5};
    static constexpr std::chrono::seconds kRelayDirectoryGracePeriod{90};
```

- [ ] **Step 3: Write the failing test for the helpers**

Create `tests/network/test_relay_hints_eviction.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_hints_eviction.h"

#include "network/clock_source.h"

#include <gtest/gtest.h>

using dinero::network::FakeClockSource;
using dinero::network::HintEvictionPolicy;
using dinero::network::ShouldEvictByFailure;
using dinero::network::ShouldEvictByTtl;
using std::chrono::minutes;

namespace {
HintEvictionPolicy default_policy() {
    return HintEvictionPolicy{
        .ttl = minutes(15),
        .max_failures = 3,
    };
}
}  // namespace

TEST(RelayHintsEvictionTest, fresh_hint_is_not_evicted) {
    FakeClockSource clock;
    const auto learned = clock.SteadyNow();
    clock.AdvanceSteady(minutes(1));
    EXPECT_FALSE(ShouldEvictByTtl(learned, clock.SteadyNow(), default_policy()));
}

TEST(RelayHintsEvictionTest, ttl_expiry_evicts) {
    FakeClockSource clock;
    const auto learned = clock.SteadyNow();
    clock.AdvanceSteady(minutes(16));
    EXPECT_TRUE(ShouldEvictByTtl(learned, clock.SteadyNow(), default_policy()));
}

TEST(RelayHintsEvictionTest, ttl_boundary_exact_15min_not_yet_evicted) {
    FakeClockSource clock;
    const auto learned = clock.SteadyNow();
    clock.AdvanceSteady(minutes(15));
    EXPECT_FALSE(ShouldEvictByTtl(learned, clock.SteadyNow(), default_policy()));
}

TEST(RelayHintsEvictionTest, failure_under_threshold_keeps_hint) {
    EXPECT_FALSE(ShouldEvictByFailure(2, default_policy()));
}

TEST(RelayHintsEvictionTest, failure_at_threshold_evicts) {
    EXPECT_TRUE(ShouldEvictByFailure(3, default_policy()));
}

TEST(RelayHintsEvictionTest, failure_over_threshold_evicts) {
    EXPECT_TRUE(ShouldEvictByFailure(99, default_policy()));
}
```

- [ ] **Step 4: Create the header**

Create `include/network/relay_hints_eviction.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Pure helper functions for RELAY_HINTS Phase 1a eviction policy.
// Extracted so the policy logic is unit-testable without
// instantiating P2PManager.

#pragma once

#include <chrono>

namespace dinero::network {

struct HintEvictionPolicy {
    std::chrono::steady_clock::duration ttl;
    int max_failures;
};

// True if (now - learned_at) > policy.ttl (strict inequality — equal-to is
// the boundary case where the hint just turned old enough to be refreshed
// but not yet evicted).
bool ShouldEvictByTtl(std::chrono::steady_clock::time_point learned_at,
                      std::chrono::steady_clock::time_point now,
                      const HintEvictionPolicy& policy);

// True if consecutive_failures >= policy.max_failures.
bool ShouldEvictByFailure(int consecutive_failures,
                          const HintEvictionPolicy& policy);

}  // namespace dinero::network
```

- [ ] **Step 5: Create the implementation**

Create `src/network/relay_hints_eviction.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_hints_eviction.h"

namespace dinero::network {

bool ShouldEvictByTtl(std::chrono::steady_clock::time_point learned_at,
                      std::chrono::steady_clock::time_point now,
                      const HintEvictionPolicy& policy) {
    return (now - learned_at) > policy.ttl;
}

bool ShouldEvictByFailure(int consecutive_failures,
                          const HintEvictionPolicy& policy) {
    return consecutive_failures >= policy.max_failures;
}

}  // namespace dinero::network
```

- [ ] **Step 6: CMake registration**

In `tests/CMakeLists.txt`, append after the `test_clock_source` block from Task 2:

```cmake
add_executable(test_relay_hints_eviction
  tests/network/test_relay_hints_eviction.cpp
  src/network/relay_hints_eviction.cpp
)
target_include_directories(test_relay_hints_eviction PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_relay_hints_eviction PRIVATE GTest::gtest_main)
add_test(NAME RelayHintsEviction COMMAND test_relay_hints_eviction)
set_tests_properties(RelayHintsEviction PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 5
)
```

You must also add `src/network/relay_hints_eviction.cpp` to whichever target builds `dinerod`. Search:

```bash
grep -n "src/network/relay_tls_keypair.cpp" CMakeLists.txt
```

Add the new .cpp on the next line, e.g., if it appears in a `set(P2P_SOURCES ...)` list, add `src/network/relay_hints_eviction.cpp` to that list. (Mirror exactly the pattern used for the neighboring file.)

- [ ] **Step 7: Configure, build, run**

```bash
cmake -S . -B build-rc14-quic
cmake --build build-rc14-quic --target test_relay_hints_eviction dinerod -j8
cd build-rc14-quic && ctest -R RelayHintsEviction --output-on-failure && cd ..
```

Expected: all 6 unit tests pass; dinerod builds cleanly.

- [ ] **Step 8: Commit**

```bash
git add include/network/relay_hints_eviction.h \
        src/network/relay_hints_eviction.cpp \
        tests/network/test_relay_hints_eviction.cpp \
        src/daemon/p2p_manager.h \
        tests/CMakeLists.txt \
        CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(relay-hints): RelayHintRecord gains failure counter; eviction helpers

RelayHintRecord adds consecutive_dial_failures (int). p2p_manager
defines kHintTtl=15min, kHintMaxFailures=3, kHintResendPeriod=5min,
kRelayDirectoryGracePeriod=90s.

Pure-function helpers ShouldEvictByTtl/ShouldEvictByFailure live in
src/network/relay_hints_eviction.{h,cpp} so the policy is unit-testable
without spinning up P2PManager. Six gtest cases cover TTL boundary,
failure under/at/over threshold.

No call sites wired yet — sweep + counter increment land in later tasks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Sweep relay_hints_by_target_ from keepalive_loop

**Files:**
- Modify: `src/daemon/p2p_manager.h` (declare `SweepRelayHintsCache`)
- Modify: `src/daemon/p2p_manager.cpp`

- [ ] **Step 1: Declare the sweep method**

In `src/daemon/p2p_manager.h`, near other private methods (around the `keepalive_loop()` declaration at line 856), add:

```cpp
// Phase 1a: evict stale relay-hint records from relay_hints_by_target_.
// Called from keepalive_loop on its existing 30s cadence; no new thread.
// Acquires relay_hints_mutex_. Logs eviction reason per entry.
void SweepRelayHintsCache();
```

- [ ] **Step 2: Implement the sweep**

In `src/daemon/p2p_manager.cpp`, place near other relay-hint helpers (e.g., right above `OrchestrateRelayDials`). Add:

```cpp
void P2PManager::SweepRelayHintsCache() {
    const auto now = clock_->SteadyNow();
    const dinero::network::HintEvictionPolicy policy{
        .ttl = kHintTtl,
        .max_failures = kHintMaxFailures,
    };

    size_t evicted_expired = 0;
    size_t evicted_failure = 0;

    std::lock_guard<std::mutex> lock(relay_hints_mutex_);
    for (auto it = relay_hints_by_target_.begin();
         it != relay_hints_by_target_.end();) {
        auto& records = it->second;
        const auto orig = records.size();
        records.erase(
            std::remove_if(records.begin(), records.end(),
                [&](const RelayHintRecord& r) {
                    if (dinero::network::ShouldEvictByTtl(
                            r.learned_at, now, policy)) {
                        ++evicted_expired;
                        std::cout << "[hint] evicted target=" << it->first
                                  << " reason=expired" << std::endl;
                        return true;
                    }
                    if (dinero::network::ShouldEvictByFailure(
                            r.consecutive_dial_failures, policy)) {
                        ++evicted_failure;
                        std::cout << "[hint] evicted target=" << it->first
                                  << " reason=failures count="
                                  << r.consecutive_dial_failures << std::endl;
                        return true;
                    }
                    return false;
                }),
            records.end());
        (void)orig;
        if (records.empty()) {
            it = relay_hints_by_target_.erase(it);
        } else {
            ++it;
        }
    }
    hints_evicted_expired_.fetch_add(evicted_expired);
    hints_evicted_failure_.fetch_add(evicted_failure);
}
```

The atomic counters above are declared in this task; Task 11 only adds the public accessors that expose them via RPC. Also add to `p2p_manager.h`, near other private members:

```cpp
// Phase 1a observability — incremented from sweep + counter paths.
std::atomic<size_t> hints_evicted_expired_{0};
std::atomic<size_t> hints_evicted_failure_{0};
std::atomic<size_t> hints_received_self_{0};
std::atomic<size_t> hints_received_relay_{0};
```

Also add `#include <atomic>` and `#include <algorithm>` if not already present (search first).

- [ ] **Step 3: Call sweep from keepalive_loop**

In `src/daemon/p2p_manager.cpp`, find `void P2PManager::keepalive_loop()` at line 6191. Inside the main loop body, near the existing `relay_registry_.Sweep();` call at line 6222, add a line right after it:

```cpp
SweepRelayHintsCache();
```

- [ ] **Step 4: Build daemon**

```bash
cmake --build build-rc14-quic --target dinerod -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinerod`. Zero new warnings.

- [ ] **Step 5: Commit**

```bash
git add src/daemon/p2p_manager.h src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
feat(relay-hints): SweepRelayHintsCache runs from keepalive_loop

Walks relay_hints_by_target_ on the existing 30s keepalive cadence
(no new thread). Removes entries past kHintTtl=15min OR with
consecutive_dial_failures >= 3. Increments hints_evicted_expired_
and hints_evicted_failure_ atomics. Logs each eviction with reason
and target.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Wire failure counter into orchestrator dial callback

**Files:**
- Modify: `src/daemon/p2p_manager.cpp`

- [ ] **Step 1: Locate the callback**

In `src/daemon/p2p_manager.cpp` at line ~1753, the orchestrator builds a callback:

```cpp
auto callback =
    [this, target_node_id, relay_peer_address, target_hex](
        bool ok, uint64_t circuit_id, const std::string& msg) {
        if (!ok || circuit_id == 0) {
            std::cout << "[P2P] relay-orchestrator: dial via "
                      << relay_peer_address << " to " << target_hex
                      << " failed: " << msg << std::endl;
            return;
        }
```

- [ ] **Step 2: Add failure-counter increment**

Inside the `if (!ok || circuit_id == 0)` branch, BEFORE the existing `return`, add:

```cpp
{
    std::lock_guard<std::mutex> hints_lock(relay_hints_mutex_);
    auto hit = relay_hints_by_target_.find(target_hex);
    if (hit != relay_hints_by_target_.end()) {
        for (auto& r : hit->second) {
            r.consecutive_dial_failures++;
        }
    }
}
```

Then, after the successful path's `start_peer_handler_thread(...)` call (a few lines down, around line 1778), add a successful-handshake reset hook. The cleanest spot: after the install completes, reset the counter for hints that match this target_hex:

```cpp
{
    std::lock_guard<std::mutex> hints_lock(relay_hints_mutex_);
    auto hit = relay_hints_by_target_.find(target_hex);
    if (hit != relay_hints_by_target_.end()) {
        for (auto& r : hit->second) {
            r.consecutive_dial_failures = 0;
        }
    }
}
```

Note: this resets even if the QUIC handshake later fails — for Phase 1a that's acceptable (the next dial attempt's failure callback re-increments). Tighter coupling (reset only after QUIC handshake completes) is a Phase 1c improvement.

- [ ] **Step 3: Build daemon**

```bash
cmake --build build-rc14-quic --target dinerod -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinerod`.

- [ ] **Step 4: Commit**

```bash
git add src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
feat(relay-hints): per-hint failure counter wired into orchestrator

OrchestrateRelayDials's RELAY_CONNECT callback now increments
consecutive_dial_failures on all hint records for the failed target.
Successful install resets the counter. After kHintMaxFailures=3
failures, SweepRelayHintsCache (Task 5) evicts the hint within 30s.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Refresh on duplicate hint ingest

**Files:**
- Modify: `src/daemon/p2p_manager.cpp`

- [ ] **Step 1: Locate hint ingest**

In `src/daemon/p2p_manager.cpp` around line 2169, the RELAY_HINTS ingest currently looks like:

```cpp
auto& bucket = relay_hints_by_target_[key];
// ... bucket.push_back(record) or replace logic
```

- [ ] **Step 2: Read the existing ingest block to verify shape**

Run:

```bash
sed -n '2150,2200p' src/daemon/p2p_manager.cpp
```

Note the exact shape of how `bucket` is mutated. (Likely an unconditional `push_back` or an overwrite pattern.)

- [ ] **Step 3: Change ingest to refresh duplicates**

Replace the bucket insertion with logic that updates an existing matching record instead of duplicating. A matching record is one with the same `(net, relay_addr, relay_port)` triple:

```cpp
auto& bucket = relay_hints_by_target_[key];
bool refreshed = false;
for (auto& existing : bucket) {
    if (existing.net == record.net &&
        existing.relay_addr == record.relay_addr &&
        existing.relay_port == record.relay_port) {
        existing.learned_at = clock_->SteadyNow();
        existing.consecutive_dial_failures = 0;
        refreshed = true;
        break;
    }
}
if (!refreshed) {
    record.learned_at = clock_->SteadyNow();
    record.consecutive_dial_failures = 0;
    bucket.push_back(std::move(record));
}

// Phase 1a observability: source-tagged counter. Self-hint = target_node_id
// equals the sender peer's node_id (or we can mark RelayPush by checking
// whether the sender is one of our configured relay endpoints). Simple
// heuristic for Phase 1a: if sender's peer.is_our_relay then RelayPush
// else Self. Refine in Phase 1c when we add the Gossip source.
if (peer && peer->is_our_relay) {
    hints_received_relay_.fetch_add(1);
} else {
    hints_received_self_.fetch_add(1);
}
```

(The exact local-variable name for the inbound record may differ — match the existing variable name used by the surrounding code. The point of the change: dedupe by `(net, relay_addr, relay_port)` and refresh `learned_at` + clear failure counter.)

- [ ] **Step 4: Build**

```bash
cmake --build build-rc14-quic --target dinerod -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
feat(relay-hints): refresh duplicates instead of stacking + source tag

RELAY_HINTS ingest now dedupes by (net, relay_addr, relay_port) per
target. Duplicates refresh learned_at via clock_ and reset the
failure counter. New atomics hints_received_self_ /
hints_received_relay_ count source category (Phase 1a heuristic:
RelayPush if sender is_our_relay, else Self).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Periodic re-send from keepalive_loop

**Files:**
- Modify: `src/daemon/p2p_manager.h` (declare method + state)
- Modify: `src/daemon/p2p_manager.cpp`

- [ ] **Step 1: Declare method and state**

In `src/daemon/p2p_manager.h` near the `SweepRelayHintsCache()` declaration from Task 5, add:

```cpp
// Phase 1a: re-send our own RELAY_HINTS(target=self) to every
// NODE_DINERO_V2 peer that isn't one of our configured relayregister=
// endpoints. Called from keepalive_loop; gated by kHintResendPeriod
// so the effective cadence is 5min ± 30s.
void MaybeReSendRelayHints();

std::chrono::steady_clock::time_point last_relay_hints_resend_{};
```

- [ ] **Step 2: Implement MaybeReSendRelayHints**

In `src/daemon/p2p_manager.cpp`, near `SweepRelayHintsCache`, add:

```cpp
void P2PManager::MaybeReSendRelayHints() {
    const auto now = clock_->SteadyNow();
    if (now - last_relay_hints_resend_ < kHintResendPeriod) {
        return;
    }
    last_relay_hints_resend_ = now;

    std::vector<std::shared_ptr<PeerInfo>> targets;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [peer_key, peer] : connected_peers_) {
            if (!peer || !peer->is_connected) continue;
            if (peer->is_our_relay) continue;  // no point telling our relay
            if (!(peer->services & dinero::p2p::NODE_DINERO_V2)) continue;
            targets.push_back(peer);
        }
    }

    uint64_t our_services = local_services_;
    for (const auto& peer : targets) {
        SendRelayHintsIfApplicable(peer.get(), our_services);
    }
    if (!targets.empty()) {
        std::cout << "[hint] re-sent RELAY_HINTS to "
                  << targets.size() << " peer(s)" << std::endl;
    }
}
```

(If `local_services_` is named differently — `our_services_`, `services_`, etc. — match the existing field. Search: `grep -n "our_services\|local_services" src/daemon/p2p_manager.cpp | head -5`.)

- [ ] **Step 3: Call from keepalive_loop**

In `src/daemon/p2p_manager.cpp` at the same site where Task 5 added `SweepRelayHintsCache();` (right after `relay_registry_.Sweep();` near line 6222), append:

```cpp
MaybeReSendRelayHints();
```

- [ ] **Step 4: Build daemon**

```bash
cmake --build build-rc14-quic --target dinerod -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/daemon/p2p_manager.h src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
feat(relay-hints): periodic 5min RELAY_HINTS re-send via keepalive_loop

MaybeReSendRelayHints checks elapsed time against kHintResendPeriod=5m
and re-fires SendRelayHintsIfApplicable for each NODE_DINERO_V2 peer
that is not one of our configured relays. Driven by the existing
keepalive_loop — no new thread (avoids QUIC race patterns from
PR #125/#126).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Fleet directory grace (RelayRegistry)

**Files:**
- Modify: `include/network/relay_registry.h`
- Modify: `src/network/relay_registry.cpp`
- Create: `tests/network/test_relay_registry_grace.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Read the existing Registration struct**

```bash
grep -n "struct Registration\|class RelayRegistry\|void Sweep\|std::optional<Registration> Lookup" include/network/relay_registry.h
```

Read the file end-to-end (it's small — under 200 lines typically).

- [ ] **Step 2: Add grace field to Registration**

In `include/network/relay_registry.h`, in the `Registration` struct (or equivalent), append:

```cpp
// Phase 1a directory grace. Set to a future time_point by
// MarkGracePending(node_id, now+kRelayDirectoryGracePeriod) when the
// registrant's underlying peer disconnects. Sweep() drops entries
// whose grace_expires_at < now. Cleared (set to time_point::max())
// when a fresh RELAY_REGISTER for the same node_id arrives.
std::chrono::steady_clock::time_point grace_expires_at{
    std::chrono::steady_clock::time_point::max()};
```

- [ ] **Step 3: Declare MarkGracePending**

In `include/network/relay_registry.h`, in the `RelayRegistry` class public section:

```cpp
// Phase 1a: flag a registration as in its disconnect-grace window.
// If a fresh Register() for the same node_id arrives before
// Sweep() runs past grace_expires_at, the entry is restored.
// Returns true if a matching registration was found and flagged.
bool MarkGracePending(const std::array<uint8_t, 20>& node_id,
                      std::chrono::steady_clock::time_point grace_expires_at);
```

- [ ] **Step 4: Implement MarkGracePending + teach Sweep**

In `src/network/relay_registry.cpp`, locate the existing `Sweep()` method. Add MarkGracePending:

```cpp
bool RelayRegistry::MarkGracePending(
        const std::array<uint8_t, 20>& node_id,
        std::chrono::steady_clock::time_point grace_expires_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(node_id);
    if (it == registrations_.end()) return false;
    it->second.grace_expires_at = grace_expires_at;
    return true;
}
```

Modify `Sweep()` (find it; pattern matches existing function) — at the top of the loop body that visits each registration, before existing TTL/expiry checks, add:

```cpp
const auto now = std::chrono::steady_clock::now();
if (it->second.grace_expires_at < now) {
    it = registrations_.erase(it);
    continue;
}
```

In Register(), clear any existing grace flag on the matching entry:

```cpp
// At the top of Register(), after locating/inserting the entry:
entry.grace_expires_at = std::chrono::steady_clock::time_point::max();
```

(Match the existing variable name for the looked-up entry.)

- [ ] **Step 5: Wire disconnect → MarkGracePending in p2p_manager**

Find where connected_peers_ entries are erased — likely in `cleanup_peer()` or similar:

```bash
grep -n "void.*cleanup_peer\|connected_peers_\.erase\|fn cleanup_peer" src/daemon/p2p_manager.cpp | head -5
```

In `cleanup_peer()` (or whichever function removes a peer from `connected_peers_`), BEFORE the erase, check if the peer was a registrant in `relay_registry_` and flag grace:

```cpp
if (peer && peer->identity_proven) {
    auto reg = relay_registry_.Lookup(peer->their_node_id);
    if (reg.has_value()) {
        relay_registry_.MarkGracePending(
            peer->their_node_id,
            clock_->SteadyNow() + kRelayDirectoryGracePeriod);
    }
}
```

- [ ] **Step 6: Write the failing test**

Create `tests/network/test_relay_registry_grace.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_registry.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>

using dinero::network::RelayRegistry;
using std::chrono::minutes;
using std::chrono::seconds;
using std::chrono::steady_clock;

namespace {
std::array<uint8_t, 20> nid(uint8_t fill) {
    std::array<uint8_t, 20> out{};
    out.fill(fill);
    return out;
}
RelayRegistry::Registration make_reg(const std::array<uint8_t, 20>& id) {
    RelayRegistry::Registration r{};
    r.node_id = id;
    // Fill any other required fields per the existing struct (match
    // the surrounding test patterns in test_addrman_relay_select.cpp).
    return r;
}
}  // namespace

TEST(RelayRegistryGraceTest, mark_grace_pending_then_sweep_evicts_after_window) {
    RelayRegistry reg;
    const auto id = nid(0x42);
    reg.Register(make_reg(id));
    ASSERT_TRUE(reg.Lookup(id).has_value());

    // Flag with 0s grace = expired immediately.
    reg.MarkGracePending(id, steady_clock::time_point{});
    reg.Sweep();
    EXPECT_FALSE(reg.Lookup(id).has_value());
}

TEST(RelayRegistryGraceTest, fresh_register_clears_grace_flag) {
    RelayRegistry reg;
    const auto id = nid(0x55);
    reg.Register(make_reg(id));
    reg.MarkGracePending(id, steady_clock::time_point{});
    // Re-register before sweep:
    reg.Register(make_reg(id));
    reg.Sweep();
    EXPECT_TRUE(reg.Lookup(id).has_value());
}

TEST(RelayRegistryGraceTest, mark_grace_returns_false_for_unknown_node) {
    RelayRegistry reg;
    EXPECT_FALSE(reg.MarkGracePending(nid(0x77), steady_clock::now()));
}
```

- [ ] **Step 7: Register test in CMake**

In `tests/CMakeLists.txt`, after the `test_relay_hints_eviction` block:

```cmake
add_executable(test_relay_registry_grace
  tests/network/test_relay_registry_grace.cpp
  src/network/relay_registry.cpp
)
target_include_directories(test_relay_registry_grace PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_relay_registry_grace PRIVATE GTest::gtest_main)
add_test(NAME RelayRegistryGrace COMMAND test_relay_registry_grace)
set_tests_properties(RelayRegistryGrace PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 5
)
```

- [ ] **Step 8: Build + run**

```bash
cmake -S . -B build-rc14-quic
cmake --build build-rc14-quic --target test_relay_registry_grace dinerod -j8
cd build-rc14-quic && ctest -R RelayRegistryGrace --output-on-failure && cd ..
```

Expected: 3 tests pass; dinerod builds clean.

- [ ] **Step 9: Commit**

```bash
git add include/network/relay_registry.h \
        src/network/relay_registry.cpp \
        src/daemon/p2p_manager.cpp \
        tests/network/test_relay_registry_grace.cpp \
        tests/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(relay-registry): 90s directory grace on registrant disconnect

Registration gains grace_expires_at; MarkGracePending(node_id, when)
flags entries scheduled for eviction. Sweep() (already runs in
keepalive_loop) drops entries past their grace window. Register()
clears the flag if the registrant reconnects before the window
elapses.

P2PManager::cleanup_peer now flags directory grace via
clock_->SteadyNow() + kRelayDirectoryGracePeriod when an
identity-proven peer disconnects.

Three gtests cover: grace-expired eviction, reconnect clears grace,
mark-grace returns false for unknown node.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Verify + regression-test registrant reconnect re-register

**Files:**
- Modify: `tests/network/test_relay_registry_grace.cpp` (extend existing file)

- [ ] **Step 1: Verify the existing reconnect path**

In `src/daemon/p2p_manager.cpp`, find `RefreshRelayRegistrations()` (referenced at line 6223 area):

```bash
grep -n "RefreshRelayRegistrations\|void.*RefreshRelay" src/daemon/p2p_manager.cpp | head -5
```

Read the function body. Confirm that on each iteration it re-sends RELAY_REGISTER to every is_our_relay peer whose register-time exceeds a threshold. This is the existing "reconnect re-registers" mechanism — Phase 1a only requires we verify it works.

Document the verification with a comment ABOVE the existing call site in keepalive_loop (just a one-liner reference). No code change if the existing logic is correct.

- [ ] **Step 2: Add an integration regression check**

Append to `tests/network/test_relay_registry_grace.cpp`:

```cpp
TEST(RelayRegistryGraceTest, reregister_within_grace_window_keeps_entry) {
    RelayRegistry reg;
    const auto id = nid(0xAB);
    reg.Register(make_reg(id));

    // Simulate disconnect → grace flag set for 90s in the "future".
    const auto grace_until = steady_clock::now() + seconds(90);
    reg.MarkGracePending(id, grace_until);

    // Reconnect before grace expires → fresh Register() arrives.
    reg.Register(make_reg(id));

    // Sweep before the original grace window expires (should be inert
    // since grace was cleared by Register()).
    reg.Sweep();
    EXPECT_TRUE(reg.Lookup(id).has_value());
}
```

- [ ] **Step 3: Build + run**

```bash
cmake --build build-rc14-quic --target test_relay_registry_grace -j8
cd build-rc14-quic && ctest -R RelayRegistryGrace --output-on-failure && cd ..
```

Expected: 4 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/network/test_relay_registry_grace.cpp
git commit -S -m "$(cat <<'EOF'
test(relay-registry): assert reconnect within grace window restores entry

Codifies the desired invariant: an entry flagged with MarkGracePending
followed by a fresh Register() inside the grace window survives the
next Sweep(). Catches future regressions in either the grace path or
Register's reset behavior.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Observability — expose counters in getnetworkinfo

**Files:**
- Modify: `src/daemon/rpc/network_rpc.cpp` (or wherever `getnetworkinfo` is built — verify path below)

- [ ] **Step 1: Locate the RPC handler**

```bash
grep -rn "getnetworkinfo\b" src/ include/ --include="*.cpp" --include="*.h" | grep -iE "Handle|handler|implement|response|build" | head -5
```

Open the file that builds the response JSON.

- [ ] **Step 2: Add hint-counter fields to the response**

In the response-building code, find where existing relay-related fields (e.g., `relay_registry_size`, `is_our_relay` counters) are added. Append:

```cpp
response["relay_hints"] = {
    {"received_self", p2p_manager_.HintsReceivedSelf()},
    {"received_relay", p2p_manager_.HintsReceivedRelay()},
    {"evicted_expired", p2p_manager_.HintsEvictedExpired()},
    {"evicted_failure", p2p_manager_.HintsEvictedFailure()},
};
```

(Use whatever JSON library the existing file uses — match exactly.)

- [ ] **Step 3: Add public accessors on P2PManager**

In `src/daemon/p2p_manager.h`, public section:

```cpp
// Phase 1a observability accessors.
size_t HintsReceivedSelf() const {
    return hints_received_self_.load(std::memory_order_relaxed);
}
size_t HintsReceivedRelay() const {
    return hints_received_relay_.load(std::memory_order_relaxed);
}
size_t HintsEvictedExpired() const {
    return hints_evicted_expired_.load(std::memory_order_relaxed);
}
size_t HintsEvictedFailure() const {
    return hints_evicted_failure_.load(std::memory_order_relaxed);
}
```

- [ ] **Step 4: Build + smoke-test**

```bash
cmake --build build-rc14-quic --target dinerod -j8 2>&1 | tail -3
# Start a regtest node briefly and confirm the response includes new fields:
./build-rc14-quic/dinerod -regtest -datadir=/tmp/dinero-regtest-hints-smoke \
  -rpc -rpcport=18999 -p2pport=18998 -daemon
sleep 3
./build-rc14-quic/dinero-cli -regtest -datadir=/tmp/dinero-regtest-hints-smoke \
  -rpcport=18999 getnetworkinfo | grep -A 5 relay_hints
./build-rc14-quic/dinero-cli -regtest -datadir=/tmp/dinero-regtest-hints-smoke \
  -rpcport=18999 stop
rm -rf /tmp/dinero-regtest-hints-smoke
```

Expected: getnetworkinfo output contains:
```
  "relay_hints": {
    "received_self": 0,
    "received_relay": 0,
    "evicted_expired": 0,
    "evicted_failure": 0
  }
```

- [ ] **Step 5: Commit**

```bash
git add src/daemon/p2p_manager.h src/daemon/rpc/network_rpc.cpp
git commit -S -m "$(cat <<'EOF'
feat(rpc): expose RELAY_HINTS Phase 1a counters in getnetworkinfo

Adds relay_hints object with received_self / received_relay /
evicted_expired / evicted_failure. Operators can curl getnetworkinfo
and confirm the lifecycle is working: nonzero received counts on a
running node, growing evicted counts when phantoms/stale entries
clear.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Integration test — 3-node regtest

**Files:**
- Create: `tests/integration/test_relay_hints_lifecycle.sh`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the bash harness**

Create `tests/integration/test_relay_hints_lifecycle.sh`:

```bash
#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Integration test for RELAY_HINTS Phase 1a:
#   - 3-node regtest topology: A (relay), B + C (registrants behind A)
#   - Assert B and C learn about each other through A within 5min
#   - Disconnect B; within 90s + buffer, A drops B from directory
#   - Reconnect B; within one re-send period, C re-learns B

set -euo pipefail

DINEROD="${DINEROD:?DINEROD must point to dinerod binary}"
DINERO_CLI="${DINERO_CLI:?DINERO_CLI must point to dinero-cli binary}"
TMP="$(mktemp -d)"
trap "rm -rf $TMP; pkill -f 'dinerod.*$TMP' || true" EXIT

start_node() {
    local name="$1" port="$2" rpcport="$3" extra="$4"
    mkdir -p "$TMP/$name"
    "$DINEROD" -regtest -datadir="$TMP/$name" \
        -rpc -rpcport="$rpcport" -p2pport="$port" \
        $extra -daemon
}

cli() { "$DINERO_CLI" -regtest -datadir="$TMP/$1" -rpcport="$2" "${@:3}"; }

start_node A 19001 19002 "-listen"
sleep 2
start_node B 19011 19012 "-listen -addnode=127.0.0.1:19001 -relayregister=127.0.0.1:19001"
start_node C 19021 19022 "-listen -addnode=127.0.0.1:19001 -relayregister=127.0.0.1:19001"
sleep 10

# Assert B and C learn each other within the resend period.
DEADLINE=$(( $(date +%s) + 360 ))  # 6 min budget
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    if cli B 19012 getnetworkinfo | grep -q '"relay_hints"' \
       && cli C 19022 getnetworkinfo | grep -q '"relay_hints"'; then
        break
    fi
    sleep 10
done

# Disconnect B; assert A drops within 90s + 30s buffer.
cli B 19012 stop
sleep 120
A_DIR_SIZE=$(cli A 19002 getnetworkinfo | grep -c '"is_our_relay"')
# Expected: A's directory no longer contains B
echo "After 2min, A's directory has $A_DIR_SIZE relays."

# Reconnect B + assert C re-learns within 5min.
start_node B 19011 19012 "-listen -addnode=127.0.0.1:19001 -relayregister=127.0.0.1:19001"
sleep 360
# Inspection only; CI assertions added in follow-up if needed.
cli C 19022 getnetworkinfo | grep -A 5 relay_hints

# Tear down
cli A 19002 stop
cli C 19022 stop
echo "INTEGRATION OK"
```

- [ ] **Step 2: Make executable + add to CMake**

```bash
chmod +x tests/integration/test_relay_hints_lifecycle.sh
```

In `tests/CMakeLists.txt` (near other integration test registrations):

```cmake
if(BUILD_TESTING)
    add_test(NAME RelayHintsLifecycleIntegration
        COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/test_relay_hints_lifecycle.sh)
    set_tests_properties(RelayHintsLifecycleIntegration PROPERTIES
        ENVIRONMENT
            "DINEROD=$<TARGET_FILE:dinerod>;DINERO_CLI=$<TARGET_FILE:dinero-cli>"
        LABELS "network;relay;integration"
        TIMEOUT 900
    )
endif()
```

- [ ] **Step 3: Run it locally**

```bash
cmake --build build-rc14-quic --target dinerod dinero-cli -j8
cd build-rc14-quic && ctest -R RelayHintsLifecycleIntegration --output-on-failure && cd ..
```

Expected: exits 0 with "INTEGRATION OK". Total runtime ~10 min.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_relay_hints_lifecycle.sh tests/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(relay-hints): 3-node regtest integration for Phase 1a lifecycle

Spins up A (relay) + B + C (registrants), asserts hint propagation
happens within the 5min re-send window, B's disconnect triggers
90s-grace eviction in A's directory, and B's reconnect re-arms
discovery from C within the next re-send period. ctest target with
15min TIMEOUT.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Open PR + canary deploy

**Files:** none (process)

- [ ] **Step 1: Push branch**

```bash
git push -u origin feature/relay-hints-lifecycle-1a
```

- [ ] **Step 2: Open PR**

```bash
gh pr create --title "RELAY_HINTS Phase 1a: lifecycle (TTL + failure-counter + directory grace + periodic resend)" --body "$(cat <<'EOF'
## Summary
- Implements Phase 1a of `docs/superpowers/specs/2026-05-23-relay-hints-lifecycle-design.md`
- Receiver: 15min TTL + 3-failure eviction
- Sender: 5min periodic re-send via existing keepalive_loop (NO new thread)
- Fleet directory: 90s grace on registrant disconnect
- ClockSource abstraction for deterministic TTL tests
- Observability counters exposed via getnetworkinfo
- Bug 3c (ngtcp2 ERR_IDLE_CLOSE on inbound QuicSessions) is OUT OF SCOPE — tracked separately
- Phase 1b (v2 wire format + signing) and Phase 1c (bounded gossip) are future PRs

## Test plan
- [ ] `ctest -L smoke` green
- [ ] `ctest -R RelayHintsLifecycleIntegration` green (3-node regtest)
- [ ] VA canary soak ≥60min: confirm `getnetworkinfo` shows
      `relay_hints.received_self > 0`, `relay_hints.evicted_expired`
      grows when stale entries clear, no memory growth in
      `relay_hints_by_target_` over the soak
- [ ] Confirm phantom `fd4fc04df38bacbf72d4ecae451d1589570bcaba`
      evicts from VA's cache within 15min OR within 3 dial failures

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: VA canary soak per `feedback_canary_soak_discipline`**

After PR opens and CI passes:

```bash
# Build on Dell for x86_64 Linux:
ssh tower@192.168.1.108 'cd /home/tower/src/dinero-v8 && \
    git fetch origin feature/relay-hints-lifecycle-1a && \
    git checkout feature/relay-hints-lifecycle-1a && \
    cmake -S . -B build-1a && \
    cmake --build build-1a --target dinerod -j$(nproc)'

# Scp + deploy to VA only (single-node canary, NOT fleet):
ssh tower@192.168.1.108 'scp build-1a/dinerod root@173.249.195.59:/tmp/dinerod-1a'
ssh -i ~/.ssh/dinerova_key root@173.249.195.59 << 'EOF'
cp /usr/bin/dinerod /usr/bin/dinerod.pre-1a-$(date +%s)
systemctl stop dinero.service
sleep 2
cp /tmp/dinerod-1a /usr/bin/dinerod
systemctl start dinero.service
EOF

# Soak 60 min, then check counters:
sleep 3600
ssh -i ~/.ssh/dinerova_key root@173.249.195.59 \
    'dinero-cli -datadir=/var/lib/dinero getnetworkinfo | grep -A 5 relay_hints'
```

Expected: nonzero `received_self`, growing `evicted_expired` or `evicted_failure`. Memory steady (no leak).

If canary clean: fleet rollout (LA → MO → CN) per the standard deploy workflow in `server_deployment.md`. If not clean: investigate, do NOT proceed to fleet.

---

## Coverage map (self-review)

| Spec requirement | Task |
|---|---|
| ClockSource abstraction | 1, 2, 3 |
| `consecutive_dial_failures` field | 4 |
| `kHintTtl=15min`, `kHintMaxFailures=3`, `kHintResendPeriod=5min`, `kRelayDirectoryGracePeriod=90s` constants | 4 |
| `SweepRelayHintsCache` from keepalive_loop | 5 |
| Failure counter increments on RELAY_CONNECT failure | 6 |
| Failure counter resets on success | 6 |
| Refresh `learned_at` on duplicate hint | 7 |
| Source-tagged counters (Self vs RelayPush) | 7, 11 |
| `MaybeReSendRelayHints` from keepalive_loop, no new thread | 8 |
| `RelayRegistry::MarkGracePending` + Sweep grace eviction | 9 |
| `cleanup_peer` flags grace on disconnect | 9 |
| Reconnect re-register works (verify existing) | 10 |
| Logging with source tag | 5, 7 (`[hint] ...` lines) |
| Counters exposed via getnetworkinfo | 11 |
| Unit tests for eviction helpers | 4 |
| Unit tests for ClockSource | 2 |
| Unit tests for grace path | 9, 10 |
| Integration test for 3-node lifecycle | 12 |
| VA canary discipline | 13 |
