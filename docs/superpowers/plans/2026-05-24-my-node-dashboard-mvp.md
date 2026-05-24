# MyNodeDashboard — Phase 1 (MVP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `AiPanel` as the primary Cmd+K target with a tabbed `CmdKPanel` whose default tab is a 3-section operator-grade dashboard (Identity · Network · Peers), polled at 5s cadence via the existing `RpcClient`. AI panel is demoted to a tab inside the same container — preserved, not removed.

**Architecture:** New `CmdKPanel` container reuses the existing slide-in animation (`QPropertyAnimation` on `panelWidth` property) currently in `AiPanel`. A `QStackedWidget` switches between two registered tabs: `MyNodeDashboard` (default) and `AiPanel` (existing). A single `NodePoller` per `MyNodeDashboard` drives all three sections via typed Qt signals; sections never call the daemon directly. Phase 1 ships zero daemon changes — everything backed by `getnetworkinfo` / `getpeerinfo` / `getblockchaininfo` / `getrelayinfo` / `getmempoolinfo`.

**Tech Stack:** C++20, Qt 6 (Widgets + Network + Test), CMake. No gtest in Qt code — tests use `QtTest` framework + `Qt6::Test` link. Codebase convention is **flat `qt/src/` layout** (no subdirs).

**Spec:** `docs/superpowers/specs/2026-05-24-my-node-dashboard-design.md`

**Branch:** `feature/my-node-dashboard-mvp` off `dinero-main`. Worktree at `/private/tmp/dinero-v8-mnd-mvp` (created in Task 0). All commits SSH-signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File map (lock decomposition before tasks)

| File | Status | Responsibility |
|---|---|---|
| `qt/src/nodepoller.h` / `.cpp` | **Create** | 5s `QTimer` polling driver. Issues parallel RPCs via `RpcClient`, parses results, emits typed signals: `identityUpdated`, `chainInfoUpdated`, `peersUpdated`. No UI logic. |
| `qt/src/dashboardtypes.h` | **Create** | Plain value structs (`NodeIdentity`, `ChainInfo`, `PeerRow`) shared by `NodePoller` + sections. Header-only. |
| `qt/src/identitysection.h` / `.cpp` | **Create** | ⚡ YOU section — node_id + posture lines. Listens to `NodePoller::identityUpdated`. |
| `qt/src/networksection.h` / `.cpp` | **Create** | 📡 NETWORK section — chain tip race + peer-height histogram + difficulty/mempool/fee grid. Listens to `chainInfoUpdated` + `peersUpdated`. |
| `qt/src/peerssection.h` / `.cpp` | **Create** | 🛰 PEERS section — sortable table, click-to-expand row. Listens to `peersUpdated`. |
| `qt/src/mynodedashboard.h` / `.cpp` | **Create** | Composes `NodePoller` + 3 section widgets inside a `QScrollArea`. |
| `qt/src/cmdkpanel.h` / `.cpp` | **Create** | Top-level slide-in container. Owns the `QPropertyAnimation` (moved from `AiPanel`'s direct animation). Holds a `QTabBar` + `QStackedWidget` with two tabs: Dashboard (default) + AI. |
| `qt/src/mainwindow.cpp` | Modify | Replace `aiPanel_` instantiation with `cmdKPanel_`. Cmd+K toggles `cmdKPanel_`. Existing `AiPanel` instance is now owned by `CmdKPanel`, not `MainWindow` directly. |
| `qt/src/mainwindow.h` | Modify | Replace `AiPanel* aiPanel_` member with `CmdKPanel* cmdKPanel_`. Forward-decl `CmdKPanel`. |
| `qt/CMakeLists.txt` | Modify | Add new sources to the dinero-qt executable target. Register 4 new ctest targets. |
| `qt/tests/test_node_poller.cpp` | **Create** | Tests that `NodePoller` emits the right signal shapes given canned RPC responses. |
| `qt/tests/test_identity_section.cpp` | **Create** | Snapshot tests: given a `NodeIdentity` value, asserts the rendered glyphs + labels. |
| `qt/tests/test_network_section.cpp` | **Create** | Asserts tip-race bar lengths and "+N ahead" / "in sync" / "-N behind" annotations. |
| `qt/tests/test_peers_section.cpp` | **Create** | Asserts default Q-descending sort, stoplight glyph per Q range, row expand/collapse. |

`AiPanel` is NOT modified — only re-parented. Its existing `togglePanel()` method becomes internal-use-only (called by `CmdKPanel` when the AI tab is selected).

---

## Conventions used in every task

- **All commits** signed: `git commit -S -m "..."`. Verify first commit: `git log --show-signature -1 --pretty=format:"%h %GS"` must show "Good signature" for `team@dinerolabs.org`.
- **Author:** `Dinero Labs <team@dinerolabs.org>` (per-repo config already set).
- **Commit message prefix:** `feat(qt-dashboard): ...` for behavior, `test(qt-dashboard): ...` for test-only, `refactor(qt): ...` for restructuring (Cmd+K rewire).
- **DO NOT build the full dinero-qt app** inside subagent sessions — full Qt builds can outlive subagent time limits. Build only the test target(s) you're testing. The parent controller runs the full app build between tasks.
- **DO NOT push the branch yet.** Tasks 1-10 are local commits; Task 11 is the single push + PR open.
- **No tests use raw `time_point` / `duration` in `EXPECT_EQ`/`QCOMPARE`** — extract `.count()` or use integer values. (Same macOS SDK 26.4 trap as the dinero-v8 work — applies to Qt tests too.)

---

## Task 0: Branch setup + worktree

**Files:** none (worktree + branch ops only)

- [ ] **Step 1: Create isolated worktree off dinero-main**

```bash
cd /Users/haydarevich/src/dinero-v8
git fetch origin dinero-main
git worktree add -b feature/my-node-dashboard-mvp /private/tmp/dinero-v8-mnd-mvp origin/dinero-main
```

Expected: `Preparing worktree (new branch 'feature/my-node-dashboard-mvp') ... HEAD is now at <SHA> <message>`. Worktree exists at `/private/tmp/dinero-v8-mnd-mvp`.

- [ ] **Step 2: Verify signing config**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
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

If any line is missing, STOP and reconfigure. Every commit on this branch must be signed.

- [ ] **Step 3: Verify Qt code is at qt/ and existing tests run**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
ls qt/src/aipanel.h qt/src/mainwindow.cpp qt/CMakeLists.txt qt/tests/test_wallet_name_utils.cpp
```

Expected: all four files exist.

---

## Task 1: dashboardtypes.h — shared value structs

**Files:**
- Create: `qt/src/dashboardtypes.h`

- [ ] **Step 1: Write the header**

Create `qt/src/dashboardtypes.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QString>
#include <QVector>
#include <chrono>
#include <cstdint>

namespace dinero::qt::dashboard {

// What we know about THIS node. Populated from getnetworkinfo +
// getrelayinfo + getmininginfo + getuptime.
struct NodeIdentity {
    QString node_id_hex;            // 40 chars (20 bytes hex), lower-case
    QString subversion;             // e.g. "/dinerod:b06ec828/"
    int     version{0};             // numeric protocol version
    quint64 services{0};            // service flags bitmap
    QString local_addr;             // best-known local advertised address
    quint16 local_port{0};
    enum Reachability { UNKNOWN, DIRECT, BEHIND_RELAY, UNREACHABLE };
    Reachability reachability{UNKNOWN};
    bool    is_relay_active{false};
    int     registrants_count{0};
    int     grace_count{0};
    bool    is_mining{false};
    QString mining_destination;     // e.g. "EpycOne address" or pool URL
    double  shares_per_min{0.0};
    std::chrono::seconds uptime{0};
};

// What we know about THE NETWORK (as observed from this node).
// Populated from getblockchaininfo + getpeerinfo (height distribution)
// + getmempoolinfo.
struct ChainInfo {
    qint64  our_height{0};
    qint64  net_consensus_height{0};   // mode of peers' reported heights
    qint64  max_peer_height{0};
    QVector<qint64> peer_heights;      // raw peer heights for histogram
    quint32 difficulty_compact{0};     // nBits
    int     mempool_tx_count{0};
    qint64  mempool_bytes{0};
    qint64  median_fee_una_per_vbyte{0};
    double  next_bits_delta_pct{0.0};  // optional; 0 if not available
};

// One row in the peers table.
struct PeerRow {
    QString addr;                  // "1.2.3.4:20999" or "relay:<id>:<circ>"
    bool    via_relay{false};
    QString relay_via_addr;        // "1.2.3.4:20999" of the relay we go through
    bool    is_inbound{false};
    QString fleet_name;            // "LA"/"VA"/"MO"/"CN" if a known fleet IP, else empty
    qint64  height{-1};
    qint64  ping_ms{-1};            // -1 = unmeasured
    int     quality_score{-1};      // 0..100; -1 = no DPP score yet
    bool    handshake_complete{true};
    bool    stalling{false};
    // Expanded-row data
    quint64 services{0};
    QString subversion;
    qint64  bytes_sent{0};
    qint64  bytes_recv{0};
    std::chrono::seconds connected_for{0};
    std::chrono::seconds last_message_ago{0};
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/dashboardtypes.h
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): add shared value structs for MyNodeDashboard

NodeIdentity, ChainInfo, PeerRow — plain header-only structs shared
between NodePoller and the section widgets. Lives in
dinero::qt::dashboard namespace.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git log --show-signature -1 --pretty=format:"%h %GS"
```

Expected: contains "Good signature" for `team@dinerolabs.org`.

---

## Task 2: NodePoller — interface + signal contract

**Files:**
- Create: `qt/src/nodepoller.h`
- Create: `qt/src/nodepoller.cpp`

- [ ] **Step 1: Investigate how `RpcClient` emits responses**

Read the relevant signals in `qt/src/rpcclient.h`. Look for emit signatures like `responseReceived`, `getInfoFinished`, etc.:

```bash
cd /private/tmp/dinero-v8-mnd-mvp
grep -nE "Q_SIGNALS|signals:|void.*Finished|emit " qt/src/rpcclient.h | head -30
```

Make note of the signal that maps to a generic `call(method, params)` response — likely a single signal carrying `(QString method, QJsonValue result, QString error)` or similar. The exact shape determines the connect-pattern in Step 3 below.

- [ ] **Step 2: Write the header**

Create `qt/src/nodepoller.h`. **Adjust the `connect()` calls in Step 3's `.cpp` based on what you found in Step 1** — the header doesn't need adjustment.

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"

#include <QObject>
#include <QTimer>

class RpcClient;

namespace dinero::qt::dashboard {

// Drives 5-second polling of getnetworkinfo / getpeerinfo /
// getblockchaininfo / getrelayinfo / getmempoolinfo and emits typed
// updates. Sections subscribe to the signals they need.
//
// Owns no UI. Safe to live anywhere; we instantiate one per
// MyNodeDashboard.
class NodePoller : public QObject {
    Q_OBJECT

public:
    explicit NodePoller(RpcClient* rpc, QObject* parent = nullptr);

    // Cadence is a constructor-time default; tests can override.
    void setIntervalMs(int ms);

    // Start/stop the timer. Start triggers an immediate first poll.
    void start();
    void stop();
    bool isRunning() const;

Q_SIGNALS:
    void identityUpdated(const NodeIdentity& identity);
    void chainInfoUpdated(const ChainInfo& info);
    void peersUpdated(const QVector<PeerRow>& peers);
    void daemonStateChanged(bool reachable);

private Q_SLOTS:
    void tick();
    void onRpcResponse(const QString& method,
                       const QJsonValue& result,
                       const QString& error);

private:
    RpcClient* rpc_{nullptr};
    QTimer     timer_;
    int        interval_ms_{5000};

    // Coalescing state — we expect responses from 5 distinct RPCs per
    // tick; we accumulate them and emit the relevant signal as each
    // arrives. No global "tick complete" event needed.
    NodeIdentity pending_identity_;
    ChainInfo    pending_chain_;
    QVector<PeerRow> pending_peers_;

    // Daemon-reachable state machine. degraded == 3 consecutive RPC
    // failures within a tick.
    int  consecutive_failures_{0};
    bool degraded_{false};

    void parseNetworkInfo(const QJsonValue& result);
    void parseChainInfo(const QJsonValue& result);
    void parsePeers(const QJsonValue& result);
    void parseMempool(const QJsonValue& result);
    void parseMining(const QJsonValue& result);
    void noteFailure();
    void noteSuccess();
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Write the implementation**

Create `qt/src/nodepoller.cpp`. Replace the `// HOOK: ...` placeholder comments with the actual signal name + connection idiom you found in Step 1.

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "rpcclient.h"

#include <QJsonArray>
#include <QJsonObject>

namespace dinero::qt::dashboard {

namespace {
QString fleetNameFor(const QString& addr) {
    if (addr.startsWith("172.93.160.131")) return "LA";
    if (addr.startsWith("173.249.195.59")) return "VA";
    if (addr.startsWith("72.18.214.120"))  return "MO";
    if (addr.startsWith("96.9.226.98"))    return "CN";
    return {};
}
}  // namespace

NodePoller::NodePoller(RpcClient* rpc, QObject* parent)
    : QObject(parent), rpc_(rpc) {
    timer_.setInterval(interval_ms_);
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &NodePoller::tick);

    // HOOK: replace `responseReceived` with the actual RpcClient
    // response signal name you found in Task 2 Step 1. Likely shape:
    //   void responseReceived(const QString& method,
    //                         const QJsonValue& result,
    //                         const QString& error);
    // If RpcClient uses per-method signals instead, connect each
    // (getNetworkInfoFinished, getPeerInfoFinished, etc.) and route
    // through onRpcResponse manually.
    connect(rpc_, &RpcClient::responseReceived,
            this, &NodePoller::onRpcResponse);
}

void NodePoller::setIntervalMs(int ms) {
    interval_ms_ = ms;
    timer_.setInterval(ms);
}

void NodePoller::start() {
    timer_.start();
    tick();  // immediate first poll
}

void NodePoller::stop() {
    timer_.stop();
}

bool NodePoller::isRunning() const {
    return timer_.isActive();
}

void NodePoller::tick() {
    if (!rpc_) return;
    pending_peers_.clear();
    rpc_->call("getnetworkinfo");
    rpc_->call("getblockchaininfo");
    rpc_->call("getpeerinfo");
    rpc_->call("getmempoolinfo");
    rpc_->call("getmininginfo");
}

void NodePoller::onRpcResponse(const QString& method,
                               const QJsonValue& result,
                               const QString& error) {
    if (!error.isEmpty()) {
        noteFailure();
        return;
    }
    noteSuccess();

    if      (method == "getnetworkinfo")    parseNetworkInfo(result);
    else if (method == "getblockchaininfo") parseChainInfo(result);
    else if (method == "getpeerinfo")       parsePeers(result);
    else if (method == "getmempoolinfo")    parseMempool(result);
    else if (method == "getmininginfo")     parseMining(result);
}

void NodePoller::parseNetworkInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_identity_.subversion = obj.value("subversion").toString();
    pending_identity_.version    = obj.value("version").toInt();
    pending_identity_.services   = static_cast<quint64>(
        obj.value("localservices").toString().toULongLong(nullptr, 16));
    pending_identity_.node_id_hex = obj.value("localnodeid").toString();

    const auto local_addrs = obj.value("localaddresses").toArray();
    if (!local_addrs.isEmpty()) {
        const auto a = local_addrs.first().toObject();
        pending_identity_.local_addr = a.value("address").toString();
        pending_identity_.local_port = static_cast<quint16>(
            a.value("port").toInt());
    }

    if (obj.contains("relay_active")) {
        pending_identity_.is_relay_active = obj.value("relay_active").toBool();
    }
    if (obj.contains("registrants")) {
        pending_identity_.registrants_count = obj.value("registrants").toInt();
    }
    if (obj.contains("grace_pending")) {
        pending_identity_.grace_count = obj.value("grace_pending").toInt();
    }

    // Reachability inference (DIRECT if listener and we have inbound peers;
    // BEHIND_RELAY if we have NODE_BEHIND_RELAY service bit; else UNKNOWN
    // until peers parse fills it in).
    const bool listening = obj.value("localrelay").toBool();
    pending_identity_.reachability =
        listening ? NodeIdentity::DIRECT : NodeIdentity::UNREACHABLE;

    Q_EMIT identityUpdated(pending_identity_);
}

void NodePoller::parseChainInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.our_height = static_cast<qint64>(
        obj.value("blocks").toDouble());

    const QString bitsHex = obj.value("bits").toString();
    pending_chain_.difficulty_compact = bitsHex.isEmpty()
        ? 0u
        : bitsHex.toUInt(nullptr, 16);

    Q_EMIT chainInfoUpdated(pending_chain_);
}

void NodePoller::parsePeers(const QJsonValue& result) {
    const auto arr = result.toArray();
    pending_peers_.clear();
    pending_peers_.reserve(arr.size());

    QVector<qint64> heights;
    heights.reserve(arr.size());

    for (const auto& v : arr) {
        const auto p = v.toObject();
        PeerRow r;
        r.addr             = p.value("addr").toString();
        r.via_relay        = r.addr.startsWith("relay:");
        r.is_inbound       = p.value("inbound").toBool();
        r.fleet_name       = fleetNameFor(r.addr);
        r.height           = static_cast<qint64>(
            p.value("synced_blocks").toDouble(-1));
        r.ping_ms          = static_cast<qint64>(
            p.value("pingtime").toDouble(-1) * 1000.0);
        r.quality_score    = p.value("quality_score").toInt(-1);
        r.handshake_complete = p.value("identity_proven").toBool(true);
        r.services         = static_cast<quint64>(
            p.value("services").toString().toULongLong(nullptr, 16));
        r.subversion       = p.value("subver").toString();
        r.bytes_sent       = static_cast<qint64>(
            p.value("bytessent").toDouble(0));
        r.bytes_recv       = static_cast<qint64>(
            p.value("bytesrecv").toDouble(0));

        if (r.height > 0) heights.push_back(r.height);
        pending_peers_.push_back(r);
    }

    if (!heights.isEmpty()) {
        pending_chain_.peer_heights = heights;
        std::sort(heights.begin(), heights.end());
        pending_chain_.max_peer_height = heights.back();
        // mode (consensus)
        qint64 best = heights.first();
        int best_count = 1, run = 1;
        for (int i = 1; i < heights.size(); ++i) {
            if (heights[i] == heights[i - 1]) { ++run; }
            else { run = 1; }
            if (run > best_count) { best_count = run; best = heights[i]; }
        }
        pending_chain_.net_consensus_height = best;
        Q_EMIT chainInfoUpdated(pending_chain_);
    }

    Q_EMIT peersUpdated(pending_peers_);
}

void NodePoller::parseMempool(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.mempool_tx_count = obj.value("size").toInt();
    pending_chain_.mempool_bytes    = static_cast<qint64>(
        obj.value("bytes").toDouble());
    Q_EMIT chainInfoUpdated(pending_chain_);
}

void NodePoller::parseMining(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_identity_.is_mining = obj.value("active").toBool();
    pending_identity_.shares_per_min =
        obj.value("shares_per_minute").toDouble(0.0);
    pending_identity_.mining_destination =
        obj.value("address").toString();
    Q_EMIT identityUpdated(pending_identity_);
}

void NodePoller::noteFailure() {
    ++consecutive_failures_;
    if (consecutive_failures_ >= 3 && !degraded_) {
        degraded_ = true;
        setIntervalMs(30000);  // back off to 30s while daemon unreachable
        Q_EMIT daemonStateChanged(false);
    }
}

void NodePoller::noteSuccess() {
    consecutive_failures_ = 0;
    if (degraded_) {
        degraded_ = false;
        setIntervalMs(5000);
        Q_EMIT daemonStateChanged(true);
    }
}

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 4: Verify the file compiles (no test yet)**

The cleanest way to verify compile-only without running the full Qt build is to add the source to the existing dinero-qt target and let CMake compile it. We'll do that in Task 7's CMake update. For now, do a manual sanity check that you can include `<QObject>` and `<QTimer>` in the file by reading the existing widgets that use them.

Skip the test-then-implement cycle for `NodePoller` itself — its tests live in Task 3 (after the value structs are ready).

- [ ] **Step 5: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/nodepoller.h qt/src/nodepoller.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): add NodePoller — 5s RPC polling driver

NodePoller drives polling of getnetworkinfo / getblockchaininfo /
getpeerinfo / getmempoolinfo / getmininginfo via the existing
RpcClient and emits typed signals (identityUpdated, chainInfoUpdated,
peersUpdated, daemonStateChanged).

Sections subscribe only to the signals they need — no monolithic
state, no per-section RPC fan-out.

Failure handling: 3 consecutive RPC errors → degraded mode (30s
backoff cadence + daemonStateChanged(false)). Recovery on first
success.

Tests land in the next task once test wiring is in place.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: NodePoller unit tests

**Files:**
- Create: `qt/tests/test_node_poller.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Write the test file**

Create `qt/tests/test_node_poller.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

using dinero::qt::dashboard::NodePoller;
using dinero::qt::dashboard::NodeIdentity;
using dinero::qt::dashboard::ChainInfo;
using dinero::qt::dashboard::PeerRow;

// Friend-shim: NodePoller's parse* methods are private. We expose them
// for testing by including the .cpp and using a derived helper. This is
// a Phase 1 simplification; a future refactor could move parsers into a
// free-function header for cleaner testing.
class TestablePoller : public NodePoller {
public:
    TestablePoller() : NodePoller(nullptr) {}

    // Trigger the public response slot directly with a canned payload.
    void feed(const QString& method, const QJsonValue& result) {
        // Public slot — re-emit via QMetaObject so the connection wiring
        // does not need a real RpcClient.
        QMetaObject::invokeMethod(this, "onRpcResponse",
            Q_ARG(QString, method),
            Q_ARG(QJsonValue, result),
            Q_ARG(QString, QString()));
    }
};

class TestNodePoller : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parses_network_info_into_identity() {
        TestablePoller p;
        QSignalSpy spy(&p, &NodePoller::identityUpdated);

        QJsonObject ni;
        ni["subversion"]    = "/dinerod:b06ec828/";
        ni["version"]       = 80000;
        ni["localservices"] = "20000000";  // hex; bit 29 set is fine
        ni["localnodeid"]   = "fd4fc04df38bacbf72d4ecae451d1589570bcaba";
        ni["localrelay"]    = true;
        QJsonArray la;
        QJsonObject one;
        one["address"] = "162.200.227.214";
        one["port"]    = 20999;
        la.append(one);
        ni["localaddresses"] = la;

        p.feed("getnetworkinfo", ni);
        QCOMPARE(spy.count(), 1);
        const auto id = spy.at(0).at(0).value<NodeIdentity>();
        QCOMPARE(id.subversion, QString("/dinerod:b06ec828/"));
        QCOMPARE(id.version, 80000);
        QCOMPARE(id.local_addr, QString("162.200.227.214"));
        QCOMPARE(id.local_port, quint16(20999));
        QCOMPARE(int(id.reachability), int(NodeIdentity::DIRECT));
    }

    void parses_peers_height_consensus_via_mode() {
        TestablePoller p;
        QSignalSpy spy_chain(&p, &NodePoller::chainInfoUpdated);
        QSignalSpy spy_peers(&p, &NodePoller::peersUpdated);

        QJsonArray peers;
        auto mk = [](const QString& a, qint64 h, int q) {
            QJsonObject o;
            o["addr"]          = a;
            o["synced_blocks"] = double(h);
            o["quality_score"] = q;
            o["inbound"]       = false;
            return o;
        };
        peers.append(mk("172.93.160.131:20999", 27402, 92));
        peers.append(mk("173.249.195.59:20999", 27402, 88));
        peers.append(mk("72.18.214.120:20999",  27402, 75));
        peers.append(mk("96.9.226.98:20999",    27401, 44));

        p.feed("getpeerinfo", peers);
        QVERIFY(spy_peers.count() >= 1);
        QVERIFY(spy_chain.count() >= 1);

        const auto rows = spy_peers.last().at(0).value<QVector<PeerRow>>();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].fleet_name, QString("LA"));
        QCOMPARE(rows[1].fleet_name, QString("VA"));
        QCOMPARE(rows[2].fleet_name, QString("MO"));
        QCOMPARE(rows[3].fleet_name, QString("CN"));
        QCOMPARE(rows[0].quality_score, 92);

        const auto ci = spy_chain.last().at(0).value<ChainInfo>();
        QCOMPARE(ci.net_consensus_height, qint64(27402));
        QCOMPARE(ci.max_peer_height,      qint64(27402));
    }

    void degraded_after_three_consecutive_failures() {
        TestablePoller p;
        QSignalSpy spy(&p, &NodePoller::daemonStateChanged);

        // Helper to trigger a failure
        auto fail = [&] {
            QMetaObject::invokeMethod(&p, "onRpcResponse",
                Q_ARG(QString, "getnetworkinfo"),
                Q_ARG(QJsonValue, QJsonValue()),
                Q_ARG(QString, QString("connection refused")));
        };

        fail();
        QCOMPARE(spy.count(), 0);
        fail();
        QCOMPARE(spy.count(), 0);
        fail();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toBool(), false);
    }
};

QTEST_MAIN(TestNodePoller)
#include "test_node_poller.moc"
```

Q_DECLARE_METATYPE: Qt needs `NodeIdentity`, `ChainInfo`, `PeerRow`, and `QVector<PeerRow>` registered for `QSignalSpy::value<>` to work. Add this near the top of `qt/src/dashboardtypes.h` (re-edit the file from Task 1):

```cpp
#include <QMetaType>
// ... at the very bottom of the file, after the namespace close:
Q_DECLARE_METATYPE(dinero::qt::dashboard::NodeIdentity)
Q_DECLARE_METATYPE(dinero::qt::dashboard::ChainInfo)
Q_DECLARE_METATYPE(dinero::qt::dashboard::PeerRow)
Q_DECLARE_METATYPE(QVector<dinero::qt::dashboard::PeerRow>)
```

- [ ] **Step 2: Register the test in qt/CMakeLists.txt**

Find the existing test block in `qt/CMakeLists.txt` (around line 1250 — search `test_wallet_name_utils`). After the existing test definitions, append:

```cmake
  add_executable(test_node_poller
    tests/test_node_poller.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_node_poller PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
  )

  target_compile_definitions(test_node_poller PRIVATE QT_NO_KEYWORDS)
  add_test(NAME NodePoller COMMAND test_node_poller)
```

Note: `NodePoller`'s constructor takes `RpcClient*` but the test passes `nullptr`. The test only feeds responses directly via `feed()` (which calls `onRpcResponse` slot) — it never triggers `tick()` which would dereference `rpc_`. The `nullptr` check at the top of `tick()` is what makes that safe.

- [ ] **Step 3: Build + run the test**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
cmake -S . -B build-mnd 2>&1 | tail -3
cmake --build build-mnd --target test_node_poller -j8 2>&1 | tail -5
cd build-mnd && ctest -R NodePoller --output-on-failure && cd ..
```

Expected: 3 tests pass.

- [ ] **Step 4: Update Q_DECLARE_METATYPE + commit**

If you added the `Q_DECLARE_METATYPE` block to `dashboardtypes.h` in Step 1, that's a modification of a file already committed in Task 1. Include it in this commit:

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/dashboardtypes.h qt/tests/test_node_poller.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(qt-dashboard): unit tests for NodePoller parsers + degraded mode

Covers:
- getnetworkinfo → identity (subversion, services, localaddr, reachability)
- getpeerinfo → height consensus via mode + per-row fleet name + Q score
- 3 consecutive RPC failures → daemonStateChanged(false)

Adds Q_DECLARE_METATYPE for NodeIdentity / ChainInfo / PeerRow /
QVector<PeerRow> so QSignalSpy::value<T>() can deserialize.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: IdentitySection widget

**Files:**
- Create: `qt/src/identitysection.h`
- Create: `qt/src/identitysection.cpp`

- [ ] **Step 1: Write the header**

Create `qt/src/identitysection.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QWidget>

class QLabel;
class QPushButton;

namespace dinero::qt::dashboard {

// ⚡ YOU section — renders the current node's identity, posture,
// relay role, mining status, uptime, and daemon version.
class IdentitySection : public QWidget {
    Q_OBJECT
public:
    explicit IdentitySection(QWidget* parent = nullptr);

public Q_SLOTS:
    void onIdentityUpdated(const NodeIdentity& id);
    void onDaemonStateChanged(bool reachable);

private:
    QLabel*      nodeIdLabel_{nullptr};      // formatted hex
    QPushButton* nodeIdCopyBtn_{nullptr};
    QLabel*      reachabilityLabel_{nullptr};
    QLabel*      relayingLabel_{nullptr};
    QLabel*      miningLabel_{nullptr};
    QLabel*      footerLabel_{nullptr};

    static QString formatNodeIdHex(const QString& raw_hex);
    static QString reachabilityLine(const NodeIdentity& id);
    static QString relayingLine(const NodeIdentity& id);
    static QString miningLine(const NodeIdentity& id);
    static QString footerLine(const NodeIdentity& id);
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Write the implementation**

Create `qt/src/identitysection.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "identitysection.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

IdentitySection::IdentitySection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    auto* header = new QLabel("⚡ YOU", this);
    header->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(header);

    auto* idRow = new QHBoxLayout();
    nodeIdLabel_ = new QLabel(this);
    nodeIdLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nodeIdLabel_->setStyleSheet("font-family: monospace;");
    nodeIdCopyBtn_ = new QPushButton("📋", this);
    nodeIdCopyBtn_->setFixedWidth(28);
    nodeIdCopyBtn_->setToolTip("Copy node_id");
    connect(nodeIdCopyBtn_, &QPushButton::clicked, this, [this]() {
        const QString raw = nodeIdLabel_->text().remove(' ');
        QApplication::clipboard()->setText(raw);
    });
    idRow->addWidget(nodeIdLabel_, 1);
    idRow->addWidget(nodeIdCopyBtn_, 0);
    root->addLayout(idRow);

    reachabilityLabel_ = new QLabel(this);
    relayingLabel_     = new QLabel(this);
    miningLabel_       = new QLabel(this);
    footerLabel_       = new QLabel(this);
    footerLabel_->setStyleSheet("color: #888; font-size: 11px;");

    root->addWidget(reachabilityLabel_);
    root->addWidget(relayingLabel_);
    root->addWidget(miningLabel_);
    root->addWidget(footerLabel_);
    root->addStretch(1);

    onIdentityUpdated({});  // initial placeholder state
}

void IdentitySection::onIdentityUpdated(const NodeIdentity& id) {
    nodeIdLabel_->setText(formatNodeIdHex(id.node_id_hex));
    reachabilityLabel_->setText(reachabilityLine(id));
    relayingLabel_->setText(relayingLine(id));
    miningLabel_->setText(miningLine(id));
    footerLabel_->setText(footerLine(id));
}

void IdentitySection::onDaemonStateChanged(bool reachable) {
    if (!reachable) {
        reachabilityLabel_->setText("● UNREACHABLE · daemon not responding");
        reachabilityLabel_->setStyleSheet("color: #c33;");
    } else {
        reachabilityLabel_->setStyleSheet("");
    }
}

QString IdentitySection::formatNodeIdHex(const QString& raw_hex) {
    if (raw_hex.isEmpty()) return "—";
    QString out;
    for (int i = 0; i < raw_hex.size(); i += 4) {
        if (!out.isEmpty()) out += ' ';
        out += raw_hex.mid(i, 4);
    }
    return out;
}

QString IdentitySection::reachabilityLine(const NodeIdentity& id) {
    switch (id.reachability) {
    case NodeIdentity::DIRECT:
        return QString("●  DIRECT · reachable on %1:%2")
            .arg(id.local_addr).arg(id.local_port);
    case NodeIdentity::BEHIND_RELAY:
        return "●  BEHIND-RELAY · reachable via relay-virtual peers";
    case NodeIdentity::UNREACHABLE:
        return "○  UNREACHABLE · not listening";
    case NodeIdentity::UNKNOWN:
    default:
        return "○  …";
    }
}

QString IdentitySection::relayingLine(const NodeIdentity& id) {
    if (!id.is_relay_active) {
        return "⤴  RELAYING · OFF";
    }
    return QString("⤴  RELAYING for %1 peer%2 · %3 in grace")
        .arg(id.registrants_count)
        .arg(id.registrants_count == 1 ? "" : "s")
        .arg(id.grace_count);
}

QString IdentitySection::miningLine(const NodeIdentity& id) {
    if (!id.is_mining) {
        return "⛏  MINING · OFF";
    }
    return QString("⛏  MINING to %1 · %2 shares/min")
        .arg(id.mining_destination.isEmpty() ? "—" : id.mining_destination)
        .arg(id.shares_per_min, 0, 'f', 1);
}

QString IdentitySection::footerLine(const NodeIdentity& id) {
    const auto secs = id.uptime.count();
    const int h = static_cast<int>(secs / 3600);
    const int m = static_cast<int>((secs % 3600) / 60);
    return QString("uptime  %1h %2m       %3")
        .arg(h).arg(m, 2, 10, QChar('0'))
        .arg(id.subversion.isEmpty() ? "—" : id.subversion);
}

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Commit (no test yet — test lands in Task 5)**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/identitysection.h qt/src/identitysection.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): IdentitySection — ⚡ YOU widget

Renders node_id (formatted hex + copy button), reachability glyph,
relay status, mining status, and footer (uptime + subversion).

Reachability inferred from NodeIdentity::Reachability enum populated
by NodePoller's network-info parser. Color-codes UNREACHABLE in red
when daemon goes degraded.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: IdentitySection unit tests

**Files:**
- Create: `qt/tests/test_identity_section.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Write the test**

Create `qt/tests/test_identity_section.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "identitysection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QtTest/QtTest>

using dinero::qt::dashboard::IdentitySection;
using dinero::qt::dashboard::NodeIdentity;

class TestIdentitySection : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void formats_node_id_in_4char_groups() {
        IdentitySection s;
        NodeIdentity id;
        id.node_id_hex = "fd4fc04df38bacbf72d4ecae451d1589570bcaba";
        s.onIdentityUpdated(id);

        // node_id label is the first QLabel containing the formatted hex
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().startsWith("fd4f")) {
                // Expect 10 4-char groups separated by single spaces
                QCOMPARE(l->text(),
                    QString("fd4f c04d f38b acbf 72d4 ecae 451d 1589 570b caba"));
                found = true;
            }
        }
        QVERIFY(found);
    }

    void reachability_direct_includes_addr_and_port() {
        IdentitySection s;
        NodeIdentity id;
        id.reachability = NodeIdentity::DIRECT;
        id.local_addr   = "162.200.227.214";
        id.local_port   = 20999;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("DIRECT")) {
                QVERIFY(l->text().contains("162.200.227.214:20999"));
                found = true;
            }
        }
        QVERIFY(found);
    }

    void relaying_off_when_not_active() {
        IdentitySection s;
        NodeIdentity id;
        id.is_relay_active = false;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("RELAYING · OFF")) found = true;
        }
        QVERIFY(found);
    }

    void relaying_on_includes_count_and_grace() {
        IdentitySection s;
        NodeIdentity id;
        id.is_relay_active   = true;
        id.registrants_count = 2;
        id.grace_count       = 1;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("RELAYING for 2 peers · 1 in grace"))
                found = true;
        }
        QVERIFY(found);
    }

    void mining_line_off_when_inactive() {
        IdentitySection s;
        NodeIdentity id;
        id.is_mining = false;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("MINING · OFF")) found = true;
        }
        QVERIFY(found);
    }

    void daemon_degraded_marks_unreachable_red() {
        IdentitySection s;
        s.onDaemonStateChanged(false);
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("UNREACHABLE") &&
                l->styleSheet().contains("color: #c33")) {
                found = true;
            }
        }
        QVERIFY(found);
    }
};

QTEST_MAIN(TestIdentitySection)
#include "test_identity_section.moc"
```

- [ ] **Step 2: Register in CMake**

Append to `qt/CMakeLists.txt` after the `NodePoller` test block:

```cmake
  add_executable(test_identity_section
    tests/test_identity_section.cpp
    src/identitysection.cpp
    src/identitysection.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_identity_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_compile_definitions(test_identity_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME IdentitySection COMMAND test_identity_section)
```

- [ ] **Step 3: Build + run**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
cmake --build build-mnd --target test_identity_section -j8 2>&1 | tail -5
cd build-mnd && ctest -R IdentitySection --output-on-failure && cd ..
```

Expected: 6 tests pass.

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/tests/test_identity_section.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(qt-dashboard): IdentitySection rendering tests

Six cases: 4-char hex grouping, DIRECT reachability includes
addr:port, relaying OFF/ON variants, mining OFF, degraded daemon
color-codes UNREACHABLE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: NetworkSection widget + tests

**Files:**
- Create: `qt/src/networksection.h`
- Create: `qt/src/networksection.cpp`
- Create: `qt/tests/test_network_section.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `qt/src/networksection.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QWidget>

class QLabel;
class QProgressBar;

namespace dinero::qt::dashboard {

// 📡 NETWORK · as you see it — chain tip race + secondary metrics.
class NetworkSection : public QWidget {
    Q_OBJECT
public:
    explicit NetworkSection(QWidget* parent = nullptr);

public Q_SLOTS:
    void onChainInfoUpdated(const ChainInfo& info);

    // Tip-race annotation helper, public for unit testing.
    static QString tipDeltaAnnotation(qint64 our, qint64 net);

private:
    QProgressBar* youBar_{nullptr};
    QProgressBar* netBar_{nullptr};
    QLabel*       deltaLabel_{nullptr};
    QLabel*       difficultyLabel_{nullptr};
    QLabel*       mempoolLabel_{nullptr};
    QLabel*       medianFeeLabel_{nullptr};
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Write the implementation**

Create `qt/src/networksection.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"

#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

NetworkSection::NetworkSection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* header = new QLabel("📡 NETWORK · as you see it", this);
    header->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(header);

    youBar_ = new QProgressBar(this);
    youBar_->setFormat("you  %v");
    youBar_->setTextVisible(true);
    root->addWidget(youBar_);

    netBar_ = new QProgressBar(this);
    netBar_->setFormat("net  %v");
    netBar_->setTextVisible(true);
    root->addWidget(netBar_);

    deltaLabel_ = new QLabel(this);
    deltaLabel_->setStyleSheet("font-weight: bold;");
    root->addWidget(deltaLabel_);

    auto* grid = new QGridLayout();
    difficultyLabel_ = new QLabel(this);
    mempoolLabel_    = new QLabel(this);
    medianFeeLabel_  = new QLabel(this);
    grid->addWidget(new QLabel("difficulty", this), 0, 0);
    grid->addWidget(difficultyLabel_,               0, 1);
    grid->addWidget(new QLabel("mempool",    this), 1, 0);
    grid->addWidget(mempoolLabel_,                  1, 1);
    grid->addWidget(new QLabel("median fee", this), 2, 0);
    grid->addWidget(medianFeeLabel_,                2, 1);
    root->addLayout(grid);
    root->addStretch(1);

    onChainInfoUpdated({});
}

void NetworkSection::onChainInfoUpdated(const ChainInfo& info) {
    const qint64 maxH = std::max(info.our_height, info.max_peer_height);
    if (maxH > 0) {
        youBar_->setRange(0, static_cast<int>(maxH));
        youBar_->setValue(static_cast<int>(info.our_height));
        netBar_->setRange(0, static_cast<int>(maxH));
        netBar_->setValue(static_cast<int>(info.net_consensus_height));
    }

    deltaLabel_->setText(
        tipDeltaAnnotation(info.our_height, info.net_consensus_height));

    difficultyLabel_->setText(QString("0x%1")
        .arg(info.difficulty_compact, 8, 16, QChar('0')));
    mempoolLabel_->setText(QString("%1 tx / %2 KB")
        .arg(info.mempool_tx_count)
        .arg(info.mempool_bytes / 1024));
    medianFeeLabel_->setText(QString("%1 una/vB")
        .arg(info.median_fee_una_per_vbyte));
}

QString NetworkSection::tipDeltaAnnotation(qint64 our, qint64 net) {
    if (net <= 0 && our <= 0) return "—";
    const qint64 delta = net - our;
    if (delta == 0) return "● in sync";
    if (delta > 0)  return QString("● +%1 behind net").arg(delta);
    return QString("● %1 ahead of net").arg(-delta);
}

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Write the test**

Create `qt/tests/test_network_section.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"
#include <QtTest/QtTest>

using dinero::qt::dashboard::NetworkSection;

class TestNetworkSection : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void tip_delta_in_sync() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27402, 27402),
                 QString("● in sync"));
    }
    void tip_delta_behind() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27400, 27402),
                 QString("● +2 behind net"));
    }
    void tip_delta_ahead() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27403, 27402),
                 QString("● 1 ahead of net"));
    }
    void tip_delta_zero_when_both_zero() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(0, 0), QString("—"));
    }
};

QTEST_MAIN(TestNetworkSection)
#include "test_network_section.moc"
```

- [ ] **Step 4: Register in CMake**

Append to `qt/CMakeLists.txt`:

```cmake
  add_executable(test_network_section
    tests/test_network_section.cpp
    src/networksection.cpp
    src/networksection.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_network_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_compile_definitions(test_network_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME NetworkSection COMMAND test_network_section)
```

- [ ] **Step 5: Build + run**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
cmake --build build-mnd --target test_network_section -j8 2>&1 | tail -5
cd build-mnd && ctest -R NetworkSection --output-on-failure && cd ..
```

Expected: 4 tests pass.

- [ ] **Step 6: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/networksection.h qt/src/networksection.cpp \
        qt/tests/test_network_section.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): NetworkSection — 📡 chain tip race + metrics

Two QProgressBars (you/net) scaled to max(our_height, max_peer_height)
make the gap visceral. Delta annotation: "in sync" / "+N behind" /
"N ahead". Secondary grid: difficulty (compact hex), mempool (tx + KB),
median fee (una/vB).

Static tipDeltaAnnotation() exposed for testing — 4 cases pass.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: PeersSection widget

**Files:**
- Create: `qt/src/peerssection.h`
- Create: `qt/src/peerssection.cpp`

- [ ] **Step 1: Write the header**

Create `qt/src/peerssection.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QSet>
#include <QVector>
#include <QWidget>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace dinero::qt::dashboard {

// 🛰 PEERS — sortable table, click row to expand details.
// Phase 1 omits the topology view toggle (Phase 3).
class PeersSection : public QWidget {
    Q_OBJECT
public:
    explicit PeersSection(QWidget* parent = nullptr);

    // Quality stoplight glyph helper, public for unit testing.
    static QString stoplightGlyph(int quality_score, bool handshake_complete);

public Q_SLOTS:
    void onPeersUpdated(const QVector<PeerRow>& peers);

private Q_SLOTS:
    void onRowClicked(QTreeWidgetItem* item, int column);

private:
    QLabel*      headerLabel_{nullptr};
    QTreeWidget* tree_{nullptr};
    QSet<QString> expandedAddrs_;  // persisted across polls

    void populateRow(QTreeWidgetItem* item, const PeerRow& r);
    void populateDetailChild(QTreeWidgetItem* parent, const PeerRow& r);
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Write the implementation**

Create `qt/src/peerssection.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "peerssection.h"

#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

PeersSection::PeersSection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    headerLabel_ = new QLabel("🛰 PEERS (0 connected)", this);
    headerLabel_->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(headerLabel_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({"Q", "dir", "who", "height", "ping"});
    tree_->setRootIsDecorated(false);  // top-level rows flush; child rows indented for expand
    tree_->setUniformRowHeights(false);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::DescendingOrder);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(tree_, &QTreeWidget::itemClicked,
            this, &PeersSection::onRowClicked);
    root->addWidget(tree_, 1);
}

QString PeersSection::stoplightGlyph(int q, bool handshake_complete) {
    if (!handshake_complete) return "○";
    if (q < 0)               return "○";
    if (q >= 70)             return "●";
    if (q >= 40)             return "◐";
    return "⚠";
}

void PeersSection::onPeersUpdated(const QVector<PeerRow>& peers) {
    headerLabel_->setText(QString("🛰 PEERS (%1 connected)")
        .arg(peers.size()));

    tree_->setSortingEnabled(false);
    tree_->clear();

    for (const auto& r : peers) {
        auto* item = new QTreeWidgetItem(tree_);
        populateRow(item, r);
        if (expandedAddrs_.contains(r.addr)) {
            populateDetailChild(item, r);
            item->setExpanded(true);
        }
    }

    tree_->setSortingEnabled(true);
}

void PeersSection::populateRow(QTreeWidgetItem* item, const PeerRow& r) {
    const QString glyph = stoplightGlyph(r.quality_score, r.handshake_complete);
    item->setText(0, QString("%1 %2").arg(glyph)
        .arg(r.quality_score < 0 ? QString("—") : QString::number(r.quality_score)));
    // Sort numerically by quality_score; use Qt::UserRole on column 0
    item->setData(0, Qt::UserRole, r.quality_score);

    item->setText(1, r.via_relay ? "⇄" : (r.is_inbound ? "↓" : "↑"));

    QString who = r.addr;
    if (!r.fleet_name.isEmpty()) who += " · " + r.fleet_name;
    if (r.via_relay && !r.relay_via_addr.isEmpty()) {
        who += QString(" (via %1)").arg(r.relay_via_addr);
    }
    item->setText(2, who);

    item->setText(3, r.height < 0 ? QString("—")
                                  : QString::number(r.height));
    item->setText(4, r.ping_ms < 0 ? QString("—")
                                   : QString("%1 ms").arg(r.ping_ms));

    item->setData(0, Qt::UserRole + 1, r.addr);  // stash for click handler
}

void PeersSection::populateDetailChild(QTreeWidgetItem* parent,
                                       const PeerRow& r) {
    auto* child = new QTreeWidgetItem(parent);
    const QString detail = QString(
        "services 0x%1 · subver %2 · ↑%3 KB ↓%4 KB · age %5s · last msg %6s ago")
        .arg(r.services, 0, 16)
        .arg(r.subversion.isEmpty() ? "—" : r.subversion)
        .arg(r.bytes_sent / 1024)
        .arg(r.bytes_recv / 1024)
        .arg(r.connected_for.count())
        .arg(r.last_message_ago.count());
    child->setFirstColumnSpanned(true);
    child->setText(0, detail);
    child->setForeground(0, Qt::gray);
}

void PeersSection::onRowClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item || item->parent() != nullptr) return;  // child row click → ignore
    const QString addr = item->data(0, Qt::UserRole + 1).toString();
    if (addr.isEmpty()) return;

    if (expandedAddrs_.contains(addr)) {
        expandedAddrs_.remove(addr);
        // Remove any existing detail child
        while (item->childCount() > 0) {
            delete item->takeChild(0);
        }
        item->setExpanded(false);
    } else {
        expandedAddrs_.insert(addr);
        // Rebuild detail from currently-displayed row data; full data
        // is in the next poll. For now, show a placeholder until the
        // next onPeersUpdated re-populates with full data.
        auto* placeholder = new QTreeWidgetItem(item);
        placeholder->setFirstColumnSpanned(true);
        placeholder->setText(0, "(loading details — refresh in <5s)");
        placeholder->setForeground(0, Qt::gray);
        item->setExpanded(true);
    }
}

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Commit (test in next task)**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/peerssection.h qt/src/peerssection.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): PeersSection — 🛰 sortable peer table

QTreeWidget-based with 5 columns (Q, dir, who, height, ping).
Default sort: Q descending. Stoplight glyph per quality range:
● ≥70, ◐ 40-69, ⚠ <40, ○ handshaking/no-score.

Click row → expand inline detail child (services, subver, bytes,
connection age, last message age). Expansion is persistent across
re-polls via expandedAddrs_ set.

Phase 1 omits topology view + right-click actions (Phase 3).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: PeersSection unit tests

**Files:**
- Create: `qt/tests/test_peers_section.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Write the test**

Create `qt/tests/test_peers_section.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "peerssection.h"
#include "dashboardtypes.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtTest/QtTest>

using dinero::qt::dashboard::PeersSection;
using dinero::qt::dashboard::PeerRow;

class TestPeersSection : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void stoplight_glyph_per_range() {
        QCOMPARE(PeersSection::stoplightGlyph(92, true), QString("●"));
        QCOMPARE(PeersSection::stoplightGlyph(70, true), QString("●"));
        QCOMPARE(PeersSection::stoplightGlyph(69, true), QString("◐"));
        QCOMPARE(PeersSection::stoplightGlyph(40, true), QString("◐"));
        QCOMPARE(PeersSection::stoplightGlyph(39, true), QString("⚠"));
        QCOMPARE(PeersSection::stoplightGlyph(0,  true), QString("⚠"));
        QCOMPARE(PeersSection::stoplightGlyph(-1, true), QString("○"));
        QCOMPARE(PeersSection::stoplightGlyph(80, false), QString("○"));
    }

    void header_count_reflects_peer_list_size() {
        PeersSection s;
        QVector<PeerRow> peers;
        peers.append(PeerRow{});
        peers.append(PeerRow{});
        peers.append(PeerRow{});
        s.onPeersUpdated(peers);
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == "🛰 PEERS (3 connected)") found = true;
        }
        QVERIFY(found);
    }

    void rows_populate_with_addr_height_ping() {
        PeersSection s;
        PeerRow r;
        r.addr = "172.93.160.131:20999";
        r.fleet_name = "LA";
        r.height = 27402;
        r.ping_ms = 12;
        r.quality_score = 92;
        s.onPeersUpdated({r});

        auto* tree = s.findChild<QTreeWidget*>();
        QVERIFY(tree != nullptr);
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto* item = tree->topLevelItem(0);
        QVERIFY(item->text(0).contains("92"));
        QVERIFY(item->text(0).contains("●"));
        QVERIFY(item->text(2).contains("172.93.160.131:20999"));
        QVERIFY(item->text(2).contains("LA"));
        QCOMPARE(item->text(3), QString("27402"));
        QCOMPARE(item->text(4), QString("12 ms"));
    }

    void relay_virtual_peer_shows_via_annotation() {
        PeersSection s;
        PeerRow r;
        r.addr = "relay:abc:def";
        r.via_relay = true;
        r.relay_via_addr = "172.93.160.131:20999";
        r.is_inbound = true;
        s.onPeersUpdated({r});
        auto* tree = s.findChild<QTreeWidget*>();
        auto* item = tree->topLevelItem(0);
        QCOMPARE(item->text(1), QString("⇄"));
        QVERIFY(item->text(2).contains("(via 172.93.160.131:20999)"));
    }
};

QTEST_MAIN(TestPeersSection)
#include "test_peers_section.moc"
```

- [ ] **Step 2: Register in CMake**

Append to `qt/CMakeLists.txt`:

```cmake
  add_executable(test_peers_section
    tests/test_peers_section.cpp
    src/peerssection.cpp
    src/peerssection.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_peers_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_compile_definitions(test_peers_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME PeersSection COMMAND test_peers_section)
```

- [ ] **Step 3: Build + run**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
cmake --build build-mnd --target test_peers_section -j8 2>&1 | tail -5
cd build-mnd && ctest -R PeersSection --output-on-failure && cd ..
```

Expected: 4 tests pass.

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/tests/test_peers_section.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
test(qt-dashboard): PeersSection rendering tests

Stoplight glyph per Q range, header count matches peer list size, row
populates address/height/ping with fleet annotation, relay-virtual
peer shows '⇄' direction + (via <relay>) annotation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: MyNodeDashboard composes everything

**Files:**
- Create: `qt/src/mynodedashboard.h`
- Create: `qt/src/mynodedashboard.cpp`
- Modify: `qt/CMakeLists.txt` (add new sources to main executable)

- [ ] **Step 1: Write the header**

Create `qt/src/mynodedashboard.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QWidget>

class RpcClient;

namespace dinero::qt::dashboard {

class NodePoller;
class IdentitySection;
class NetworkSection;
class PeersSection;

class MyNodeDashboard : public QWidget {
    Q_OBJECT
public:
    explicit MyNodeDashboard(RpcClient* rpc, QWidget* parent = nullptr);

    void start();   // begin polling
    void stop();    // stop polling

private:
    NodePoller*      poller_{nullptr};
    IdentitySection* identitySection_{nullptr};
    NetworkSection*  networkSection_{nullptr};
    PeersSection*    peersSection_{nullptr};
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Write the implementation**

Create `qt/src/mynodedashboard.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mynodedashboard.h"

#include "identitysection.h"
#include "networksection.h"
#include "nodepoller.h"
#include "peerssection.h"

#include <QScrollArea>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

MyNodeDashboard::MyNodeDashboard(RpcClient* rpc, QWidget* parent)
    : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* layout  = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    identitySection_ = new IdentitySection(content);
    networkSection_  = new NetworkSection(content);
    peersSection_    = new PeersSection(content);

    layout->addWidget(identitySection_);
    layout->addWidget(networkSection_);
    layout->addWidget(peersSection_, 1);

    scroll->setWidget(content);
    outer->addWidget(scroll);

    poller_ = new NodePoller(rpc, this);
    connect(poller_, &NodePoller::identityUpdated,
            identitySection_, &IdentitySection::onIdentityUpdated);
    connect(poller_, &NodePoller::chainInfoUpdated,
            networkSection_, &NetworkSection::onChainInfoUpdated);
    connect(poller_, &NodePoller::peersUpdated,
            peersSection_, &PeersSection::onPeersUpdated);
    connect(poller_, &NodePoller::daemonStateChanged,
            identitySection_, &IdentitySection::onDaemonStateChanged);
}

void MyNodeDashboard::start() { poller_->start(); }
void MyNodeDashboard::stop()  { poller_->stop();  }

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Add all new sources to dinero-qt's main executable in CMake**

Search for where the dinero-qt executable target is defined and where existing source files like `aipanel.cpp` are listed:

```bash
cd /private/tmp/dinero-v8-mnd-mvp
grep -n "src/aipanel.cpp\|set(QT_SOURCES\|add_executable(dinero-qt\|target_sources" qt/CMakeLists.txt | head -10
```

Add these source files alongside the existing list (match the exact pattern):
- `src/dashboardtypes.h`
- `src/nodepoller.cpp` + `src/nodepoller.h`
- `src/identitysection.cpp` + `src/identitysection.h`
- `src/networksection.cpp` + `src/networksection.h`
- `src/peerssection.cpp` + `src/peerssection.h`
- `src/mynodedashboard.cpp` + `src/mynodedashboard.h`

(The `CmdKPanel` files don't exist yet — they land in Task 10.)

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/mynodedashboard.h qt/src/mynodedashboard.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): MyNodeDashboard — composes 3 sections + NodePoller

Wires NodePoller signals to Identity/Network/Peers sections inside a
scrollable QVBoxLayout. start()/stop() control polling lifecycle so
the panel doesn't poll when hidden.

Also registers all new dashboard sources in the dinero-qt executable
target so they compile into the main app.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: CmdKPanel — tabbed slide-in container

**Files:**
- Create: `qt/src/cmdkpanel.h`
- Create: `qt/src/cmdkpanel.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `qt/src/cmdkpanel.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QPropertyAnimation>
#include <QWidget>

class AiPanel;
class QStackedWidget;
class QTabBar;
class RpcClient;

namespace dinero::qt::dashboard {

class MyNodeDashboard;

// Top-level slide-in container for the Cmd+K experience.
// Owns the slide animation. Hosts a QTabBar + QStackedWidget with two
// tabs in Phase 1: Dashboard (default) and AI (existing AiPanel,
// passed in by the parent so its lifecycle is unchanged).
class CmdKPanel : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int panelWidth READ panelWidth WRITE setPanelWidth)
public:
    explicit CmdKPanel(RpcClient* rpc, AiPanel* aiPanel,
                       QWidget* parent = nullptr);

    int  panelWidth() const;
    void setPanelWidth(int w);

    void togglePanel();
    bool isPanelOpen() const { return panelOpen_; }

Q_SIGNALS:
    void panelToggled(bool open);

private:
    bool             panelOpen_{false};
    int              targetWidth_{520};
    QPropertyAnimation* slideAnim_{nullptr};
    QTabBar*         tabBar_{nullptr};
    QStackedWidget*  stack_{nullptr};
    MyNodeDashboard* dashboard_{nullptr};
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Write the implementation**

Create `qt/src/cmdkpanel.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "cmdkpanel.h"

#include "aipanel.h"
#include "mynodedashboard.h"

#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

CmdKPanel::CmdKPanel(RpcClient* rpc, AiPanel* aiPanel, QWidget* parent)
    : QWidget(parent) {
    setFixedWidth(0);  // start collapsed; slide animation grows panelWidth

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabBar_ = new QTabBar(this);
    tabBar_->addTab("Dashboard");
    tabBar_->addTab("AI");
    root->addWidget(tabBar_);

    stack_ = new QStackedWidget(this);
    dashboard_ = new MyNodeDashboard(rpc, this);
    stack_->addWidget(dashboard_);   // index 0
    stack_->addWidget(aiPanel);      // index 1 — re-parented to us
    root->addWidget(stack_, 1);

    connect(tabBar_, &QTabBar::currentChanged,
            stack_, &QStackedWidget::setCurrentIndex);
    tabBar_->setCurrentIndex(0);

    slideAnim_ = new QPropertyAnimation(this, "panelWidth", this);
    slideAnim_->setDuration(200);
}

int CmdKPanel::panelWidth() const { return width(); }

void CmdKPanel::setPanelWidth(int w) {
    setFixedWidth(w);
}

void CmdKPanel::togglePanel() {
    slideAnim_->stop();
    if (panelOpen_) {
        slideAnim_->setStartValue(targetWidth_);
        slideAnim_->setEndValue(0);
        dashboard_->stop();
        panelOpen_ = false;
    } else {
        slideAnim_->setStartValue(0);
        slideAnim_->setEndValue(targetWidth_);
        dashboard_->start();
        panelOpen_ = true;
    }
    slideAnim_->start();
    Q_EMIT panelToggled(panelOpen_);
}

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Add to CMake (executable target)**

Add `src/cmdkpanel.cpp` and `src/cmdkpanel.h` to the dinero-qt executable source list, alongside the dashboard sources added in Task 9.

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/cmdkpanel.h qt/src/cmdkpanel.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): CmdKPanel — tabbed slide-in container

Slide-in QWidget with QTabBar (Dashboard, AI) + QStackedWidget.
Owns the QPropertyAnimation on panelWidth that was previously in
AiPanel.

Constructor takes the existing AiPanel pointer and re-parents it as
the AI tab — AiPanel's behavior is unchanged, just nested one level
deeper.

togglePanel() starts/stops the MyNodeDashboard poller so polling
only runs when the panel is open.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Wire Cmd+K to CmdKPanel; demote AiPanel

**Files:**
- Modify: `qt/src/mainwindow.h`
- Modify: `qt/src/mainwindow.cpp`

- [ ] **Step 1: Update mainwindow.h**

In `qt/src/mainwindow.h`, locate the `AiPanel* aiPanel_` member (search):

```bash
cd /private/tmp/dinero-v8-mnd-mvp
grep -n "AiPanel\*\s*aiPanel_\|class AiPanel" qt/src/mainwindow.h
```

Add a forward declaration for `dinero::qt::dashboard::CmdKPanel` near the existing `class AiPanel;` forward decl:

```cpp
namespace dinero::qt::dashboard { class CmdKPanel; }
```

Add the new member alongside `aiPanel_`:

```cpp
  dinero::qt::dashboard::CmdKPanel* cmdKPanel_ = nullptr;
```

(Keep `aiPanel_` — it still exists, just owned by `cmdKPanel_` instead of by the layout directly.)

- [ ] **Step 2: Update the panel instantiation in mainwindow.cpp**

Find the existing `aiPanel_ = new AiPanel(...)` block (around line 2071):

```bash
grep -n "aiPanel_ = new AiPanel" qt/src/mainwindow.cpp
```

Read the surrounding 8 lines for context. Replace the existing:

```cpp
aiPanel_ = new AiPanel(rpc_->datadir(), contentArea);
aiPanel_->setPanelWidth(0);
contentLayout->addWidget(aiPanel_);
```

with:

```cpp
aiPanel_ = new AiPanel(rpc_->datadir(), nullptr);  // re-parented by CmdKPanel below
cmdKPanel_ = new dinero::qt::dashboard::CmdKPanel(rpc_, aiPanel_, contentArea);
cmdKPanel_->setPanelWidth(0);
contentLayout->addWidget(cmdKPanel_);
```

Also add the include at the top of mainwindow.cpp near the existing `#include "aipanel.h"`:

```cpp
#include "cmdkpanel.h"
```

- [ ] **Step 3: Update the Cmd+K handler**

Find the existing `MainWindow::onToggleAiPanel` function (around line 14826):

```bash
grep -n "void MainWindow::onToggleAiPanel" qt/src/mainwindow.cpp
```

Replace its body:

```cpp
void MainWindow::onToggleAiPanel() {
  if (cmdKPanel_) {
    cmdKPanel_->togglePanel();
  }
}
```

(The function name stays — it's a slot referenced by the shortcut connection. Only the body changes.)

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git add qt/src/mainwindow.h qt/src/mainwindow.cpp
git commit -S -m "$(cat <<'EOF'
refactor(qt): Cmd+K opens CmdKPanel (Dashboard primary, AI as tab)

Replaces direct AiPanel-as-Cmd+K-target with the new CmdKPanel
container. AiPanel is preserved as the AI tab inside CmdKPanel —
the existing AiPanel instance is constructed with nullptr parent and
re-parented by CmdKPanel.

onToggleAiPanel slot name retained for backward-compat with the
QShortcut connection; only its body changes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Full app build + manual sanity

**Files:** none (verification only)

- [ ] **Step 1: Full app build**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
cmake --build build-mnd --target dinero-qt -j8 2>&1 | tail -10
```

Expected: `[100%] Built target dinero-qt` (or whatever the executable target is named — `dinero-qt.app` on macOS). No new warnings on lines you touched. Pre-existing warnings allowed.

If build fails:
- Symbol-not-found for a section method → check that the source file made it into the executable's source list (Task 9 Step 3 + Task 10 Step 3)
- `cmdkpanel.h: No such file` → check the include in mainwindow.cpp + the file is in the new worktree
- Missing slot `onIdentityUpdated` → check Q_OBJECT macros are present in the section headers (they're declared in Task 4/6/7 Step 1)

- [ ] **Step 2: Run the full ctest suite for our new tests**

```bash
cd /private/tmp/dinero-v8-mnd-mvp/build-mnd
ctest -R "NodePoller|IdentitySection|NetworkSection|PeersSection" --output-on-failure
```

Expected: all 4 suites pass, total 17+ tests.

- [ ] **Step 3: Launch the app + manual checklist**

Run `dinero-qt` and verify:

- [ ] Cmd+K opens panel from the right within a frame (no jank)
- [ ] Default tab is **Dashboard** (not AI)
- [ ] Identity section shows your node_id (40 chars, grouped 4-by-4) and at least one status line
- [ ] Network section shows two height bars; if your node is in sync with peers, "● in sync" is visible
- [ ] Peers section shows your connected peers with Q-scores (or `—` if Dynamic P2P hasn't yet evaluated)
- [ ] Click the AI tab → AiPanel renders normally (configScreen or chatScreen depending on saved API key state)
- [ ] Click back to Dashboard → returns to your node view
- [ ] Cmd+K again closes the panel
- [ ] Cmd+K again re-opens it; last-active tab is restored (Dashboard or AI)
- [ ] Sections update within ~5s of changes (e.g., disconnect a peer, see it disappear)

- [ ] **Step 4: Stop daemon, watch degraded mode**

```bash
# In a separate terminal:
dinero-cli stop
```

Wait ~15s, observe in the panel:
- [ ] After 3 failed polls (≤15s with 5s cadence), the reachability line turns red and says "UNREACHABLE · daemon not responding"
- [ ] The dashboard freezes last values (no flicker, no crash)

Restart daemon (`dinerod -daemon`) and verify the dashboard returns to green within ~10s.

- [ ] **Step 5: Commit a short sanity log to the worktree**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
echo "Phase 1 MVP manual sanity completed $(date -u +%FT%TZ)" \
  >> docs/superpowers/plans/2026-05-24-my-node-dashboard-mvp-sanity.log
git add docs/superpowers/plans/2026-05-24-my-node-dashboard-mvp-sanity.log
git commit -S -m "$(cat <<'EOF'
docs: record Phase 1 MVP manual sanity completion

Confirms Cmd+K opens panel, Dashboard is default tab, identity +
network + peers render with live daemon data, AI tab still works,
degraded mode triggers on daemon stop and clears on restart.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Push branch + open PR

**Files:** none (process)

- [ ] **Step 1: Push the branch**

```bash
cd /private/tmp/dinero-v8-mnd-mvp
git push -u origin feature/my-node-dashboard-mvp
```

- [ ] **Step 2: Open draft PR**

```bash
gh pr create --draft --title "feat(qt): MyNodeDashboard Phase 1 (MVP) — Cmd+K becomes 'you are a node'" --body "$(cat <<'EOF'
## Summary

Phase 1 of the [MyNodeDashboard design](https://github.com/DineroLabs/dinero-v8/blob/dinero-main/docs/superpowers/specs/2026-05-24-my-node-dashboard-design.md). Replaces the underutilized `AiPanel`-as-Cmd+K-default with a tabbed `CmdKPanel` whose primary tab is a 3-section operator-grade dashboard. AI is preserved as a tab — no functional regression.

## What's in

| Component | Detail |
|---|---|
| `CmdKPanel` | New top-level slide-in container. Owns the QPropertyAnimation, hosts QTabBar + QStackedWidget. |
| `MyNodeDashboard` | New widget composing the 3 sections + NodePoller. |
| `NodePoller` | New 5s polling driver. Issues parallel RPCs via existing `RpcClient`. Emits typed signals (`identityUpdated`, `chainInfoUpdated`, `peersUpdated`, `daemonStateChanged`). |
| `IdentitySection` (⚡ YOU) | node_id + reachability + relaying + mining + uptime. |
| `NetworkSection` (📡 NETWORK) | Chain tip race (you/net bars), delta annotation, difficulty/mempool/median-fee grid. |
| `PeersSection` (🛰 PEERS) | Sortable table with stoplight glyphs (●◐⚠○), click-to-expand inline detail. |
| `mainwindow.cpp` rewire | Cmd+K → `cmdKPanel_->togglePanel()`. `aiPanel_` preserved + re-parented into CmdKPanel. |

## Test plan

- [x] Local: 4 new ctest suites (NodePoller, IdentitySection, NetworkSection, PeersSection) — 17+ unit tests, all green
- [x] Local: full dinero-qt app builds clean
- [x] Local: manual sanity checklist completed (Cmd+K, tab switch, degraded mode, restore)
- [ ] CI: full Tests + core-heavy lanes
- [ ] Mac canary: a single TestFlight-equivalent (or signed-but-not-notarized local build) used for one day

## What's not in this PR (Phase 2/3)

- Contribution sparklines + Decentralization score
- Discovery section (relay-hint cache)
- Topology view (radial peer map)
- Right-click peer actions (disconnect/ban/pin)
- Relaying toggle in Identity
- "Dial now" action on hint cache rows

These land in follow-up PRs scoped to Phase 2 and Phase 3.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Wait for CI to confirm green, then mark ready-for-review**

```bash
# Poll CI every 90s up to ~20 min
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
  STATUS=$(gh pr checks $(gh pr view --json number --jq .number) --repo DineroLabs/dinero-v8 2>&1)
  if echo "$STATUS" | grep -qE "fail|FAILURE"; then
    echo "FAILURE at iter $i:"
    echo "$STATUS" | head -10
    break
  fi
  if [ "$(echo "$STATUS" | grep -c "pending")" = "0" ]; then
    echo "all checks complete"
    echo "$STATUS"
    break
  fi
  echo "iter $i: still pending"
  sleep 90
done
```

If green:

```bash
gh pr ready
```

Stop here. Don't merge. Phase 1 lands when the human reviewer approves.

---

## Coverage map (self-review)

| Spec requirement (Phase 1 only) | Task |
|---|---|
| `CmdKPanel` container + tab switcher | 10 |
| `AiPanel` demoted to a tab | 10 + 11 |
| Slide-in animation reused (`QPropertyAnimation` on `panelWidth`) | 10 |
| Cmd+K bound to `CmdKPanel` not `AiPanel` | 11 |
| `NodePoller` with 5s cadence | 2 |
| `NodePoller` issues all 5 RPCs per tick | 2 |
| `NodePoller` emits typed signals | 2 |
| `NodePoller` degraded mode after 3 failures | 2 + tests in 3 |
| `IdentitySection` ⚡ YOU | 4 |
| Node_id formatted in 4-char groups | 4 + tests in 5 |
| Reachability glyph + line | 4 + tests in 5 |
| Relaying status with count + grace | 4 + tests in 5 |
| Mining status with shares/min | 4 + tests in 5 |
| `NetworkSection` 📡 NETWORK | 6 |
| Chain tip race (two bars) | 6 |
| Tip delta annotation | 6 + tests in 6 |
| Difficulty / mempool / median fee grid | 6 |
| `PeersSection` 🛰 PEERS | 7 |
| Sortable table (Q descending default) | 7 + tests in 8 |
| Stoplight glyph per Q range | 7 + tests in 8 |
| Click row → inline detail expand | 7 |
| Relay-virtual peer "(via X)" annotation | 7 + tests in 8 |
| Polling lifecycle (start on open, stop on close) | 10 |
| Full app build clean | 12 |
| Manual sanity checklist | 12 |
| PR opened on a feature branch | 13 |
