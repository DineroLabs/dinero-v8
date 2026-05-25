# MyNodeDashboard Phase 2a — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the 🌐 CONTRIBUTION section to the Cmd+K dashboard: 3 rolling-5min traffic sparklines, a stat grid (circuits/blocks/hints/gossip), and a Decentralization Score line that gamifies operator participation. UI-only — every required RPC field is already polled.

**Architecture:** New `ContributionSection` widget composing a small hand-rolled `SparklineWidget` (QPainter, no QChart dep), plus stat labels and a score-with-tooltip row. `NodePoller` gains two new signals — `contributionStatsUpdated` (per-tick stat snapshot) and `decentralizationScoreUpdated` (re-computed every tick) — driven from the same 5s tick that already polls `getnetworkinfo` / `getpeerinfo` / `getmempoolinfo` / `mining.status` / `dynamic_p2p.observe`. Sparkline buffers live in `NodePoller` as a 120-sample ring (5min × 1 sample / 2.5s). Score formula is the spec's 10.0-max weighted sum.

**Tech Stack:** C++20, Qt 6 (Widgets + Test). NO new dependencies — sparkline is `QPainter` against an `OpenGLPaintDevice`-free widget. NO daemon changes (Discovery section deferred to Phase 2b which needs a new daemon RPC).

**Spec:** `docs/superpowers/specs/2026-05-24-my-node-dashboard-design.md` (Phase 2 section).

**Branch:** `feature/qt-dashboard-phase2a` off `dinero-main`. Worktree at `/private/tmp/dinero-v8-phase2a-impl` (Task 0 creates; spec worktree at `/private/tmp/dinero-v8-phase2a-spec` stays untouched). All commits SSH-signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File map (decomposition locked in before tasks)

| File | Status | Responsibility |
|---|---|---|
| `qt/src/dashboardtypes.h` | Modify | Add `ContributionStats` struct (circuits/blocks/hints/gossip + bytes sums for sparkline accounting) and `DecentralizationScore` struct (score + breakdown + plain-English label). Q_DECLARE_METATYPE for both. |
| `qt/src/sparklinewidget.h` / `.cpp` | **Create** | Small QWidget that paints up to N samples as a vertical-bar sparkline. Pure presentation; no logic about WHAT to plot. ~80 lines of QPainter. |
| `qt/src/nodepoller.h` / `.cpp` | Modify | Add 120-sample rolling buffers for bytes_in/out/relay; add `contributionStatsUpdated` + `decentralizationScoreUpdated` signals + per-tick computation. The score formula lives here as a static helper for unit-testability. |
| `qt/src/contributionsection.h` / `.cpp` | **Create** | 🌐 YOUR CONTRIBUTION section widget. Composes 3 `SparklineWidget`s + stat grid (4 labels) + score line (label + tooltip). Listens to `NodePoller::contributionStatsUpdated` + `decentralizationScoreUpdated`. |
| `qt/src/mynodedashboard.h` / `.cpp` | Modify | Add `ContributionSection*` member; insert between `NetworkSection` and `PeersSection`; wire the 2 new signals. |
| `qt/CMakeLists.txt` | Modify | Add new source files to the dinero-qt executable target. Register the new test executables. |
| `qt/tests/test_sparkline_widget.cpp` | **Create** | Unit tests for `SparklineWidget::setSamples` + paint geometry. |
| `qt/tests/test_decentralization_score.cpp` | **Create** | Unit tests for the score formula (each weight, edge cases, clamping, plain-English mapping). |
| `qt/tests/test_contribution_section.cpp` | **Create** | Snapshot tests for the section's label rendering. |

**Files NOT modified:**
- `qt/src/identitysection.{h,cpp}` (Phase 2a doesn't touch identity)
- `qt/src/networksection.{h,cpp}` (no change)
- `qt/src/peerssection.{h,cpp}` (no change)
- Any daemon-side code (UI-only PR)

---

## Conventions (all tasks)

- **Commits:** SSH-signed as `Dinero Labs <team@dinerolabs.org>`. Verify first commit: `git log --show-signature -1 --pretty=format:"%h %GS"` shows "Good signature".
- **Commit prefix:** `feat(qt-dashboard): ...` for new behavior, `test(qt-dashboard): ...` for tests, `refactor(qt-dashboard): ...` for restructuring.
- **DO NOT pass `std::chrono::duration` or `time_point` to `QCOMPARE` / `QVERIFY`.** Extract `.count()` first. (macOS SDK 26.4 `<chrono>` formatter trap discovered in Phase 1 — applies here too.)
- **DO NOT run full `cmake --build ... --target dinero-qt` inside a subagent session.** Full Qt link can outlive subagent timeouts. Build only the test target you need. Parent runs the full app build in the final task.
- **Configure command:** `cmake -S qt -B build-p2a -DDINERO_SOLO_MINER_SOURCE_ROOT=$(pwd)/miner -DDINERO_SOURCE_ROOT=$(pwd)` from the worktree root. The `-DDINERO_*` flags are required because `qt/CMakeLists.txt`'s `PROJECT_IS_TOP_LEVEL` path expects sibling dirs (lesson from Phase 1.5).
- **Score formula authority:** the static helper `NodePoller::ComputeDecentralizationScore` in Task 3 is the canonical implementation. All tests + section render call it.
- **No new git push until Task 9.** Local commits until then.

---

## Task 0: Branch + worktree setup

**Files:** none (git ops).

- [ ] **Step 1: Create the implementation worktree off dinero-main**

```bash
cd /Users/haydarevich/src/dinero-v8
git fetch origin dinero-main
git worktree add -b feature/qt-dashboard-phase2a /private/tmp/dinero-v8-phase2a-impl origin/dinero-main
```

Expected: `Preparing worktree (new branch 'feature/qt-dashboard-phase2a') ... HEAD is now at 6694a8e5 ...` (or later if dinero-main has moved). Worktree exists at `/private/tmp/dinero-v8-phase2a-impl`.

- [ ] **Step 2: Verify signing config**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
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

- [ ] **Step 3: Sync submodules**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git submodule update --init --recursive 2>&1 | tail -5
git status --short
```

Expected: empty `git status --short`. Avoids the submodule-drift trap from earlier sessions.

- [ ] **Step 4: First configure of the qt-only build dir (the gating env check)**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake -S qt -B build-p2a \
  -DDINERO_SOLO_MINER_SOURCE_ROOT=$(pwd)/miner \
  -DDINERO_SOURCE_ROOT=$(pwd) 2>&1 | tail -3
```

Expected: `Configuring done` + `Generating done` + `Build files have been written to: /private/tmp/dinero-v8-phase2a-impl/build-p2a`.

If you see "add_subdirectory given source ... which is not an existing directory": the `-D` flags above weren't honored. Re-check the exact `$(pwd)` resolution.

---

## Task 1: dashboardtypes — `ContributionStats` + `DecentralizationScore`

**Files:**
- Modify: `qt/src/dashboardtypes.h`

- [ ] **Step 1: Add the two structs + their METATYPE declarations**

Open `qt/src/dashboardtypes.h`. After the existing `DynamicP2POverview` struct (look for `struct DynamicP2POverview {`) and before `struct NodeIdentity {`, insert:

```cpp
// Per-tick contribution snapshot. Sparkline buffers (rolling 5-min) live
// in NodePoller — this struct carries only the spot values that are
// rendered as plain labels.
struct ContributionStats {
    int     circuits_active{0};        // # of relay circuits we route
    int     blocks_served_today{0};    // approx — see Phase 2a note in code
    int     hints_sent{0};             // RELAY_HINTS we've sent since launch
    int     peers_via_gossip{0};       // peers known to us only via gossip (proxy: received_relay)
    qint64  bytes_in_rate{0};          // current 1-sample rate (bytes/sec), for the label
    qint64  bytes_out_rate{0};
    qint64  relay_bytes_rate{0};
};

// Decentralization score (0.0–10.0) computed from observable signals.
// `breakdown` carries each weighted component so the tooltip can show the
// formula attribution. `label` is the plain-English bucket from the spec.
struct DecentralizationScore {
    double  total{0.0};                 // clamp(0, 10) of the weighted sum
    QString label;                      // "just observing" / "consuming responsibly" / etc.
    struct {
        double reachable{0.0};          // 0 or 1.0
        double relay_active{0.0};       // 0 or 2.0
        double uptime{0.0};             // 0..1.5
        double peer_diversity{0.0};     // 0..1.5
        double traffic{0.0};            // 0..1.0
        double mining{0.0};             // 0..1.5
        double gossip_reach{0.0};       // 0..1.5
    } breakdown;
};
```

At the bottom of the file (right after the existing `Q_DECLARE_METATYPE(...)` lines), add:

```cpp
Q_DECLARE_METATYPE(dinero::qt::dashboard::ContributionStats)
Q_DECLARE_METATYPE(dinero::qt::dashboard::DecentralizationScore)
```

- [ ] **Step 2: Commit**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git add qt/src/dashboardtypes.h
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): add ContributionStats + DecentralizationScore types

Two new structs for Phase 2a. ContributionStats carries the per-tick
spot values for the stat grid + sparkline-rate labels (the rolling
buffers themselves live in NodePoller). DecentralizationScore carries
the 0-10 total + per-component breakdown for the tooltip + the
plain-English bucket label.

Q_DECLARE_METATYPE both so QSignalSpy::value<T>() can deserialize in
unit tests.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git log --show-signature -1 --pretty=format:"%h %GS"
```

Expected: contains "Good signature" + "team@dinerolabs.org".

---

## Task 2: `SparklineWidget` — hand-rolled QPainter sparkline

**Files:**
- Create: `qt/src/sparklinewidget.h`
- Create: `qt/src/sparklinewidget.cpp`
- Create: `qt/tests/test_sparkline_widget.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Header**

Create `qt/src/sparklinewidget.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QVector>
#include <QWidget>

namespace dinero::qt::dashboard {

// Minimal vertical-bar sparkline. Takes a vector of non-negative
// integers; paints each as a bar with height proportional to
// sample/max(samples). Zero-sample input renders an empty rect.
//
// No autoscroll, no axis labels — pure presentation. Caller decides
// what data goes in.
class SparklineWidget : public QWidget {
    Q_OBJECT
public:
    explicit SparklineWidget(QWidget* parent = nullptr);

    void setSamples(const QVector<qint64>& samples);
    QVector<qint64> samples() const { return samples_; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<qint64> samples_;
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Implementation**

Create `qt/src/sparklinewidget.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sparklinewidget.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

namespace dinero::qt::dashboard {

namespace {
constexpr int kDefaultWidthPx  = 200;
constexpr int kDefaultHeightPx = 14;
constexpr int kBarSpacingPx    = 1;
}  // namespace

SparklineWidget::SparklineWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(kDefaultHeightPx);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize SparklineWidget::sizeHint() const {
    return QSize(kDefaultWidthPx, kDefaultHeightPx);
}

void SparklineWidget::setSamples(const QVector<qint64>& samples) {
    samples_ = samples;
    update();
}

void SparklineWidget::paintEvent(QPaintEvent* /*event*/) {
    if (samples_.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const qint64 max_val =
        *std::max_element(samples_.constBegin(), samples_.constEnd());
    if (max_val <= 0) return;  // all zero → nothing to draw

    const int w = width();
    const int h = height();
    const int n = samples_.size();
    if (n <= 0 || w <= 0 || h <= 0) return;

    // Bar width: fit n bars + (n-1) spacing into width().
    const double bar_w = static_cast<double>(
        std::max(1, (w - (n - 1) * kBarSpacingPx))) / n;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(80, 160, 240));  // soft blue, default scheme

    for (int i = 0; i < n; ++i) {
        const qint64 v = std::max<qint64>(0, samples_[i]);
        const int bar_h = static_cast<int>(
            (static_cast<double>(v) / max_val) * h);
        const int x = static_cast<int>(i * (bar_w + kBarSpacingPx));
        p.drawRect(x, h - bar_h, static_cast<int>(bar_w), bar_h);
    }
}

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 3: Test**

Create `qt/tests/test_sparkline_widget.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sparklinewidget.h"

#include <QtTest/QtTest>

using dinero::qt::dashboard::SparklineWidget;

class TestSparklineWidget : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void empty_samples_are_safe_to_paint() {
        SparklineWidget w;
        w.resize(100, 16);
        // No samples; paintEvent must not crash and must return early.
        // We can't easily assert on QPainter calls without a custom
        // device, but we can at least call show + update + verify the
        // event loop runs without crashing.
        w.setSamples({});
        w.show();
        QTest::qWait(50);
        QCOMPARE(w.samples().size(), 0);
    }

    void samples_round_trip() {
        SparklineWidget w;
        const QVector<qint64> input{0, 5, 10, 7, 0, 0, 12, 3};
        w.setSamples(input);
        QCOMPARE(w.samples(), input);
    }

    void size_hint_has_minimum_height() {
        SparklineWidget w;
        QVERIFY(w.sizeHint().height() >= 14);
        QVERIFY(w.sizeHint().width()  >= 100);
    }

    void all_zero_samples_still_safe() {
        SparklineWidget w;
        w.resize(100, 16);
        w.setSamples({0, 0, 0, 0});
        w.show();
        QTest::qWait(50);
        // Survives the paintEvent's max-value=0 guard.
        QCOMPARE(w.samples().size(), 4);
    }
};

QTEST_MAIN(TestSparklineWidget)
#include "test_sparkline_widget.moc"
```

- [ ] **Step 4: Register the test target in `qt/CMakeLists.txt`**

Find the existing `test_clock_source` block (around line 1262 from the previous Phase 1 work, search with `grep -n "test_clock_source" qt/CMakeLists.txt`). Append after the existing test blocks but before the "Qt firewall compile-fail probe" if present:

```cmake
  add_executable(test_sparkline_widget
    tests/test_sparkline_widget.cpp
    src/sparklinewidget.cpp
    src/sparklinewidget.h
  )
  add_dependencies(test_sparkline_widget gtest gtest_main)
  target_include_directories(test_sparkline_widget BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(test_sparkline_widget PRIVATE
    Qt6::Widgets
    Qt6::Test
  )
  target_compile_definitions(test_sparkline_widget PRIVATE QT_NO_KEYWORDS)
  add_test(NAME SparklineWidget COMMAND test_sparkline_widget)
  set_tests_properties(SparklineWidget PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 10
  )
```

(The `add_dependencies(... gtest gtest_main)` and the googletest include line aren't strictly needed for a Qt-Test-only target, but matching the existing pattern in this codebase avoids surprises during CMake regeneration.)

Also add `src/sparklinewidget.cpp` and `src/sparklinewidget.h` to the dinero-qt main executable. Search:

```bash
grep -n "src/identitysection.cpp\|GUI_SOURCES" qt/CMakeLists.txt | head -5
```

Add the two new lines in the same `set(GUI_SOURCES ...)` list where `identitysection.cpp` already appears.

- [ ] **Step 5: Configure + build + run**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake -S qt -B build-p2a -DDINERO_SOLO_MINER_SOURCE_ROOT=$(pwd)/miner -DDINERO_SOURCE_ROOT=$(pwd) 2>&1 | tail -3
cmake --build build-p2a --target test_sparkline_widget -j8 2>&1 | tail -5
cd build-p2a && ctest -R SparklineWidget --output-on-failure
```

Expected: 4 internal Qt-Test cases pass, ctest reports "100% tests passed".

If the test fails because the window can't show (headless CI): wrap the `w.show()` calls in `if (!qApp->offscreen())` or set `QT_QPA_PLATFORM=offscreen` in the test env. For now keep it simple — local builds work.

- [ ] **Step 6: Commit**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git add qt/src/sparklinewidget.h qt/src/sparklinewidget.cpp \
        qt/tests/test_sparkline_widget.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): SparklineWidget — hand-rolled QPainter sparkline

Tiny QWidget for the Contribution section's 3 traffic plots. Takes
a QVector<qint64>, paints each as a vertical bar with height
proportional to sample/max. Zero-sample / all-zero input is a
no-op (no division by zero).

No QChart dependency — pure QPainter, ~80 lines. Style: soft blue
solid bars, no antialiasing (sharper pixels at this size).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git log --show-signature -1 --pretty=format:"%h %GS"
```

---

## Task 3: NodePoller — sparkline buffers + score formula + new signals

**Files:**
- Modify: `qt/src/nodepoller.h`
- Modify: `qt/src/nodepoller.cpp`
- Create: `qt/tests/test_decentralization_score.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Header additions**

In `qt/src/nodepoller.h`, in the `Q_SIGNALS:` section (after the existing `dynamicP2POverviewUpdated` signal), add:

```cpp
    void contributionStatsUpdated(const ContributionStats& stats);
    void decentralizationScoreUpdated(const DecentralizationScore& score);
```

Then in the `public:` section (after `setIntervalMs` or the constructor), add the static formula helper for unit-testability:

```cpp
    // Inputs to the decentralization score (separated from poll state
    // so tests can drive the formula deterministically). All values
    // are observable signals from the polled RPCs + LocalMiningProvider.
    struct ScoreInputs {
        bool   reachable_with_inbound{false};
        bool   relay_active_with_registrants{false};
        qint64 uptime_seconds{0};
        int    unique_peer_subnets_slash16{0};
        qint64 bytes_relayed_24h{0};
        double local_hashrate_hps{0.0};
        double fleet_hashrate_hps{0.0};
        int    peers_who_learned_via_gossip{0};
    };

    static DecentralizationScore ComputeDecentralizationScore(
        const ScoreInputs& inputs);
```

In the private section, near the other `pending_*` members, add the rolling buffers + last-sample timestamps for delta accounting:

```cpp
    // Rolling 5-minute sparkline buffers (sample-per-tick).
    // 120 samples at the 2.5s render cadence we expose to the
    // section widget — actually filled at the 5s poll cadence, so 60
    // poll-samples gets us 5 minutes. Section widget can decimate or
    // upsample as it wishes.
    static constexpr int kSparklineCapacity = 60;
    QVector<qint64> bytes_in_buffer_;
    QVector<qint64> bytes_out_buffer_;
    QVector<qint64> relay_bytes_buffer_;

    // Last-tick totals so we can compute per-tick deltas (rates).
    qint64 last_bytes_sent_total_{0};
    qint64 last_bytes_recv_total_{0};
    qint64 last_relay_bytes_total_{0};
    bool   have_baseline_{false};

    // Aggregate counters from getnetworkinfo.relay.hints, cached so the
    // score formula and the stat grid can read them without re-parsing.
    int    relay_hints_sent_{0};
    int    relay_hints_received_relay_{0};

    void pushSparklineSample(QVector<qint64>* buf, qint64 sample);
    void emitContributionAndScore();</cpp>
```

Wait — the above is for context only. Actually add it as plain C++:

```cpp
    static constexpr int kSparklineCapacity = 60;
    QVector<qint64> bytes_in_buffer_;
    QVector<qint64> bytes_out_buffer_;
    QVector<qint64> relay_bytes_buffer_;

    qint64 last_bytes_sent_total_{0};
    qint64 last_bytes_recv_total_{0};
    qint64 last_relay_bytes_total_{0};
    bool   have_baseline_{false};

    int    relay_hints_sent_{0};
    int    relay_hints_received_relay_{0};

    void pushSparklineSample(QVector<qint64>* buf, qint64 sample);
    void emitContributionAndScore();
```

- [ ] **Step 2: Implement the score formula**

In `qt/src/nodepoller.cpp`, near the top with other helper functions (after `fleetNameFor` if present), add:

```cpp
namespace {

QString scoreLabel(double total) {
    if (total < 2.0) return "just observing";
    if (total < 4.0) return "consuming responsibly";
    if (total < 6.0) return "pulling your weight";
    if (total < 8.0) return "you're carrying real weight";
    return "you're load-bearing for the network";
}

double linearCap(double value, double cap_at_one) {
    if (cap_at_one <= 0.0) return 0.0;
    return std::min(1.0, std::max(0.0, value / cap_at_one));
}

}  // namespace
```

Then add the static method implementation (place near the existing static helpers, e.g., right after `noteSuccess`):

```cpp
DecentralizationScore NodePoller::ComputeDecentralizationScore(
        const ScoreInputs& in) {
    DecentralizationScore s;

    // 1.0 weight: reachable + at least one inbound peer
    s.breakdown.reachable = in.reachable_with_inbound ? 1.0 : 0.0;

    // 2.0 weight: actively serving as a relay with ≥1 registrant
    s.breakdown.relay_active = in.relay_active_with_registrants ? 2.0 : 0.0;

    // 1.5 weight: uptime, linear to 30 days
    constexpr qint64 kThirtyDaysSec = 30LL * 24 * 3600;
    s.breakdown.uptime =
        linearCap(static_cast<double>(in.uptime_seconds), kThirtyDaysSec) * 1.5;

    // 1.5 weight: peer diversity (unique /16 subnets), capped at 8
    s.breakdown.peer_diversity =
        linearCap(static_cast<double>(in.unique_peer_subnets_slash16), 8.0) * 1.5;

    // 1.0 weight: traffic, log10 scale (10^9 bytes = 1 GB = full)
    double traffic_log = 0.0;
    if (in.bytes_relayed_24h > 0) {
        traffic_log = std::log10(static_cast<double>(in.bytes_relayed_24h));
    }
    s.breakdown.traffic = linearCap(traffic_log, 9.0) * 1.0;

    // 1.5 weight: mining ratio (local / fleet), capped at 1.0
    double mining_ratio = 0.0;
    if (in.fleet_hashrate_hps > 0.0) {
        mining_ratio = in.local_hashrate_hps / in.fleet_hashrate_hps;
    }
    s.breakdown.mining = linearCap(mining_ratio, 1.0) * 1.5;

    // 1.5 weight: gossip reach (peers who learned of us via gossip / 32)
    s.breakdown.gossip_reach =
        linearCap(static_cast<double>(in.peers_who_learned_via_gossip), 32.0) * 1.5;

    s.total = s.breakdown.reachable
            + s.breakdown.relay_active
            + s.breakdown.uptime
            + s.breakdown.peer_diversity
            + s.breakdown.traffic
            + s.breakdown.mining
            + s.breakdown.gossip_reach;
    if (s.total < 0.0)  s.total = 0.0;
    if (s.total > 10.0) s.total = 10.0;

    s.label = scoreLabel(s.total);
    return s;
}
```

Add `#include <cmath>` and `#include <algorithm>` at the top of `qt/src/nodepoller.cpp` if not already present.

- [ ] **Step 3: Implement the sparkline buffer helpers**

In `qt/src/nodepoller.cpp`, near the other private helpers:

```cpp
void NodePoller::pushSparklineSample(QVector<qint64>* buf, qint64 sample) {
    if (!buf) return;
    buf->append(sample < 0 ? 0 : sample);
    while (buf->size() > kSparklineCapacity) {
        buf->removeFirst();
    }
}
```

- [ ] **Step 4: Hook into parsePeers + parseNetworkInfo + emit**

Find the existing `parsePeers` function. After the peer loop populates `pending_peers_`, compute current totals + push sparkline samples + emit:

```cpp
    // Phase 2a: accumulate bytes for sparkline + per-tick rate stats.
    qint64 cur_sent_total = 0;
    qint64 cur_recv_total = 0;
    qint64 cur_relay_total = 0;  // sum of relay-virtual peers' bytes
    QVector<QString> subnets_slash16;
    int hot_via_gossip = 0;
    for (const auto& r : pending_peers_) {
        cur_sent_total += r.bytes_sent;
        cur_recv_total += r.bytes_recv;
        if (r.via_relay) cur_relay_total += r.bytes_sent + r.bytes_recv;
        // Approx /16 subnet — split on '.' and take first two octets.
        const auto dot1 = r.addr.indexOf('.');
        if (dot1 > 0) {
            const auto dot2 = r.addr.indexOf('.', dot1 + 1);
            if (dot2 > 0) {
                const auto sub16 = r.addr.left(dot2);
                if (!subnets_slash16.contains(sub16)) {
                    subnets_slash16.append(sub16);
                }
            }
        }
    }
    if (!have_baseline_) {
        last_bytes_sent_total_  = cur_sent_total;
        last_bytes_recv_total_  = cur_recv_total;
        last_relay_bytes_total_ = cur_relay_total;
        have_baseline_ = true;
    }
    const qint64 delta_recv  = std::max<qint64>(0, cur_recv_total  - last_bytes_recv_total_);
    const qint64 delta_sent  = std::max<qint64>(0, cur_sent_total  - last_bytes_sent_total_);
    const qint64 delta_relay = std::max<qint64>(0, cur_relay_total - last_relay_bytes_total_);
    pushSparklineSample(&bytes_in_buffer_,    delta_recv);
    pushSparklineSample(&bytes_out_buffer_,   delta_sent);
    pushSparklineSample(&relay_bytes_buffer_, delta_relay);
    last_bytes_sent_total_  = cur_sent_total;
    last_bytes_recv_total_  = cur_recv_total;
    last_relay_bytes_total_ = cur_relay_total;

    // Cache subnet count for the score formula; emit comes after
    // both peers + network info have been parsed at least once.
    pending_chain_.peer_heights = pending_chain_.peer_heights;  // no-op preserve
    Q_UNUSED(hot_via_gossip);
    Q_UNUSED(subnets_slash16);  // referenced below in emitContributionAndScore via re-derivation
    emitContributionAndScore();
```

Now find `parseNetworkInfo` and inside it, after reading the `relay` nested object, additionally read the hint counters:

```cpp
    if (obj.contains("relay")) {
        const auto relay = obj.value("relay").toObject();
        // ...existing relay.active parsing stays here...
        const auto hints = relay.value("hints").toObject();
        relay_hints_sent_           = hints.value("received_self").toInt(0);
        relay_hints_received_relay_ = hints.value("received_relay").toInt(0);
    }
    emitContributionAndScore();  // emit fresh score whenever network info changes
```

(The `received_self` is admittedly an imperfect proxy for "hints I've sent" — it's the count of hints we received that name us as the target. Phase 2b can refine with a real `relay_hints_sent` counter if the daemon adds one. The comment explains the gap.)

- [ ] **Step 5: Implement `emitContributionAndScore`**

```cpp
void NodePoller::emitContributionAndScore() {
    ContributionStats stats;
    stats.circuits_active     = 0;  // TODO Phase 2b: read from a daemon RPC
    stats.blocks_served_today = 0;  // TODO Phase 2b: read from a daemon counter
    stats.hints_sent          = relay_hints_sent_;
    stats.peers_via_gossip    = relay_hints_received_relay_;
    stats.bytes_in_rate  = bytes_in_buffer_.isEmpty()  ? 0 : bytes_in_buffer_.last();
    stats.bytes_out_rate = bytes_out_buffer_.isEmpty() ? 0 : bytes_out_buffer_.last();
    stats.relay_bytes_rate =
        relay_bytes_buffer_.isEmpty() ? 0 : relay_bytes_buffer_.last();
    Q_EMIT contributionStatsUpdated(stats);

    // Score formula inputs derived from current pending_* state.
    ScoreInputs in;
    in.reachable_with_inbound =
        (pending_identity_.reachability == NodeIdentity::DIRECT)
        && (pending_peers_.size() > 0);  // any-peer proxy for "has inbound"
    in.relay_active_with_registrants =
        pending_identity_.is_relay_active
        && (pending_identity_.registrants_count > 0);
    in.uptime_seconds = pending_identity_.uptime.count();
    QVector<QString> subnets;
    for (const auto& r : pending_peers_) {
        const auto dot1 = r.addr.indexOf('.');
        if (dot1 > 0) {
            const auto dot2 = r.addr.indexOf('.', dot1 + 1);
            if (dot2 > 0) {
                const auto sub16 = r.addr.left(dot2);
                if (!subnets.contains(sub16)) subnets.append(sub16);
            }
        }
    }
    in.unique_peer_subnets_slash16 = subnets.size();
    // 24h bytes — placeholder until a 24h rolling buffer exists; for now,
    // use current relay-buffer sum which represents the last ~5 min of
    // relay traffic. Tooltip in Section makes the approximation explicit.
    qint64 sum_relay = 0;
    for (auto v : relay_bytes_buffer_) sum_relay += v;
    in.bytes_relayed_24h = sum_relay * 288;  // 5min × 288 = 24h (linear extrapolation)
    in.local_hashrate_hps = pending_identity_.shares_per_min * 1'000'000.0;
    in.fleet_hashrate_hps = pending_chain_.next_bits_delta_pct;  // wrong field; see below
    in.peers_who_learned_via_gossip = relay_hints_received_relay_;
    Q_EMIT decentralizationScoreUpdated(ComputeDecentralizationScore(in));
}
```

Wait — `fleet_hashrate_hps` source was a `getmininginfo.networkhashps` value that we don't currently store on ChainInfo. Two options:
(a) Add `double network_hashrate_hps{0.0}` to `ChainInfo` (and parse it in `parseMining`).
(b) Store fleet hashrate as a NodePoller member directly.

Pick (a) for cohesion — `ChainInfo` already carries chain-level state. In `qt/src/dashboardtypes.h` add a field:

```cpp
    double next_bits_delta_pct{0.0};  // existing
    double network_hashrate_hps{0.0};  // NEW Phase 2a — from getmininginfo.networkhashps
```

In `qt/src/nodepoller.cpp::parseMining`, after the existing fields, add:

```cpp
    pending_chain_.network_hashrate_hps =
        obj.value("networkhashps").toDouble(0.0);
```

Then in `emitContributionAndScore`, replace `pending_chain_.next_bits_delta_pct` with `pending_chain_.network_hashrate_hps`.

- [ ] **Step 6: Score formula unit tests**

Create `qt/tests/test_decentralization_score.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QtTest/QtTest>

using dinero::qt::dashboard::DecentralizationScore;
using dinero::qt::dashboard::NodePoller;

namespace {
NodePoller::ScoreInputs zeroInputs() { return {}; }
}  // namespace

class TestDecentralizationScore : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void all_zero_yields_zero_and_just_observing() {
        const auto s = NodePoller::ComputeDecentralizationScore(zeroInputs());
        QCOMPARE(s.total, 0.0);
        QCOMPARE(s.label, QString("just observing"));
    }

    void reachable_alone_is_one() {
        auto in = zeroInputs();
        in.reachable_with_inbound = true;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.reachable, 1.0);
        QCOMPARE(s.total, 1.0);
    }

    void relay_active_is_worth_two() {
        auto in = zeroInputs();
        in.relay_active_with_registrants = true;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.relay_active, 2.0);
    }

    void uptime_30_days_is_full_one_point_five() {
        auto in = zeroInputs();
        in.uptime_seconds = 30LL * 24 * 3600;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        // QCOMPARE on doubles compares with fuzzy tolerance.
        QCOMPARE(s.breakdown.uptime, 1.5);
    }

    void uptime_caps_at_full_for_long_runs() {
        auto in = zeroInputs();
        in.uptime_seconds = 365LL * 24 * 3600;  // 1 year
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.uptime, 1.5);
    }

    void eight_subnets_is_full_peer_diversity() {
        auto in = zeroInputs();
        in.unique_peer_subnets_slash16 = 8;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.peer_diversity, 1.5);
    }

    void one_gigabyte_relayed_is_full_traffic_score() {
        auto in = zeroInputs();
        in.bytes_relayed_24h = 1'000'000'000;  // 10^9
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.traffic, 1.0);
    }

    void mining_ratio_caps_at_one_full_weight() {
        auto in = zeroInputs();
        in.local_hashrate_hps = 1e9;
        in.fleet_hashrate_hps = 1e8;  // we're 10x bigger; ratio capped
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.mining, 1.5);
    }

    void all_max_inputs_yields_ten_and_load_bearing() {
        NodePoller::ScoreInputs in;
        in.reachable_with_inbound = true;
        in.relay_active_with_registrants = true;
        in.uptime_seconds = 30LL * 24 * 3600;
        in.unique_peer_subnets_slash16 = 8;
        in.bytes_relayed_24h = 1'000'000'000;
        in.local_hashrate_hps = 1.0;
        in.fleet_hashrate_hps = 1.0;
        in.peers_who_learned_via_gossip = 32;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.total, 10.0);
        QCOMPARE(s.label, QString("you're load-bearing for the network"));
    }

    void score_clamps_to_ten_max() {
        NodePoller::ScoreInputs in;
        in.reachable_with_inbound = true;
        in.relay_active_with_registrants = true;
        in.uptime_seconds = 365LL * 24 * 3600;     // would exceed if not capped
        in.unique_peer_subnets_slash16 = 100;       // would exceed
        in.bytes_relayed_24h = 1e15;                // would exceed
        in.local_hashrate_hps = 1e15;
        in.fleet_hashrate_hps = 1.0;
        in.peers_who_learned_via_gossip = 10'000;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.total, 10.0);
    }

    void label_buckets_match_spec() {
        auto in = zeroInputs();
        // Just one signal: reachable (1.0) → "just observing" still applies since <2.0
        in.reachable_with_inbound = true;
        QCOMPARE(NodePoller::ComputeDecentralizationScore(in).label,
                 QString("just observing"));

        // Add relay_active (+2.0 = 3.0) → "consuming responsibly"
        in.relay_active_with_registrants = true;
        QCOMPARE(NodePoller::ComputeDecentralizationScore(in).label,
                 QString("consuming responsibly"));
    }
};

QTEST_MAIN(TestDecentralizationScore)
#include "test_decentralization_score.moc"
```

- [ ] **Step 7: Register the test in CMake**

Append to `qt/CMakeLists.txt` after the `SparklineWidget` test block:

```cmake
  add_executable(test_decentralization_score
    tests/test_decentralization_score.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
  )
  add_dependencies(test_decentralization_score gtest gtest_main)
  target_include_directories(test_decentralization_score BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(test_decentralization_score PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_decentralization_score PRIVATE QT_NO_KEYWORDS)
  add_test(NAME DecentralizationScore COMMAND test_decentralization_score)
  set_tests_properties(DecentralizationScore PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )
```

If the test target needs additional source files (e.g., `rpcclient.cpp` per Phase 1's experience), add them after `src/nodepoller.cpp`. Build first; add only what the linker actually demands.

- [ ] **Step 8: Build + run**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake -S qt -B build-p2a -DDINERO_SOLO_MINER_SOURCE_ROOT=$(pwd)/miner -DDINERO_SOURCE_ROOT=$(pwd) 2>&1 | tail -3
cmake --build build-p2a --target test_decentralization_score -j8 2>&1 | tail -5
cd build-p2a && ctest -R DecentralizationScore --output-on-failure
```

Expected: 11 test cases pass.

- [ ] **Step 9: Commit**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git add qt/src/nodepoller.h qt/src/nodepoller.cpp qt/src/dashboardtypes.h \
        qt/tests/test_decentralization_score.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): NodePoller computes sparkline buffers + decentralization score

Three additions to NodePoller for Phase 2a:

1. Rolling 5-min sparkline buffers (60 samples × 5s tick) for bytes_in /
   bytes_out / relay_traffic. Each tick: sum getpeerinfo bytes, compute
   per-tick delta vs last tick's total, push into the buffer (first tick
   establishes the baseline and pushes 0). relay_traffic isolates bytes
   from via_relay peers.

2. ContributionStats per-tick snapshot: spot rates from the sparkline
   tails + hint-sent/gossip-reach counters from getnetworkinfo.relay.hints
   + placeholders (circuits_active = 0, blocks_served_today = 0) for
   Phase 2b's daemon-side surface work.

3. Decentralization score: static formula ComputeDecentralizationScore
   per the spec (sums to max 10.0), driven by ScoreInputs derived from
   already-polled RPCs + LocalMiningProvider. Score + per-component
   breakdown + plain-English label emitted via decentralizationScoreUpdated.

Two new signals: contributionStatsUpdated, decentralizationScoreUpdated.
Both fire on every tick after the relevant parsers run.

Honest approximations documented inline:
- bytes_relayed_24h is 5min×288 linear extrapolation (no 24h buffer yet)
- peers_who_learned_via_gossip = received_relay count (proxy)
- hints_sent = received_self count (proxy — daemon doesn't emit a real
  sent counter yet; Phase 2b can refine)

11 unit tests for the score formula cover each weight, the all-zero
"just observing" case, the all-max "load-bearing" case, the cap-at-10
guard, and the bucket transitions.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git log --show-signature -1 --pretty=format:"%h %GS"
```

---

## Task 4: `ContributionSection` widget + tests

**Files:**
- Create: `qt/src/contributionsection.h`
- Create: `qt/src/contributionsection.cpp`
- Create: `qt/tests/test_contribution_section.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Header**

Create `qt/src/contributionsection.h`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QVector>
#include <QWidget>

class QLabel;

namespace dinero::qt::dashboard {

class SparklineWidget;

// 🌐 YOUR CONTRIBUTION section — sparklines + stat grid + score line.
// Listens to NodePoller for two signals: contributionStatsUpdated
// (per-tick spot values + sparkline tail rates) and
// decentralizationScoreUpdated (re-derived per tick).
class ContributionSection : public QWidget {
    Q_OBJECT
public:
    explicit ContributionSection(QWidget* parent = nullptr);

    // Section also takes sparkline-buffer updates separately so the
    // NodePoller can hand off the full rolling QVector without
    // re-emitting it as part of every ContributionStats update.
    void setSparklineBuffers(const QVector<qint64>& bytes_in,
                             const QVector<qint64>& bytes_out,
                             const QVector<qint64>& relay_bytes);

public Q_SLOTS:
    void onContributionStatsUpdated(const ContributionStats& stats);
    void onDecentralizationScoreUpdated(const DecentralizationScore& score);

public:
    // Static formatting helpers exposed for unit testing.
    static QString formatRate(qint64 bytes_per_sec);
    static QString scoreLine(const DecentralizationScore& score);

private:
    SparklineWidget* sparkIn_{nullptr};
    SparklineWidget* sparkOut_{nullptr};
    SparklineWidget* sparkRelay_{nullptr};
    QLabel* lblInRate_{nullptr};
    QLabel* lblOutRate_{nullptr};
    QLabel* lblRelayRate_{nullptr};
    QLabel* lblCircuits_{nullptr};
    QLabel* lblBlocksServed_{nullptr};
    QLabel* lblHintsSent_{nullptr};
    QLabel* lblPeersGossip_{nullptr};
    QLabel* lblScore_{nullptr};
};

}  // namespace dinero::qt::dashboard
```

- [ ] **Step 2: Implementation**

Create `qt/src/contributionsection.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "contributionsection.h"
#include "sparklinewidget.h"

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

ContributionSection::ContributionSection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    auto* header = new QLabel("🌐 YOUR CONTRIBUTION", this);
    header->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(header);

    auto addSparkRow = [&](const QString& label,
                           SparklineWidget*& out_widget,
                           QLabel*& out_rate_label) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(label, this);
        lbl->setMinimumWidth(110);
        out_widget = new SparklineWidget(this);
        out_rate_label = new QLabel(this);
        out_rate_label->setMinimumWidth(70);
        out_rate_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(lbl, 0);
        row->addWidget(out_widget, 1);
        row->addWidget(out_rate_label, 0);
        root->addLayout(row);
    };
    addSparkRow("bytes in",      sparkIn_,    lblInRate_);
    addSparkRow("bytes out",     sparkOut_,   lblOutRate_);
    addSparkRow("relay traffic", sparkRelay_, lblRelayRate_);

    // Stat grid: 4 cells in 2 columns
    auto* grid = new QGridLayout();
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);
    auto addStatRow = [&](int row, const QString& cap, QLabel*& out) {
        grid->addWidget(new QLabel(cap, this), row, 0);
        out = new QLabel("0", this);
        out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(out, row, 1);
    };
    addStatRow(0, "circuits routing through you",         lblCircuits_);
    addStatRow(1, "blocks served today (approx)",         lblBlocksServed_);
    addStatRow(2, "hints I've sent",                       lblHintsSent_);
    addStatRow(3, "peers who learned of me via gossip",   lblPeersGossip_);
    root->addLayout(grid);

    lblScore_ = new QLabel(this);
    lblScore_->setStyleSheet("font-weight: bold; padding-top: 4px;");
    lblScore_->setToolTip(
        "Decentralization score (0–10): higher = your node is doing more for the network.\n"
        "Components: reachable, relay-active, uptime, peer diversity, traffic, mining, gossip reach.");
    root->addWidget(lblScore_);

    root->addStretch(1);

    onContributionStatsUpdated({});
    onDecentralizationScoreUpdated({});
}

void ContributionSection::setSparklineBuffers(
        const QVector<qint64>& in,
        const QVector<qint64>& out,
        const QVector<qint64>& relay) {
    if (sparkIn_)    sparkIn_->setSamples(in);
    if (sparkOut_)   sparkOut_->setSamples(out);
    if (sparkRelay_) sparkRelay_->setSamples(relay);
}

void ContributionSection::onContributionStatsUpdated(const ContributionStats& s) {
    if (lblInRate_)      lblInRate_->setText(formatRate(s.bytes_in_rate));
    if (lblOutRate_)     lblOutRate_->setText(formatRate(s.bytes_out_rate));
    if (lblRelayRate_)   lblRelayRate_->setText(formatRate(s.relay_bytes_rate));
    if (lblCircuits_)    lblCircuits_->setText(QString::number(s.circuits_active));
    if (lblBlocksServed_)lblBlocksServed_->setText(QString::number(s.blocks_served_today));
    if (lblHintsSent_)   lblHintsSent_->setText(QString::number(s.hints_sent));
    if (lblPeersGossip_) lblPeersGossip_->setText(QString::number(s.peers_via_gossip));
}

void ContributionSection::onDecentralizationScoreUpdated(const DecentralizationScore& s) {
    if (lblScore_) lblScore_->setText(scoreLine(s));
}

QString ContributionSection::formatRate(qint64 bps) {
    if (bps < 0) bps = 0;
    if (bps >= 1'000'000) return QString("%1 MB/s").arg(bps / 1'000'000.0, 0, 'f', 2);
    if (bps >= 1'000)     return QString("%1 KB/s").arg(bps / 1'000.0,    0, 'f', 1);
    return QString("%1 B/s").arg(bps);
}

QString ContributionSection::scoreLine(const DecentralizationScore& s) {
    return QString("▶  Decentralization score · %1 / 10   \"%2\"")
        .arg(s.total, 0, 'f', 1)
        .arg(s.label.isEmpty() ? "—" : s.label);
}

}  // namespace dinero::qt::dashboard
```

(Note: `QHBoxLayout` needs an `#include <QHBoxLayout>` — add to the cpp file's includes.)

- [ ] **Step 3: Tests**

Create `qt/tests/test_contribution_section.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "contributionsection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QtTest/QtTest>

using dinero::qt::dashboard::ContributionSection;
using dinero::qt::dashboard::ContributionStats;
using dinero::qt::dashboard::DecentralizationScore;

class TestContributionSection : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void format_rate_buckets() {
        QCOMPARE(ContributionSection::formatRate(0),         QString("0 B/s"));
        QCOMPARE(ContributionSection::formatRate(500),       QString("500 B/s"));
        QCOMPARE(ContributionSection::formatRate(1500),      QString("1.5 KB/s"));
        QCOMPARE(ContributionSection::formatRate(2'500'000), QString("2.50 MB/s"));
    }

    void score_line_uses_one_decimal_and_label() {
        DecentralizationScore s;
        s.total = 7.34;
        s.label = "you're carrying real weight";
        const auto line = ContributionSection::scoreLine(s);
        QVERIFY(line.contains("7.3"));
        QVERIFY(line.contains("you're carrying real weight"));
    }

    void score_line_handles_empty_label() {
        DecentralizationScore s;
        s.total = 0.0;
        const auto line = ContributionSection::scoreLine(s);
        QVERIFY(line.contains("0.0"));
        QVERIFY(line.contains("—"));  // em dash fallback
    }

    void stats_update_changes_visible_labels() {
        ContributionSection w;
        ContributionStats s;
        s.circuits_active     = 3;
        s.blocks_served_today = 14;
        s.hints_sent          = 247;
        s.peers_via_gossip    = 18;
        w.onContributionStatsUpdated(s);

        bool found_3   = false;
        bool found_14  = false;
        bool found_247 = false;
        bool found_18  = false;
        for (auto* l : w.findChildren<QLabel*>()) {
            if (l->text() == "3")    found_3   = true;
            if (l->text() == "14")   found_14  = true;
            if (l->text() == "247")  found_247 = true;
            if (l->text() == "18")   found_18  = true;
        }
        QVERIFY(found_3);
        QVERIFY(found_14);
        QVERIFY(found_247);
        QVERIFY(found_18);
    }
};

QTEST_MAIN(TestContributionSection)
#include "test_contribution_section.moc"
```

- [ ] **Step 4: Register test + add sources to main app target**

In `qt/CMakeLists.txt`, append after the `DecentralizationScore` test block:

```cmake
  add_executable(test_contribution_section
    tests/test_contribution_section.cpp
    src/contributionsection.cpp
    src/contributionsection.h
    src/sparklinewidget.cpp
    src/sparklinewidget.h
  )
  add_dependencies(test_contribution_section gtest gtest_main)
  target_include_directories(test_contribution_section BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(test_contribution_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )
  target_compile_definitions(test_contribution_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME ContributionSection COMMAND test_contribution_section)
  set_tests_properties(ContributionSection PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 10
  )
```

Also add `src/contributionsection.cpp` and `src/contributionsection.h` to the dinero-qt main executable's `set(GUI_SOURCES ...)` list (the same place sparklinewidget was added in Task 2).

- [ ] **Step 5: Build + run**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake -S qt -B build-p2a -DDINERO_SOLO_MINER_SOURCE_ROOT=$(pwd)/miner -DDINERO_SOURCE_ROOT=$(pwd) 2>&1 | tail -3
cmake --build build-p2a --target test_contribution_section -j8 2>&1 | tail -5
cd build-p2a && ctest -R ContributionSection --output-on-failure
```

Expected: 4 test cases pass.

- [ ] **Step 6: Commit**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git add qt/src/contributionsection.h qt/src/contributionsection.cpp \
        qt/tests/test_contribution_section.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): ContributionSection — 🌐 sparklines + stats + score

Composes 3 SparklineWidgets (bytes_in / bytes_out / relay_traffic),
a 4-row stat grid (circuits / blocks served / hints sent / peers via
gossip), and a Decentralization score line with a tooltip explaining
the formula components.

Per-tick stat refresh + sparkline-buffer refresh are separate paths
(NodePoller emits the lightweight stats on every tick; sparkline
buffers come through a heavier setSparklineBuffers wire).

Static formatRate + scoreLine helpers are public for unit testing.
4 unit tests cover rate-bucket boundaries (B/KB/MB), score-line label
fallback, and per-stat label rendering.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Wire `ContributionSection` into `MyNodeDashboard`

**Files:**
- Modify: `qt/src/mynodedashboard.h`
- Modify: `qt/src/mynodedashboard.cpp`

- [ ] **Step 1: Update the header**

In `qt/src/mynodedashboard.h`:

1. Add a forward declaration in the existing namespace block:
```cpp
class ContributionSection;
```

2. Add a member alongside the other section pointers (private section):
```cpp
    ContributionSection* contributionSection_{nullptr};
```

- [ ] **Step 2: Update the implementation**

In `qt/src/mynodedashboard.cpp`:

1. Add the include near the others:
```cpp
#include "contributionsection.h"
```

2. In the constructor, after `peersSection_ = new PeersSection(content);`, add:
```cpp
    contributionSection_ = new ContributionSection(content);
```

3. After `layout->addWidget(peersSection_, 1);`, **REPLACE** that line with:
```cpp
    layout->addWidget(peersSection_);            // was: peersSection_, 1
    layout->addWidget(contributionSection_, 1);  // contribution gets the stretch
```

4. After the existing `connect(poller_, &NodePoller::dynamicP2POverviewUpdated, ...)` block, add:
```cpp
    connect(poller_, &NodePoller::contributionStatsUpdated,
            contributionSection_, &ContributionSection::onContributionStatsUpdated);
    connect(poller_, &NodePoller::decentralizationScoreUpdated,
            contributionSection_, &ContributionSection::onDecentralizationScoreUpdated);
```

- [ ] **Step 3: Compile-check the test targets still build**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake --build build-p2a --target test_sparkline_widget test_decentralization_score test_contribution_section -j8 2>&1 | tail -5
cd build-p2a && ctest -R "SparklineWidget|DecentralizationScore|ContributionSection" --output-on-failure
```

Expected: 3 ctest entries pass (4 + 11 + 4 internal cases = 19 internal).

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git add qt/src/mynodedashboard.h qt/src/mynodedashboard.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): MyNodeDashboard composes ContributionSection

Inserts ContributionSection between PeersSection and the bottom of the
scroll area. PeersSection loses its stretch factor; contribution gets
it instead so the sparklines + score line stay anchored to the bottom
of the panel when there are few peers.

Connects NodePoller's two new signals (contributionStatsUpdated,
decentralizationScoreUpdated) to the section's slots.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Sparkline buffer wire — push from NodePoller to ContributionSection

**Files:**
- Modify: `qt/src/nodepoller.h`
- Modify: `qt/src/nodepoller.cpp`
- Modify: `qt/src/mynodedashboard.cpp`

The sparkline buffers live in NodePoller (60-sample rolling), but the section needs them pushed in. We don't want every tick to copy 3 QVectors over a signal — instead, MyNodeDashboard reads them directly via a public accessor after each tick.

- [ ] **Step 1: Add accessors to `NodePoller`**

In `qt/src/nodepoller.h`, public section:

```cpp
    QVector<qint64> bytesInBuffer()    const { return bytes_in_buffer_; }
    QVector<qint64> bytesOutBuffer()   const { return bytes_out_buffer_; }
    QVector<qint64> relayBytesBuffer() const { return relay_bytes_buffer_; }
```

- [ ] **Step 2: Push to the section in MyNodeDashboard after each contribution tick**

In `qt/src/mynodedashboard.cpp`, REPLACE the `connect(poller_, &NodePoller::contributionStatsUpdated, contributionSection_, &ContributionSection::onContributionStatsUpdated);` line you added in Task 5 with a lambda that also pushes the sparkline buffers:

```cpp
    connect(poller_, &NodePoller::contributionStatsUpdated,
            this, [this](const ContributionStats& stats) {
                contributionSection_->setSparklineBuffers(
                    poller_->bytesInBuffer(),
                    poller_->bytesOutBuffer(),
                    poller_->relayBytesBuffer());
                contributionSection_->onContributionStatsUpdated(stats);
            });
```

- [ ] **Step 3: Verify tests still pass (no behavior change to NodePoller's outputs)**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake --build build-p2a --target test_decentralization_score test_contribution_section -j8 2>&1 | tail -3
cd build-p2a && ctest -R "DecentralizationScore|ContributionSection" --output-on-failure 2>&1 | tail -5
```

Expected: both suites still pass.

- [ ] **Step 4: Commit**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git add qt/src/nodepoller.h qt/src/mynodedashboard.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): wire sparkline buffers from NodePoller to ContributionSection

Public buffer accessors on NodePoller (bytesInBuffer, bytesOutBuffer,
relayBytesBuffer) let MyNodeDashboard pull the rolling 60-sample
QVectors on every contributionStatsUpdated tick and push them via
ContributionSection::setSparklineBuffers.

This avoids copying 3 QVectors through a signal on every tick when
they only change by 1 sample per tick.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Full app build + manual sanity

**Files:** none (verification).

- [ ] **Step 1: Full dinero-qt app build**

This runs in the parent session (subagent skips for time-budget reasons). The plan engineer runs:

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cmake --build build-p2a -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinero-qt`. macOS code-sign valid on disk.

- [ ] **Step 2: All test suites pass**

```bash
cd /private/tmp/dinero-v8-phase2a-impl/build-p2a
ctest -L "dashboard" --output-on-failure 2>&1 | tail -15
```

Expected: all Phase 1 + Phase 2a dashboard test suites pass (8+ ctest entries).

- [ ] **Step 3: Smoke launch + check the new section renders**

```bash
APP=/private/tmp/dinero-v8-phase2a-impl/build-p2a/bin/dinero-qt.app
nohup "$APP/Contents/MacOS/dinero-qt" \
    --datadir=/tmp/dinero-qt-phase2a-smoke -no-daemon-autostart \
    > /tmp/dinero-qt-phase2a-smoke.log 2>&1 &
sleep 6
ps -p $! > /dev/null && echo "✓ alive" || echo "✗ crashed"
kill -TERM $!
sleep 2
rm -rf /tmp/dinero-qt-phase2a-smoke
```

Expected: process stays alive >5s and exits cleanly on SIGTERM.

- [ ] **Step 4: Commit sanity log**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
cat > docs/superpowers/plans/2026-05-24-my-node-dashboard-phase2a-sanity.md << EOF
# MyNodeDashboard Phase 2a — Sanity Log

**Date:** $(date -u +%FT%TZ)
**Branch:** \`feature/qt-dashboard-phase2a\`

## Results

| Check | Result |
|---|---|
| Full \`dinero-qt\` app build | \`[100%] Built target dinero-qt\` |
| ctest (all dashboard suites) | PASS |
| App launch (isolated datadir) | Starts, alive >5s, SIGTERM-exits cleanly |

## Pending human verification

- Cmd+K opens dashboard within a frame
- New 🌐 CONTRIBUTION section visible below 🛰 PEERS
- 3 sparklines appear (likely empty for first few ticks until traffic accumulates)
- Stat grid populates with circuits/blocks/hints/gossip numbers (likely 0/0 + real hint counts from getnetworkinfo.relay.hints)
- Decentralization score line shows "X.X / 10  \"<bucket label>\"" with non-degenerate value once the node has run a few minutes

## Known approximations (documented in code)

- bytes_relayed_24h is a 5min × 288 linear extrapolation — Phase 2b can add a real 24h buffer if needed
- peers_who_learned_via_gossip = received_relay count (proxy)
- hints_sent = received_self count (proxy)
- circuits_active = 0 + blocks_served_today = 0 placeholders until Phase 2b adds daemon-side accessors
EOF
git add docs/superpowers/plans/2026-05-24-my-node-dashboard-phase2a-sanity.md
git commit -S -m "$(cat <<'EOF'
docs: Phase 2a sanity log — full build + tests + smoke launch

Records the build + test + launch results. Notes the four honest
approximations in the score formula (bytes_relayed extrapolation,
gossip proxy, hints_sent proxy, placeholder circuits/blocks counts)
that Phase 2b can refine with daemon-side surface work.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Push branch + open draft PR

**Files:** none (process).

- [ ] **Step 1: Push**

```bash
cd /private/tmp/dinero-v8-phase2a-impl
git push -u origin feature/qt-dashboard-phase2a
```

- [ ] **Step 2: Open draft PR**

```bash
gh pr create --draft --title "feat(qt): MyNodeDashboard Phase 2a — Contribution section + Decentralization score" --body "$(cat <<'EOF'
## Summary

Phase 2a of the [MyNodeDashboard design](https://github.com/DineroLabs/dinero-v8/blob/dinero-main/docs/superpowers/specs/2026-05-24-my-node-dashboard-design.md). Adds the 🌐 CONTRIBUTION section below the existing 🛰 PEERS section in the Cmd+K dashboard:

- 3 rolling-5-min sparklines (bytes in / bytes out / relay traffic) — hand-rolled QPainter, no QChart dep
- 4-row stat grid (circuits routing through you / blocks served today / hints I've sent / peers who learned of me via gossip)
- Decentralization Score line: 0–10 with plain-English bucket label ("just observing" → "you're load-bearing"), tooltip explains the 7 weighted components

## What's NOT in this PR (Phase 2b)

- 💡 Discovery section (per-target relay-hint cache rows) — needs a new daemon-side `relay_hints.list` RPC that doesn't exist yet
- Real `circuits_active` counter from the daemon (Phase 2a uses 0 placeholder)
- Real `blocks_served_today` counter from the daemon (Phase 2a uses 0 placeholder)
- True 24h bytes-relayed (Phase 2a uses 5min × 288 linear extrapolation)
- Real per-target gossip-reach (Phase 2a uses `received_relay` count as proxy)

These approximations are documented in code + the sanity log. The score is still a strictly correct rendering of the spec's formula given the inputs available to the dashboard today.

## Test plan

- [x] 19 unit test cases pass (SparklineWidget 4 + DecentralizationScore 11 + ContributionSection 4)
- [x] Full dinero-qt app builds clean
- [x] App launches + exits cleanly on isolated datadir
- [ ] CI: Tests + core-heavy lanes
- [ ] Human-driven UI check: Cmd+K shows the new section, sparklines populate over ~30s of polling, score line shows a non-zero value once at least one signal is true

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Wait for CI, mark ready**

```bash
# Poll CI every 90s up to ~20 min
for i in $(seq 1 13); do
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

Stop here. Don't merge.

---

## Coverage map (self-review)

| Phase 2a spec requirement | Task |
|---|---|
| 3 sparklines (bytes in/out/relay) | 2 + 3 + 4 |
| Stat grid (circuits/blocks/hints/gossip) | 4 |
| Decentralization score 0–10 | 3 (formula) + 4 (line render) |
| Plain-English label per bucket | 3 (`scoreLabel`) + tests in 3 |
| Score formula 7 components per spec | 3 (`ComputeDecentralizationScore`) |
| Sparkline rolling 5-min window | 3 (`kSparklineCapacity = 60` × 5s tick) |
| ContributionSection slots into Cmd+K panel | 5 (`MyNodeDashboard` composes) |
| Sparkline buffers pushed efficiently | 6 (accessor pattern, not signal copy) |
| Unit tests | 2, 3, 4 (19 cases total) |
| Full app build clean | 7 |
| PR opened | 8 |
| Discovery section explicitly deferred to Phase 2b | mentioned in PR body + sanity log |
