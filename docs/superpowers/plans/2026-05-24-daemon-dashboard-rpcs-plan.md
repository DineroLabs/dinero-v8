# Daemon Dashboard RPC Surface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose three pieces of already-computed data via JSON-RPC so the Cmd+K MyNodeDashboard can fill its currently empty cells: local `node_id_hex` (via `getnetworkinfo`), per-peer `ping_ms` + `quality_score` (via `getpeerinfo`), and full Dynamic P2P state (new `dynamic_p2p.observe` method).

**Architecture:** Three additive changes. Extract the existing `BuildDynamicP2PQualitySnapshot` free function from `p2p_service.cpp` into a shared header so both the RPC handler and the P2P service can call it. Add a `get_local_node_id_hex()` accessor on `P2PManager`. New RPC handler in its own file, registered alongside `mining.status` in `rpc_server.cpp`. No new tracking logic — all data sources already exist.

**Tech Stack:** C++20, JsonCpp via `din::Json`, existing daemon-test infrastructure. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-05-24-daemon-dashboard-rpcs-design.md`

**Branch:** `feature/daemon-dashboard-rpcs` off `dinero-main`. Worktree at `/private/tmp/dinero-v8-daemon-rpcs-impl` (Task 0 creates it; the existing `/private/tmp/dinero-v8-daemon-rpcs` worktree holds the spec branch and stays untouched). All commits SSH-signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File map (decomposition locked in before tasks)

| File | Status | Responsibility |
|---|---|---|
| `include/p2p/peer_quality_derivation.h` | **Create** | Inline free function `BuildDynamicP2PQualitySnapshot(const PeerInfo&)` extracted from `p2p_service.cpp`. Shared header so both daemon-side P2P service and core RPC handlers can call it without duplicate-symbol issues. |
| `src/daemon/services/p2p_service.cpp` | Modify | Remove the local definition of `BuildDynamicP2PQualitySnapshot` (lines ~144-170) — call sites at `p2p_service.cpp:377` and `p2p_service.cpp:523` now resolve to the header-inline version. Include the new header. |
| `src/daemon/p2p_manager.h` / `.cpp` | Modify | Add public `std::string get_local_node_id_hex() const` accessor. Reads `node_identity_->get_node_id_bytes()`, hex-encodes lowercase. Empty string if `node_identity_` not yet set. |
| `src/rpc/methods_network_context.cpp` | Modify | Add `node_id_hex` field to the `getnetworkinfo` response builder. Look near where other top-level identity fields (`subversion`, `version`, `protocolversion`) are inserted. |
| `include/rpc/peer_json_utils.h` | Modify | Add `ping_ms` (int from `peer.avg_latency_ms`) and `quality_score` (int from `BuildDynamicP2PQualitySnapshot(peer).score`) to `BuildPeerInfoJson`. Include the new derivation header. |
| `src/daemon/rpc_dynamic_p2p_handlers.cpp` | **Create** | Implementation of `dynamic_p2p.observe` handler. Serializes `PeerQualitySnapshot` per peer + `DynamicP2PGovernorSnapshot` from `P2PService::BuildStatus()`. Mode-aware. |
| `include/daemon/rpc_dynamic_p2p_handlers.h` | **Create** | Public declaration of the handler's entry point. Header so `rpc_server.cpp` can register it. |
| `src/daemon/rpc_server.cpp` | Modify | Register `dynamic_p2p.observe` in `m_method_handlers` map (near `mining.status` at line ~208). |
| `tests/network/test_dashboard_rpcs.sh` | **Create** | Integration smoke against a regtest daemon: asserts the shape of all 3 new fields/RPC. |
| `tests/CMakeLists.txt` | Modify | Register the integration test target. |

**Files NOT changed:**
- `include/p2p/peer_quality.h` (existing PeerQualitySnapshot + PeerQuality class — header stays clean of PeerInfo coupling)
- `src/core/rpc/network_rpc_handlers.cpp` (the dispatcher path that calls `BuildPeerInfoJson` — single source of truth, no per-caller wiring)
- Any consensus, networking, or storage code

---

## Conventions (all tasks)

- **Commits:** SSH-signed as `Dinero Labs <team@dinerolabs.org>`. Verify the first commit: `git log --show-signature -1 --pretty=format:"%h %GS"` must show "Good signature".
- **Commit-message prefix:** `feat(rpc): ...` for new behavior, `refactor(rpc): ...` for extractions, `test(rpc): ...` for test-only.
- **DO NOT run a full `make` or `cmake --build . --target dinerod` in subagent sessions.** Full daemon link can outlive subagent time limits. Build incrementally with `--target dinero_core` or the specific test target only. Parent session runs full daemon build for verification between tasks.
- **No `git push` until Task 9.** Tasks 1-8 are local commits; Task 9 is the single push + PR open.
- **Self-loop discipline:** when adding/calling `BuildDynamicP2PQualitySnapshot`, the function is pure given `PeerInfo` — never holds state. Re-derives on each call.

---

## Task 0: Branch + worktree setup

**Files:** none (git ops only).

- [ ] **Step 1: Create the implementation worktree off dinero-main**

```bash
cd /Users/haydarevich/src/dinero-v8
git fetch origin dinero-main
git worktree add -b feature/daemon-dashboard-rpcs /private/tmp/dinero-v8-daemon-rpcs-impl origin/dinero-main
```

Expected: `Preparing worktree (new branch 'feature/daemon-dashboard-rpcs') ... HEAD is now at <SHA> ...`. Worktree exists at `/private/tmp/dinero-v8-daemon-rpcs-impl`.

- [ ] **Step 2: Verify signing config**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git config --get user.signingkey
git config --get user.email
git config --get gpg.format
git config --get commit.gpgsign
```

Expected:
```
/Users/haydarevich/.ssh/id_ed25519_dinero_signing.pub
team@dinerolabs.org
ssh
true
```

If any line is missing, STOP and reconfigure.

- [ ] **Step 3: Verify the spec branch worktree stays untouched**

```bash
git -C /private/tmp/dinero-v8-daemon-rpcs status
```

Expected: `On branch spec/daemon-dashboard-rpcs ... nothing to commit, working tree clean`. The spec worktree is read-only during this work.

- [ ] **Step 4: Sync submodules in the new worktree**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git submodule update --init --recursive 2>&1 | tail -5
git status --short
```

Expected: empty `git status --short` output. (This avoids the submodule-drift trap previously seen — see PR #139's sanity log.)

---

## Task 1: Extract `BuildDynamicP2PQualitySnapshot` to a shared header

**Files:**
- Create: `include/p2p/peer_quality_derivation.h`
- Modify: `src/daemon/services/p2p_service.cpp` (remove local definition, add include)

- [ ] **Step 1: Read the existing function definition + its call sites**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
sed -n '144,170p' src/daemon/services/p2p_service.cpp
grep -n "BuildDynamicP2PQualitySnapshot" src/daemon/services/p2p_service.cpp
```

Confirm the function spans lines ~144-170 and is called at ~lines 377 and 523. The function body is the source of truth for the migration.

- [ ] **Step 2: Create the new header with the function as `inline`**

Create `include/p2p/peer_quality_derivation.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "p2p/peer_quality.h"
#include "daemon/p2p_manager.h"  // for PeerInfo + ServiceFlags

namespace dinero::p2p {

// Derive a PeerQualitySnapshot from a PeerInfo by applying observable
// events (handshake state, useful headers/blocks, latency, relay
// capability). Pure: holds no state, re-derives on each call.
// Extracted from p2p_service.cpp so both the P2P service and JSON-RPC
// peer-row builder can use the same logic.
inline PeerQualitySnapshot BuildDynamicP2PQualitySnapshot(
        const ::PeerInfo& peer) {
    PeerQuality quality;
    if (peer.is_connected) {
        quality.Apply(PeerQualityEvent::ConnectionSuccess);
    }
    if (peer.protocol_version != 0 || !peer.user_agent.empty()) {
        quality.Apply(PeerQualityEvent::HandshakeSuccess);
    }
    if (peer.synced_headers > 0 || peer.best_known_height > 0 ||
        peer.best_height > 0) {
        quality.Apply(PeerQualityEvent::UsefulHeader);
    }
    if (peer.synced_blocks > 0) {
        quality.Apply(PeerQualityEvent::UsefulBlock);
    }
    if (peer.avg_latency_ms > 0.0) {
        quality.RecordLatency(static_cast<uint32_t>(peer.avg_latency_ms));
    }

    auto snapshot = quality.Snapshot();
    if ((peer.service_flags & dinero::ServiceFlags::NODE_RELAY) != 0 &&
        peer.is_connected) {
        snapshot.relay_successes = 1;
        snapshot.relay_candidate = snapshot.score >= 55;
    }
    return snapshot;
}

}  // namespace dinero::p2p
```

- [ ] **Step 3: Remove the local definition from `p2p_service.cpp` and include the new header**

Open `src/daemon/services/p2p_service.cpp`. Near the top with other includes, add:

```cpp
#include "p2p/peer_quality_derivation.h"
```

Then delete the function definition that currently lives at lines ~144-170 (the entire `dinero::p2p::PeerQualitySnapshot BuildDynamicP2PQualitySnapshot(const PeerInfo& peer) { ... }` block). The call sites at lines ~377 and ~523 will now resolve to the inline version in the header.

- [ ] **Step 4: Confirm the file still compiles**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake -S . -B build-daemon 2>&1 | tail -3
cmake --build build-daemon --target dinero_core -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinero_core` (or similar). No new errors. Pre-existing warnings allowed (especially `-Wreorder-ctor` on `relay_quic_options`).

If a "multiple definition" linker error appears: the function may have leaked from being non-inline. Confirm the `inline` keyword is present in Step 2's header.

- [ ] **Step 5: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add include/p2p/peer_quality_derivation.h src/daemon/services/p2p_service.cpp
git commit -S -m "$(cat <<'EOF'
refactor(rpc): extract BuildDynamicP2PQualitySnapshot to shared header

Lifts the 25-line free function from p2p_service.cpp (line ~144) into
a new inline header so both the P2P service and the upcoming JSON-RPC
peer-row builder can derive PeerQualitySnapshot from a PeerInfo without
duplicate-symbol issues at link time.

No behavior change. The two existing call sites in p2p_service.cpp
(lines ~377 and ~523) now resolve to the header-inline version.

Preparation for surfacing quality_score in getpeerinfo (next task).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git log --show-signature -1 --pretty=format:"%h %GS"
```

Expected output contains "Good signature" for `team@dinerolabs.org`.

---

## Task 2: Add `P2PManager::get_local_node_id_hex()` accessor

**Files:**
- Modify: `src/daemon/p2p_manager.h`
- Modify: `src/daemon/p2p_manager.cpp`

- [ ] **Step 1: Declare the accessor**

In `src/daemon/p2p_manager.h`, near the existing `set_node_identity(...)` declaration (around line 355), add a public-section getter:

```cpp
    // Returns the local node's identity as a 40-character lowercase hex
    // string (20 bytes encoded). Empty string if node_identity_ has not
    // yet been set. Safe to call before set_node_identity().
    std::string get_local_node_id_hex() const;
```

- [ ] **Step 2: Implement the accessor**

In `src/daemon/p2p_manager.cpp`, near the existing `set_node_identity` implementation (search `grep -n "void P2PManager::set_node_identity"`), add the new method:

```cpp
std::string P2PManager::get_local_node_id_hex() const {
    if (!node_identity_) return std::string();
    const auto bytes = node_identity_->get_node_id_bytes();
    static const char kHexChars[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHexChars[b >> 4]);
        out.push_back(kHexChars[b & 0x0F]);
    }
    return out;
}
```

- [ ] **Step 3: Compile-check**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinero_core -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add src/daemon/p2p_manager.h src/daemon/p2p_manager.cpp
git commit -S -m "$(cat <<'EOF'
feat(p2p): add get_local_node_id_hex() accessor on P2PManager

Returns the local node's identity as a 40-char lowercase hex string
(20 bytes encoded), or empty string if node_identity_ has not yet been
set. Safe to call before set_node_identity() — required by the RPC
handler which may be invoked early during startup.

Preparation for surfacing node_id_hex in getnetworkinfo (next task).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add `node_id_hex` to `getnetworkinfo`

**Files:**
- Modify: `src/rpc/methods_network_context.cpp`

- [ ] **Step 1: Locate the getnetworkinfo response builder**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
grep -n "rpc_context_getnetworkinfo\|\"subversion\"\|result\[\"subversion\"\]" src/rpc/methods_network_context.cpp | head -10
```

Find the line that sets `result["subversion"] = ...` inside `rpc_context_getnetworkinfo`. This is the canonical insertion point for new top-level identity fields.

- [ ] **Step 2: Get a P2PManager reference from the ExecutionContext**

Check how other fields access `p2p_manager` in this file:

```bash
grep -n "p2p_manager\|GetP2PManager\|ctx\.p2p" src/rpc/methods_network_context.cpp | head -10
```

The handler should already have access to a `P2PManager*` (via `ctx` or a member). Use the same pattern.

- [ ] **Step 3: Add the field**

Near the `result["subversion"] = ...` line, append:

```cpp
    // Local node identity (20-byte node_id, 40-char lowercase hex).
    // Empty string if node_identity_ has not yet been initialized.
    if (p2p_manager) {
        result["node_id_hex"] = p2p_manager->get_local_node_id_hex();
    } else {
        result["node_id_hex"] = std::string();
    }
```

(Adjust `p2p_manager` to whatever the local variable is named in this function — could be `p2p_mgr`, `ctx.p2p_manager`, etc.)

- [ ] **Step 4: Compile-check**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinero_core -j8 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Smoke-test against a fresh regtest daemon**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinerod -j8 2>&1 | tail -3
TMP=$(mktemp -d)
./build-daemon/dinerod -regtest -datadir="$TMP" -daemon
sleep 3
./build-daemon/dinero-cli -regtest -datadir="$TMP" getnetworkinfo | grep -E "node_id_hex|subversion"
./build-daemon/dinero-cli -regtest -datadir="$TMP" stop
rm -rf "$TMP"
```

Expected: output includes a line like `"node_id_hex" : "ab12cd34..."` (40 lowercase hex chars).

If you see `"node_id_hex" : ""`: node_identity_ wasn't initialized at the time of the call. Check that the regtest startup path calls `set_node_identity` before serving RPCs (it does per `src/daemon/services/p2p_service.cpp:1141`).

- [ ] **Step 6: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add src/rpc/methods_network_context.cpp
git commit -S -m "$(cat <<'EOF'
feat(rpc): getnetworkinfo emits top-level node_id_hex

Adds a 40-char lowercase hex field exposing the local node's identity
via the P2PManager::get_local_node_id_hex() accessor. Empty string if
node_identity_ has not yet been initialized (early startup or test
harness without it).

Additive change — pre-existing consumers ignore unknown fields.
rpc_schema stays "din.rpc.v1".

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Add `ping_ms` + `quality_score` to `BuildPeerInfoJson`

**Files:**
- Modify: `include/rpc/peer_json_utils.h`

- [ ] **Step 1: Add the include for the derivation header**

In `include/rpc/peer_json_utils.h`, near the existing includes (top of file), add:

```cpp
#include "p2p/peer_quality_derivation.h"
```

- [ ] **Step 2: Add the two new fields inside `BuildPeerInfoJson`**

Inside the `BuildPeerInfoJson` function body, just before the `return peer;` line (around line 76), insert:

```cpp
    // Phase 1.5 dashboard surface: ping (from EMA latency in PeerInfo)
    // + quality_score (derived per-call from PeerInfo state). Both fields
    // always present so consumers can rely on key existence.
    peer["ping_ms"] = static_cast<int>(peer_info.avg_latency_ms);
    peer["quality_score"] =
        dinero::p2p::BuildDynamicP2PQualitySnapshot(peer_info).score;
```

- [ ] **Step 3: Compile-check (both core_rpc and the other caller in methods_network_context.cpp)**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinero_core -j8 2>&1 | tail -5
```

Expected: clean build. Both `src/core/rpc/network_rpc_handlers.cpp` and `src/rpc/methods_network_context.cpp` consume `BuildPeerInfoJson` and will pick up the new fields automatically.

- [ ] **Step 4: Smoke-test against the same regtest daemon (with a peer)**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinerod -j8 2>&1 | tail -3
TMP=$(mktemp -d)
./build-daemon/dinerod -regtest -datadir="$TMP/a" -p2pport=19001 -rpcport=19002 -daemon
sleep 2
./build-daemon/dinerod -regtest -datadir="$TMP/b" -p2pport=19011 -rpcport=19012 -addnode=127.0.0.1:19001 -daemon
sleep 5
./build-daemon/dinero-cli -regtest -datadir="$TMP/a" -rpcport=19002 getpeerinfo \
    | python3 -c "import sys,json; p=json.load(sys.stdin); print('first peer ping_ms=', p[0].get('ping_ms'), 'quality_score=', p[0].get('quality_score'))"
./build-daemon/dinero-cli -regtest -datadir="$TMP/a" -rpcport=19002 stop
./build-daemon/dinero-cli -regtest -datadir="$TMP/b" -rpcport=19012 stop
rm -rf "$TMP"
```

Expected: output like `first peer ping_ms= 0 quality_score= 92` (or any numeric value). Both keys present.

- [ ] **Step 5: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add include/rpc/peer_json_utils.h
git commit -S -m "$(cat <<'EOF'
feat(rpc): getpeerinfo emits per-peer ping_ms + quality_score

Each peer entry gains two fields:
- ping_ms: int from PeerInfo.avg_latency_ms (EMA of measured latency,
  0 if no pings have round-tripped yet)
- quality_score: int from BuildDynamicP2PQualitySnapshot(peer).score
  (derived per-call, defaults to 50 = PeerQualitySnapshot's spawn value)

Both fields always present so consumers can rely on key existence.
Additive change — pre-existing peer-row consumers ignore unknown fields.

Single edit point in BuildPeerInfoJson covers all callers (both
src/core/rpc/network_rpc_handlers.cpp and
src/rpc/methods_network_context.cpp).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: New `dynamic_p2p.observe` handler implementation

**Files:**
- Create: `include/daemon/rpc_dynamic_p2p_handlers.h`
- Create: `src/daemon/rpc_dynamic_p2p_handlers.cpp`

- [ ] **Step 1: Find how P2PService exposes BuildStatus**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
grep -nE "Status BuildStatus|class P2PService|p2p_service_->|m_p2p_service\b" src/daemon/rpc_server.cpp src/daemon/services/p2p_service.h | head -15
```

Note how rpc_server.cpp accesses the P2PService instance (likely a member like `m_p2p_service`). You'll use that same accessor in Step 3 to call `BuildStatus()` from the new handler.

Also check the Status struct shape:

```bash
grep -nE "struct Status|Status BuildStatus" include/daemon/services/p2p_service.h | head -3
```

- [ ] **Step 2: Create the header**

Create `include/daemon/rpc_dynamic_p2p_handlers.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <json/value.h>

namespace dinero { namespace daemon { class P2PService; } }
class P2PManager;

namespace dinero::rpc {

// Implements the dynamic_p2p.observe JSON-RPC method.
//
// Returns a snapshot of the Dynamic P2P state: mode (active/observe/off),
// governor counts + candidate lists (when enabled), and the full
// PeerQualitySnapshot for each currently-connected peer.
//
// Never throws and never returns a JSON-RPC error: when DPP is in OFF
// mode or P2PService is unavailable, returns a valid object with
// {enabled: false, mode: "off", governor: null, peers: []}.
Json::Value HandleDynamicP2PObserve(
    dinero::daemon::P2PService* p2p_service,
    P2PManager* p2p_manager);

}  // namespace dinero::rpc
```

- [ ] **Step 3: Create the implementation**

Create `src/daemon/rpc_dynamic_p2p_handlers.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "daemon/rpc_dynamic_p2p_handlers.h"

#include "daemon/p2p_manager.h"
#include "daemon/services/p2p_service.h"
#include "p2p/peer_quality.h"
#include "p2p/peer_quality_derivation.h"

namespace dinero::rpc {

namespace {

Json::Value QualityToJson(const dinero::p2p::PeerQualitySnapshot& q) {
    Json::Value j;
    j["score"]                  = q.score;
    j["connection_successes"]   = static_cast<Json::UInt64>(q.connection_successes);
    j["connection_failures"]    = static_cast<Json::UInt64>(q.connection_failures);
    j["handshake_successes"]    = static_cast<Json::UInt64>(q.handshake_successes);
    j["handshake_failures"]     = static_cast<Json::UInt64>(q.handshake_failures);
    j["useful_headers"]         = static_cast<Json::UInt64>(q.useful_headers);
    j["useful_blocks"]          = static_cast<Json::UInt64>(q.useful_blocks);
    j["stale_height_events"]    = static_cast<Json::UInt64>(q.stale_height_events);
    j["relay_successes"]        = static_cast<Json::UInt64>(q.relay_successes);
    j["relay_failures"]         = static_cast<Json::UInt64>(q.relay_failures);
    j["latency_ms"]             = static_cast<Json::UInt64>(q.latency_ms);
    j["hot_peer_candidate"]     = q.hot_peer_candidate;
    j["relay_candidate"]        = q.relay_candidate;
    return j;
}

Json::Value StringListToJsonArray(const std::vector<std::string>& src) {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : src) arr.append(s);
    return arr;
}

Json::Value DisabledShape(const std::string& mode) {
    Json::Value out;
    out["rpc_schema"] = "din.rpc.v1";
    out["enabled"]    = false;
    out["mode"]       = mode;
    out["governor"]   = Json::nullValue;
    out["peers"]      = Json::Value(Json::arrayValue);
    return out;
}

}  // namespace

Json::Value HandleDynamicP2PObserve(
        dinero::daemon::P2PService* p2p_service,
        P2PManager* p2p_manager) {
    if (!p2p_service || !p2p_manager) {
        return DisabledShape("error");
    }

    const std::string mode = p2p_service->DynamicP2PMode();
    const bool enabled     = p2p_service->IsDynamicP2PActive();

    if (mode == "off" || !enabled) {
        return DisabledShape(mode);
    }

    Json::Value out;
    out["rpc_schema"] = "din.rpc.v1";
    out["enabled"]    = enabled;
    out["mode"]       = mode;

    // Governor snapshot: read the BuildStatus() result and serialize
    // the dynamic_p2p_governor sub-struct.
    auto status = p2p_service->BuildStatus();

    Json::Value governor;
    governor["available"]                  = status.dynamic_p2p_governor.available;
    governor["mode"]                       = status.dynamic_p2p_governor.mode;
    governor["candidate_source"]           = status.dynamic_p2p_governor.candidate_source;
    governor["connected_outbound"]         = static_cast<Json::UInt64>(status.dynamic_p2p_governor.connected_outbound);
    governor["configured_seed_hot"]        = static_cast<Json::UInt64>(status.dynamic_p2p_governor.configured_seed_hot);
    governor["relay_capable_seen"]         = static_cast<Json::UInt64>(status.dynamic_p2p_governor.relay_capable_seen);
    governor["hot_peers"]                  = StringListToJsonArray(status.dynamic_p2p_governor.hot_peers);
    governor["warm_candidates"]            = StringListToJsonArray(status.dynamic_p2p_governor.warm_candidates);
    governor["relay_registration_candidates"] =
        StringListToJsonArray(status.dynamic_p2p_governor.relay_registration_candidates);
    governor["demote_candidates"]          = StringListToJsonArray(status.dynamic_p2p_governor.demote_candidates);
    out["governor"] = governor;

    // Per-peer snapshot: iterate connected peers, derive quality.
    Json::Value peers(Json::arrayValue);
    {
        auto peer_snapshot = p2p_manager->GetConnectedPeers();  // pattern below
        for (const auto& peer : peer_snapshot) {
            Json::Value row;
            row["addr"] = peer.address + ":" + std::to_string(peer.port);
            row["quality"] = QualityToJson(
                dinero::p2p::BuildDynamicP2PQualitySnapshot(peer));
            peers.append(row);
        }
    }
    out["peers"] = peers;

    return out;
}

}  // namespace dinero::rpc
```

**Note on `p2p_manager->GetConnectedPeers()`:** if P2PManager exposes a different method to enumerate peers (e.g., `getPeers()`, `SnapshotConnectedPeers()`, or a const-ref to a member map), substitute the actual API. Investigate with:

```bash
grep -nE "GetConnectedPeers|getPeers|SnapshotPeers|connected_peers_" src/daemon/p2p_manager.h | head -10
```

If no public peer-enumeration accessor exists, add a small one returning `std::vector<PeerInfo>` (read-only snapshot under `peers_mutex_`). Document the addition in the commit message as part of this task.

- [ ] **Step 4: Add the new source files to CMake**

Find where dinero_core lists its sources:

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
grep -n "src/daemon/rpc_server.cpp\|src/daemon/rpc_mining_implementation.cpp" CMakeLists.txt src/p2p/CMakeLists.txt 2>/dev/null | head -5
```

Append `src/daemon/rpc_dynamic_p2p_handlers.cpp` to the same source list (right after the `rpc_mining_implementation.cpp` entry if you can find it; otherwise alongside `rpc_server.cpp`).

- [ ] **Step 5: Compile-check**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake -S . -B build-daemon 2>&1 | tail -3
cmake --build build-daemon --target dinero_core -j8 2>&1 | tail -5
```

Expected: clean build.

If `GetConnectedPeers` is unresolved: add a small accessor on P2PManager in this same commit (don't split). Pattern:

```cpp
// in p2p_manager.h public section:
std::vector<PeerInfo> GetConnectedPeersSnapshot() const;

// in p2p_manager.cpp:
std::vector<PeerInfo> P2PManager::GetConnectedPeersSnapshot() const {
    std::vector<PeerInfo> out;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    out.reserve(connected_peers_.size());
    for (const auto& [_, peer_ptr] : connected_peers_) {
        if (peer_ptr) out.push_back(*peer_ptr);
    }
    return out;
}
```

- [ ] **Step 6: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add include/daemon/rpc_dynamic_p2p_handlers.h \
        src/daemon/rpc_dynamic_p2p_handlers.cpp \
        CMakeLists.txt
# Also add p2p_manager.{h,cpp} if you needed to add GetConnectedPeersSnapshot.
git status --short  # verify only expected files
git commit -S -m "$(cat <<'EOF'
feat(rpc): dynamic_p2p.observe handler — implementation

New file: src/daemon/rpc_dynamic_p2p_handlers.cpp implements the
dynamic_p2p.observe JSON-RPC method. Surfaces:
- enabled (bool) + mode (active_slow_churn/observe/off)
- governor: full DynamicP2PGovernorSnapshot serialization including
  hot_peers/warm_candidates/relay_registration_candidates/demote_candidates
  as JSON arrays (matching the underlying vector<string> shape, not
  integer counts as the early spec draft suggested)
- peers: per-connected-peer { addr, quality: <full PeerQualitySnapshot> }
  using BuildDynamicP2PQualitySnapshot from the shared header

Mode-aware: in off-mode (or when P2PService unavailable) returns
{enabled:false, mode:"off", governor:null, peers:[]} — never errors.

Registration in rpc_server.cpp lands in the next task.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Register `dynamic_p2p.observe` in `rpc_server.cpp`

**Files:**
- Modify: `src/daemon/rpc_server.cpp`

- [ ] **Step 1: Add the include**

In `src/daemon/rpc_server.cpp`, near the existing `#include "daemon/..."` block (top of file), add:

```cpp
#include "daemon/rpc_dynamic_p2p_handlers.h"
```

- [ ] **Step 2: Register the method**

Find the existing `mining.status` registration:

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
grep -n "mining.status" src/daemon/rpc_server.cpp | head -3
```

Immediately after the `mining.status` registration block (matching the same lambda+`rpc::adaptJson` pattern), append:

```cpp
    m_method_handlers["dynamic_p2p.observe"] = rpc::adaptJson(
        [this](const Json::Value& /*params*/) -> Json::Value {
            return dinero::rpc::HandleDynamicP2PObserve(
                m_p2p_service.get(),
                m_p2p_service ? m_p2p_service->p2p_manager() : nullptr);
        });
```

(Adjust `m_p2p_service` to whatever the actual member name is — could be `p2p_service_`, `p2pService_`, etc. Adjust `->p2p_manager()` to whatever accessor returns the P2PManager — could be `->GetP2PManager()`, `->p2p_mgr()`, etc. Confirm with `grep -n "P2PService\|p2p_service" src/daemon/rpc_server.h src/daemon/rpc_server.cpp | head -10`.)

- [ ] **Step 3: Compile-check**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinerod -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinerod`. (This is the FULL daemon link — may take 5-10 min if it hasn't been done yet in this worktree. OK to wait; subsequent rebuilds are fast.)

- [ ] **Step 4: Smoke-test against fresh regtest daemon**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
TMP=$(mktemp -d)
./build-daemon/dinerod -regtest -datadir="$TMP" -daemon
sleep 3
echo "=== off-mode baseline (default config) ==="
./build-daemon/dinero-cli -regtest -datadir="$TMP" dynamic_p2p.observe | python3 -m json.tool | head -10
echo "=== help shows the new method? ==="
./build-daemon/dinero-cli -regtest -datadir="$TMP" help | grep dynamic_p2p
./build-daemon/dinero-cli -regtest -datadir="$TMP" stop
rm -rf "$TMP"
```

Expected: `dynamic_p2p.observe` returns valid JSON with `"enabled": false, "mode": "off", "governor": null, "peers": []`.

- [ ] **Step 5: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add src/daemon/rpc_server.cpp
git commit -S -m "$(cat <<'EOF'
feat(rpc): register dynamic_p2p.observe in rpc_server

Wires the dynamic_p2p.observe method into m_method_handlers using the
same adaptJson lambda pattern as mining.status. Handler implementation
lives in src/daemon/rpc_dynamic_p2p_handlers.cpp.

After this commit the method is callable via JSON-RPC, dinero-cli,
and shows up in help output.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Integration smoke test

**Files:**
- Create: `tests/network/test_dashboard_rpcs.sh`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the smoke script**

Create `tests/network/test_dashboard_rpcs.sh`:

```bash
#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Smoke test for the 3 dashboard RPC additions:
#   1. getnetworkinfo.node_id_hex (40-char hex)
#   2. getpeerinfo[].ping_ms + .quality_score (always present)
#   3. dynamic_p2p.observe (valid shape in off-mode)

set -euo pipefail

DINEROD="${DINEROD:?DINEROD must point to dinerod binary}"
DINERO_CLI="${DINERO_CLI:?DINERO_CLI must point to dinero-cli binary}"
TMP="$(mktemp -d)"
trap "$DINERO_CLI -regtest -datadir=$TMP/a -rpcport=19002 stop 2>/dev/null || true; \
      $DINERO_CLI -regtest -datadir=$TMP/b -rpcport=19012 stop 2>/dev/null || true; \
      sleep 2; rm -rf $TMP" EXIT

mkdir -p "$TMP/a" "$TMP/b"

# Spin two regtest nodes so getpeerinfo has at least one entry to assert on.
"$DINEROD" -regtest -datadir="$TMP/a" -p2pport=19001 -rpcport=19002 -listen -daemon
sleep 2
"$DINEROD" -regtest -datadir="$TMP/b" -p2pport=19011 -rpcport=19012 \
    -addnode=127.0.0.1:19001 -daemon
sleep 6

cli_a() { "$DINERO_CLI" -regtest -datadir="$TMP/a" -rpcport=19002 "$@"; }

# ─── Assertion 1: getnetworkinfo.node_id_hex ────────────────────────────────
NODE_ID=$(cli_a getnetworkinfo | python3 -c \
    "import sys,json; print(json.load(sys.stdin).get('node_id_hex','MISSING'))")
case "$NODE_ID" in
    MISSING) echo "FAIL: getnetworkinfo missing node_id_hex"; exit 1 ;;
    "")      echo "FAIL: node_id_hex is empty (node_identity_ not initialized?)"; exit 1 ;;
esac
[ "${#NODE_ID}" = "40" ] || { echo "FAIL: node_id_hex length is ${#NODE_ID}, want 40"; exit 1; }
echo "PASS: node_id_hex = $NODE_ID"

# ─── Assertion 2: getpeerinfo[].ping_ms + .quality_score ────────────────────
PEER_FIELDS=$(cli_a getpeerinfo | python3 -c "
import sys,json
peers = json.load(sys.stdin)
if not peers: print('NO_PEERS'); sys.exit(0)
p = peers[0]
have_ping  = 'ping_ms' in p
have_score = 'quality_score' in p
print(f'ping_ms={p.get(\"ping_ms\",\"MISSING\")} quality_score={p.get(\"quality_score\",\"MISSING\")} both_present={have_ping and have_score}')
")
echo "$PEER_FIELDS"
echo "$PEER_FIELDS" | grep -q "both_present=True" || { echo "FAIL: peer fields not both present"; exit 1; }
echo "PASS: getpeerinfo has ping_ms + quality_score"

# ─── Assertion 3: dynamic_p2p.observe shape ─────────────────────────────────
cli_a dynamic_p2p.observe | python3 -c "
import sys,json
d = json.load(sys.stdin)
assert 'enabled' in d and isinstance(d['enabled'], bool), 'enabled missing/wrong type'
assert 'mode'    in d and isinstance(d['mode'], str),     'mode missing/wrong type'
assert 'peers'   in d and isinstance(d['peers'], list),   'peers missing/wrong type'
assert 'governor' in d, 'governor key missing'  # may be null in off-mode
print(f'PASS: dynamic_p2p.observe enabled={d[\"enabled\"]} mode={d[\"mode\"]} peers={len(d[\"peers\"])}')
"

echo ""
echo "=== ALL ASSERTIONS PASS ==="
```

- [ ] **Step 2: Make executable + register in CMake**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
chmod +x tests/network/test_dashboard_rpcs.sh
```

In `tests/CMakeLists.txt`, near other shell-based integration tests:

```cmake
if(BUILD_TESTING)
    add_test(NAME DashboardRpcsSmoke
        COMMAND ${CMAKE_SOURCE_DIR}/tests/network/test_dashboard_rpcs.sh)
    set_tests_properties(DashboardRpcsSmoke PROPERTIES
        ENVIRONMENT
            "DINEROD=$<TARGET_FILE:dinerod>;DINERO_CLI=$<TARGET_FILE:dinero-cli>"
        LABELS "network;rpc;integration"
        TIMEOUT 60
    )
endif()
```

- [ ] **Step 3: Run the smoke**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinerod dinero-cli -j8 2>&1 | tail -3
cd build-daemon && ctest -R DashboardRpcsSmoke --output-on-failure
```

Expected: `=== ALL ASSERTIONS PASS ===` and ctest reports "1 test passed".

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git add tests/network/test_dashboard_rpcs.sh tests/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(rpc): integration smoke for the 3 dashboard RPC additions

Spins up a 2-node regtest topology + asserts:
- getnetworkinfo.node_id_hex is present and exactly 40 hex chars
- getpeerinfo[0] has both ping_ms and quality_score keys
- dynamic_p2p.observe returns valid {enabled, mode, governor, peers}
  shape (in default off-mode)

ctest registration uses generator-expression paths so DINEROD/
DINERO_CLI env always point at the just-built binaries.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Full build + manual canary

**Files:** none (verification only)

- [ ] **Step 1: Full dinerod build**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cmake --build build-daemon --target dinerod -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinerod`. Zero new warnings on lines you touched.

- [ ] **Step 2: Run the smoke test + any other relay-area ctest that exists**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl/build-daemon
ctest -L "rpc" --output-on-failure 2>&1 | tail -20
```

Expected: all relay/rpc-labeled tests pass.

- [ ] **Step 3: Manual canary against your live daemon**

Drop the new binary in place on your local Mac (NOT the fleet — single-node canary per `feedback_canary_soak_discipline`):

```bash
# Back up + swap in
cp /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod \
   /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod.pre-dashboard-rpcs-$(date +%s)
cp /private/tmp/dinero-v8-daemon-rpcs-impl/build-daemon/dinerod \
   /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod
# Restart (use rpc stop, never hard kill — per feedback_daemon_shutdown.md)
/Applications/dinero-qt.app/Contents/Resources/dinero-cli -datadir="$HOME/Library/Application Support/Dinero" -rpcport=20998 stop
sleep 5
# Relaunch in the same way you originally did
# (...your usual dinerod launch line...)
```

- [ ] **Step 4: Confirm the 3 RPCs return the expected data on the live daemon**

```bash
CLI="/Applications/dinero-qt.app/Contents/Resources/dinero-cli"
echo "=== node_id_hex ==="
$CLI getnetworkinfo | grep node_id_hex
echo "=== per-peer ==="
$CLI getpeerinfo | python3 -c "import sys,json; ps=json.load(sys.stdin); print(ps[0].get('ping_ms'), ps[0].get('quality_score'))"
echo "=== dynamic_p2p.observe ==="
$CLI dynamic_p2p.observe | python3 -m json.tool | head -15
```

Expected: real values (node_id matches what the fleet knows about your node; ping_ms is a small positive integer; quality_score is 50+; dynamic_p2p.observe returns a populated `peers` array if DPP is in observe/active mode).

- [ ] **Step 5: Rollback the live daemon to the pre-canary binary (keep canary on disk for PR review)**

```bash
cp /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod.pre-dashboard-rpcs-* \
   /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod
# Restart with RPC stop + relaunch
```

The canary binary stays in `/private/tmp/dinero-v8-daemon-rpcs-impl/build-daemon/dinerod` for any further review or re-test. We're not deploying to fleet in this plan — that's a separate decision after PR review.

- [ ] **Step 6: Commit a short sanity log**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
cat > docs/superpowers/plans/2026-05-24-daemon-dashboard-rpcs-sanity.md << EOF
# Daemon Dashboard RPCs — Sanity Log

**Date:** $(date -u +%FT%TZ)
**Branch:** \`feature/daemon-dashboard-rpcs\`

## Results

| Check | Result |
|---|---|
| Full \`dinerod\` build | \`[100%] Built target dinerod\` |
| ctest integration smoke (\`DashboardRpcsSmoke\`) | PASS — 3 assertions |
| Live canary: getnetworkinfo.node_id_hex | 40-char hex on live daemon |
| Live canary: getpeerinfo.ping_ms + quality_score | Both keys present, non-zero quality_score |
| Live canary: dynamic_p2p.observe | Valid shape, mode reflects config |

## Rollback

Pre-canary binary preserved at \`/Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod.pre-dashboard-rpcs-<timestamp>\`. Live daemon restored to pre-canary binary; new binary stays in \`/private/tmp/dinero-v8-daemon-rpcs-impl/build-daemon/dinerod\` for reviewer test.
EOF
git add docs/superpowers/plans/2026-05-24-daemon-dashboard-rpcs-sanity.md
git commit -S -m "$(cat <<'EOF'
docs: daemon dashboard RPCs sanity log

Records the full build + ctest + live-canary results before PR.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Push branch + open PR

**Files:** none (process).

- [ ] **Step 1: Push the branch**

```bash
cd /private/tmp/dinero-v8-daemon-rpcs-impl
git push -u origin feature/daemon-dashboard-rpcs
```

- [ ] **Step 2: Open draft PR**

```bash
gh pr create --draft --title "feat(rpc): expose node_id + per-peer ping + dynamic_p2p.observe for dashboard" --body "$(cat <<'EOF'
## Summary

Three additive RPC changes that surface data already computed internally but unreachable via JSON-RPC. Closes the "—" gaps in PR #139's [MyNodeDashboard](https://github.com/DineroLabs/dinero-v8/pull/139) without any new tracking logic.

| Change | What |
|---|---|
| `getnetworkinfo` gains `node_id_hex` | Top-level 40-char lowercase hex string from `node_identity_->get_node_id_bytes()` via new `P2PManager::get_local_node_id_hex()` accessor |
| `getpeerinfo` gains per-peer `ping_ms` + `quality_score` | `ping_ms` from `PeerInfo.avg_latency_ms`, `quality_score` from `BuildDynamicP2PQualitySnapshot(peer).score` (function extracted to shared header `include/p2p/peer_quality_derivation.h` so both P2P service and RPC handlers use the same logic) |
| New `dynamic_p2p.observe` RPC | Mode-aware (works in off-mode returning `{enabled:false, mode:"off", governor:null, peers:[]}`). Serializes full `PeerQualitySnapshot` per peer + `DynamicP2PGovernorSnapshot` from `P2PService::BuildStatus()`. Governor sub-objects emit `vector<string>` fields as JSON arrays |

## Spec

`docs/superpowers/specs/2026-05-24-daemon-dashboard-rpcs-design.md` (on `spec/daemon-dashboard-rpcs` branch — merge alongside this PR).

## Test plan

- [x] **Integration smoke** (`ctest -R DashboardRpcsSmoke`) — 2-node regtest topology asserts all 3 new fields/RPC shapes
- [x] **Full dinerod build clean** — `[100%] Built target dinerod` on macOS arm64
- [x] **Live canary** — pre-staged binary returns real values for all 3; rolled back to pre-canary binary after verification
- [ ] **CI** — full Tests + core-heavy lanes (will run on push)
- [ ] **Fleet rollout** — out of scope; consumer (dinero-qt PR #139) gets the new RPCs once a daemon built from this branch is deployed

## Backward compatibility

All three changes are additive. No existing field renamed or removed. `rpc_schema` stays `"din.rpc.v1"`. v1 consumers ignore unknown fields.

## What's NOT in this PR

- Per-peer ping/latency tracking (already exists as `PeerInfo.avg_latency_ms` — we just surface it)
- New Dynamic P2P features (just exposing the existing governor + per-peer quality)
- Any dinero-qt change (consumer wire-up lands in a follow-up Qt PR once this merges)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Wait for CI + mark ready**

```bash
# Poll CI every 90s up to ~20 min
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
  STATUS=$(gh pr checks $(gh pr view --json number --jq .number) --repo DineroLabs/dinero-v8 2>&1)
  if echo "$STATUS" | grep -qE "fail|FAILURE"; then
    echo "FAILURE at iter $i:"; echo "$STATUS" | head -10; break
  fi
  if [ "$(echo "$STATUS" | grep -c "pending")" = "0" ]; then
    echo "all checks complete"; echo "$STATUS"; break
  fi
  echo "iter $i: still pending"; sleep 90
done
gh pr ready
```

Stop here. Don't merge. Phase 1.5 daemon RPCs land when the human reviewer approves.

---

## Coverage map (self-review)

| Spec requirement | Task |
|---|---|
| `getnetworkinfo.node_id_hex` top-level field | 2 + 3 |
| `getpeerinfo.ping_ms` per peer | 4 |
| `getpeerinfo.quality_score` per peer | 4 |
| New `dynamic_p2p.observe` RPC | 5 + 6 |
| DPP off-mode graceful shape (`enabled:false, mode:"off"`) | 5 (`DisabledShape` helper) |
| Governor sub-struct exposes full `vector<string>` lists as arrays | 5 (`StringListToJsonArray`) |
| Per-peer `PeerQualitySnapshot` (all 13 fields) | 5 (`QualityToJson`) |
| Backward compat: additive only, no schema bump | All — verified no field removed/renamed |
| No new tracking logic, surface-only | Tasks 1-5 — each cites existing source |
| `rpc_schema: "din.rpc.v1"` preserved | 5 (literal in `out["rpc_schema"]`) |
| Integration smoke test | 7 |
| Manual canary discipline | 8 |
| Single PR (not split per spec) | 9 |
