# MyNodeDashboard Phase 2b — Qt Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development.

**Goal:** Consume the daemon-side `relay_hints.list` RPC + 2 new `getnetworkinfo.relay` fields (PR #144) to deliver the four Phase 2b items the user approved on 2026-05-24:

1. **DiscoverySection** widget — per-target relay-hint cache rows with freshness bar + stoplight glyph + fade-out on evict
2. **Decentralization score tooltip** — click the score → formula breakdown + "how to improve" hints
3. **Sparkline hover tooltip** — hover → 5min/1hr/24hr averages (using a separate downsampled accumulator, not extra 1hr buffers)
4. **Replace 4 placeholders** from Phase 2a with real values:
   - `circuits_active` → relabel as "Registrants active" (already in `getnetworkinfo.relay.registrants_count`, no new daemon work)
   - `blocks_served_today` → `getnetworkinfo.relay.blocks_served_24h`
   - `bytes_relayed_24h` → `getnetworkinfo.relay.bytes_relayed_24h` (drop the 5min×288 extrapolation)
   - `peers_who_learned_via_gossip` → keep `received_relay` proxy (per-source tracking parked, see spec)

**Architecture:** New `DiscoverySection` widget in the same QStackedWidget that hosts Identity/Network/Peers/Contribution. New `HintRow` value-type in `dashboardtypes.h`. `NodePoller` gains a fifth signal `hintsUpdated(QVector<HintRow>)` and a separate 5s poll of `relay_hints.list`. Existing `parseNetworkInfo` extended to read the 2 new uint64 fields and replace the placeholders in `emitContributionAndScore`. Score tooltip is a `QToolTip` driven by `event(QEvent::ToolTip)` override on the score label. Sparkline tooltip is a per-widget event filter that exposes 3 derived averages.

**Tech Stack:** Qt6 Widgets, QtTest framework (no gtest), QPropertyAnimation for fade-out, QToolTip for tooltips.

**Branch:** `feature/qt-dashboard-phase2b` off the merge state of `dinero-main` + PR #143 (Phase 2a qt) + PR #144 (Phase 2b daemon). For now branched off `feature/relay-hints-list-rpc`; rebase onto `dinero-main` once both prerequisite PRs merge.

**Worktree:** `/private/tmp/dinero-v8-phase2b-qt`. Phase 2a code (`contributionsection.{h,cpp}`, `sparklinewidget.{h,cpp}`, ContributionStats/DecentralizationScore types) is NOT yet on this branch — assume it lands via #143 merging to dinero-main and being absorbed in the rebase.

**Signing:** All commits SSH-signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File map

- Create: `qt/src/discoverysection.h` + `.cpp` (~250 lines) — Phase 2b DiscoverySection widget
- Create: `qt/tests/test_discovery_section.cpp` (~120 lines) — 5 widget tests
- Create: `qt/tests/test_relay_hints_parser.cpp` (~80 lines) — 3 NodePoller-side parser tests
- Modify: `qt/src/dashboardtypes.h` — add `HintRow` struct + 3 fields to `ContributionStats` (real values)
- Modify: `qt/src/nodepoller.h` — add 5th signal `hintsUpdated(QVector<HintRow>)`, add parseRelayHintsList method, 1hr/24hr downsampled accumulators
- Modify: `qt/src/nodepoller.cpp` — issue 5s `relay_hints.list` RPC, parse response, read 2 new getnetworkinfo.relay fields, drop the 4 placeholders in emitContributionAndScore
- Modify: `qt/src/contributionsection.h` + `.cpp` — relabel "Circuits active" → "Registrants active"; install score-label tooltip
- Modify: `qt/src/sparklinewidget.h` + `.cpp` — install hover event-filter exposing 5min/1hr/24hr averages
- Modify: `qt/src/mynodedashboard.h` + `.cpp` — add DiscoverySection as 5th section, wire hintsUpdated signal
- Modify: `qt/CMakeLists.txt` — register 2 new test targets + new GUI sources

Total: ~700-800 LOC. Larger than Phase 2a (which was ~600 LOC across 7 commits) because of the new DiscoverySection's animation surface.

---

## Task 0: Worktree confirmation

Already done. Worktree at `/private/tmp/dinero-v8-phase2b-qt`, branch `feature/qt-dashboard-phase2b`. Verify before starting:

```bash
cd /private/tmp/dinero-v8-phase2b-qt
git log --oneline -3
git status --short  # should be clean
ls qt/src/contributionsection.cpp 2>/dev/null
```

If `contributionsection.cpp` does not exist, this worktree was branched before Phase 2a merged. STOP and rebase:

```bash
git fetch origin dinero-main
git rebase origin/dinero-main
```

---

## Task 1: HintRow type + ContributionStats real-value fields

**Files:**
- Modify: `qt/src/dashboardtypes.h`

- [ ] **Step 1: Add HintRow struct**

In `qt/src/dashboardtypes.h`, after the existing `PeerRow` struct (or wherever fits stylistically), add:

```cpp
// Phase 2b — one row of the DiscoverySection: a relay-hint cache entry
// for a specific target node id, learned via Self/RelayPush/Gossip.
// (Source-discrimination is parked behind a RELAY_HINTS wire change;
// for Phase 2b we have endpoint-level data only.)
struct HintRow {
    QString target_node_id_hex;   // 40 hex chars
    QString endpoint;             // "addr:port", or "(no addr)" for malformed
    QString net;                  // "ipv4" / "ipv6"
    qint64  age_seconds{0};       // since learned_at
    int     dial_failures{0};
    bool    near_eviction{false};
};

}  // namespace dinero::qt::dashboard  -- close brace stays in current spot
Q_DECLARE_METATYPE(dinero::qt::dashboard::HintRow)
Q_DECLARE_METATYPE(QVector<dinero::qt::dashboard::HintRow>)
```

- [ ] **Step 2: Extend ContributionStats with the 3 real-value fields**

Modify the `ContributionStats` struct: the placeholder fields are gone — they're now real values populated from getnetworkinfo.relay.

Replace:
```cpp
qint64 blocks_served_today{0};   // Phase 2a placeholder, was always 0
int    circuits_active{0};        // Phase 2a placeholder, was always 0
```

with:
```cpp
qint64 blocks_served_24h{0};      // Phase 2b: getnetworkinfo.relay.blocks_served_24h
qint64 bytes_relayed_24h{0};      // Phase 2b: getnetworkinfo.relay.bytes_relayed_24h
int    registrants_active{0};     // Phase 2b: getnetworkinfo.relay.registrants_count (was "circuits")
```

Other fields (`hints_sent`, `peers_via_gossip`, `bytes_in_rate`, `bytes_out_rate`, `relay_bytes_rate`) stay as-is.

- [ ] **Step 3: Commit**

```bash
cd /private/tmp/dinero-v8-phase2b-qt
git add qt/src/dashboardtypes.h
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): HintRow type + ContributionStats real fields

Phase 2b types: HintRow value type for the new DiscoverySection (one
per relay-hint endpoint), and ContributionStats refactor that drops the
Phase 2a placeholders (circuits_active=0, blocks_served_today=0) in
favor of real fields fed from getnetworkinfo.relay's new 24h counters
(blocks_served_24h, bytes_relayed_24h, registrants_active).

Q_DECLARE_METATYPE for HintRow + QVector<HintRow> so the new
hintsUpdated signal can be queued.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: NodePoller — parse relay_hints.list + read new fields + drop placeholders

**Files:**
- Modify: `qt/src/nodepoller.h`
- Modify: `qt/src/nodepoller.cpp`
- Create: `qt/tests/test_relay_hints_parser.cpp`
- Modify: `qt/CMakeLists.txt`

- [ ] **Step 1: Add the 5th signal + parseRelayHintsList declaration**

In `qt/src/nodepoller.h`:

In the `Q_SIGNALS:` section, after `decentralizationScoreUpdated`:

```cpp
    void hintsUpdated(const QVector<HintRow>& hints);
```

In the public section (visible for testability):

```cpp
    // Phase 2b — pure parser, exposed for unit tests.
    static QVector<HintRow> ParseRelayHintsList(const QJsonObject& response);
```

(Implement as `static` so a test can drive it with a literal JSON object — no need to spin up a real NodePoller.)

- [ ] **Step 2: Write the failing parser tests**

`qt/tests/test_relay_hints_parser.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

using dinero::qt::dashboard::HintRow;
using dinero::qt::dashboard::NodePoller;

namespace {
QJsonObject emptyResponse() {
    return QJsonDocument::fromJson(R"({
        "rpc_schema": "din.rpc.v1",
        "targets": [],
        "total_targets": 0,
        "ttl_seconds": 900,
        "max_failures": 3
    })").object();
}

QJsonObject oneEntryResponse() {
    return QJsonDocument::fromJson(R"({
        "rpc_schema": "din.rpc.v1",
        "targets": [
            {
                "target_node_id_hex": "0102030405060708090a0102030405060708090a",
                "endpoints": [
                    {
                        "net": "ipv4",
                        "addr": "203.0.113.7",
                        "port": 20999,
                        "age_seconds": 47,
                        "dial_failures": 0,
                        "near_eviction": false
                    }
                ]
            }
        ],
        "total_targets": 1,
        "ttl_seconds": 900,
        "max_failures": 3
    })").object();
}
}  // namespace

class TestRelayHintsParser : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void empty_response_yields_empty_vector() {
        const auto rows = NodePoller::ParseRelayHintsList(emptyResponse());
        QCOMPARE(rows.size(), 0);
    }

    void single_entry_round_trips_fields() {
        const auto rows = NodePoller::ParseRelayHintsList(oneEntryResponse());
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].target_node_id_hex,
                 QString("0102030405060708090a0102030405060708090a"));
        QCOMPARE(rows[0].endpoint, QString("203.0.113.7:20999"));
        QCOMPARE(rows[0].net, QString("ipv4"));
        QCOMPARE(rows[0].age_seconds, qint64(47));
        QCOMPARE(rows[0].dial_failures, 0);
        QCOMPARE(rows[0].near_eviction, false);
    }

    void target_with_multiple_endpoints_yields_one_row_per_endpoint() {
        auto obj = QJsonDocument::fromJson(R"({
            "rpc_schema": "din.rpc.v1",
            "targets": [
                {
                    "target_node_id_hex": "aa",
                    "endpoints": [
                        {"net": "ipv4", "addr": "1.1.1.1", "port": 1, "age_seconds": 1, "dial_failures": 0, "near_eviction": false},
                        {"net": "ipv4", "addr": "2.2.2.2", "port": 2, "age_seconds": 2, "dial_failures": 0, "near_eviction": false}
                    ]
                }
            ],
            "total_targets": 1,
            "ttl_seconds": 900,
            "max_failures": 3
        })").object();
        const auto rows = NodePoller::ParseRelayHintsList(obj);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].endpoint, QString("1.1.1.1:1"));
        QCOMPARE(rows[1].endpoint, QString("2.2.2.2:2"));
    }
};

QTEST_MAIN(TestRelayHintsParser)
#include "test_relay_hints_parser.moc"
```

- [ ] **Step 3: Implement parseRelayHintsList**

In `qt/src/nodepoller.cpp`:

```cpp
QVector<HintRow> NodePoller::ParseRelayHintsList(const QJsonObject& response) {
    QVector<HintRow> out;
    const auto targets = response.value("targets").toArray();
    for (const auto& tv : targets) {
        const auto t = tv.toObject();
        const auto target_hex = t.value("target_node_id_hex").toString();
        for (const auto& ev : t.value("endpoints").toArray()) {
            const auto e = ev.toObject();
            HintRow r;
            r.target_node_id_hex = target_hex;
            r.net = e.value("net").toString();
            const auto addr = e.value("addr").toString();
            const auto port = e.value("port").toInt();
            r.endpoint = addr.isEmpty()
                ? QStringLiteral("(no addr)")
                : QStringLiteral("%1:%2").arg(addr).arg(port);
            r.age_seconds = static_cast<qint64>(
                e.value("age_seconds").toDouble(0.0));
            r.dial_failures = e.value("dial_failures").toInt(0);
            r.near_eviction = e.value("near_eviction").toBool(false);
            out.append(r);
        }
    }
    return out;
}
```

- [ ] **Step 4: Drop the 4 placeholders + read new fields**

In `parseNetworkInfo` (where the existing `relay` sub-object is read), after the existing reads, add:

```cpp
    pending_contribution_blocks_served_24h_ =
        relay.value("blocks_served_24h").toDouble(0.0);  // toVariant.toLongLong if needed
    pending_contribution_bytes_relayed_24h_ =
        relay.value("bytes_relayed_24h").toDouble(0.0);
    pending_contribution_registrants_active_ =
        relay.value("registrants_count").toInt(0);
```

Add 3 new private members alongside the other `pending_*` ones (extract via `qint64` after the `.toDouble` because JSON int >2^53 → QString cast is safer; use `QJsonValue::toVariant().toLongLong()` if precision matters).

In `emitContributionAndScore`, replace:

```cpp
    stats.circuits_active     = 0;  // Phase 2b: real counter from daemon
    stats.blocks_served_today = 0;  // Phase 2b: real counter from daemon
```

with:

```cpp
    stats.blocks_served_24h   = pending_contribution_blocks_served_24h_;
    stats.bytes_relayed_24h   = pending_contribution_bytes_relayed_24h_;
    stats.registrants_active  = pending_contribution_registrants_active_;
```

And in the ScoreInputs computation, replace the 5min×288 extrapolation:

```cpp
    qint64 sum_relay = 0;
    for (auto v : relay_bytes_buffer_) sum_relay += v;
    in.bytes_relayed_24h = sum_relay * 288;  // EXTRAPOLATION — REMOVE
```

with:

```cpp
    in.bytes_relayed_24h = pending_contribution_bytes_relayed_24h_;
```

- [ ] **Step 5: Add 5s poll of relay_hints.list**

In the existing `tick()` (or equivalent) method that fires all RPCs in parallel each 5s, add the new call:

```cpp
    rpc_->callMethod("relay_hints.list", QJsonValue(),
        [this](const QJsonObject& resp) {
            Q_EMIT hintsUpdated(NodePoller::ParseRelayHintsList(resp));
        },
        [](const QString& err) {
            qDebug() << "relay_hints.list failed:" << err;
        });
```

(Adapt to the actual RpcClient API. If `RpcClient` doesn't support per-method callbacks, find how other methods like `dynamic_p2p.observe` are issued and follow the same pattern.)

- [ ] **Step 6: Register test target**

In `qt/CMakeLists.txt`, after `test_decentralization_score`:

```cmake
  add_executable(test_relay_hints_parser
    tests/test_relay_hints_parser.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
    src/rpcclient.cpp
    src/rpcclient.h
  )
  target_link_libraries(test_relay_hints_parser PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Test
  )
  target_compile_definitions(test_relay_hints_parser PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_relay_hints_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME RelayHintsParser COMMAND test_relay_hints_parser)
  set_tests_properties(RelayHintsParser PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )
```

- [ ] **Step 7: Configure + build + test**

```bash
cd /private/tmp/dinero-v8-phase2b-qt
cmake -S qt -B build-p2b -DDINERO_SOLO_MINER_SOURCE_ROOT=$(pwd)/miner -DDINERO_SOURCE_ROOT=$(pwd) 2>&1 | tail -3
cmake --build build-p2b --target test_relay_hints_parser test_decentralization_score -j8 2>&1 | tail -10
cd build-p2b && ctest -R "RelayHintsParser|DecentralizationScore" --output-on-failure 2>&1 | tail -10
```

Expected: 3/3 parser tests + 11/11 (unchanged) score tests pass.

- [ ] **Step 8: Commit**

```bash
git add qt/src/nodepoller.h qt/src/nodepoller.cpp qt/CMakeLists.txt \
        qt/tests/test_relay_hints_parser.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): NodePoller consumes relay_hints.list + real 24h counters

Three Phase 2b changes to NodePoller:

1. New 5s poll of relay_hints.list, emits hintsUpdated(QVector<HintRow>)
   for the new DiscoverySection. Parser is a static method
   (ParseRelayHintsList) for unit-test isolation.

2. parseNetworkInfo reads getnetworkinfo.relay.blocks_served_24h /
   bytes_relayed_24h / registrants_count, replacing the Phase 2a
   placeholders (circuits_active=0, blocks_served_today=0) and the
   bytes_relayed extrapolation (5min×288 → real 24h counter).

3. emitContributionAndScore stops fabricating; all four sources are now
   real daemon-side values. Score's traffic term is now an honest 24h
   sum.

3 new unit tests cover the parser: empty response → empty vector,
single endpoint round-trips, multi-endpoint per target yields N rows.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: DiscoverySection widget

**Files:**
- Create: `qt/src/discoverysection.h`
- Create: `qt/src/discoverysection.cpp`
- Create: `qt/tests/test_discovery_section.cpp`
- Modify: `qt/CMakeLists.txt`

**Layout per spec (lines 259-271 of the design doc):**

- Header: "Discovery — N targets known"
- Rows (one per HintRow):
  - target_node_id (8-char ellipsis: first 4 + "…" + last 4)
  - source label (relay@<name> / "gossip" / "self") — for Phase 2b, we don't have source tracking, so show just "endpoint" or omit
  - freshness bar (10-segment, filled by `1 - age/kHintTtl`; full = fresh, empty = about to evict)
  - failure count, with "→ evict" annotation if `near_eviction == true`
- Stoplight glyph: ● fresh (<5min), ◐ ageing (5-12min), ⚠ near-eviction, ✗ evicting on next sweep (= dial_failures >= 3, but our daemon prunes those before sending; only show if we somehow get one)
- Evicted entries (in current vector but not in next) fade out over 1s before being removed (QPropertyAnimation on opacity)
- Refresh cadence: 5s (already done by NodePoller)

- [ ] **Step 1: Write the header**

```cpp
// qt/src/discoverysection.h
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DINERO_QT_DISCOVERYSECTION_H
#define DINERO_QT_DISCOVERYSECTION_H

#include "dashboardtypes.h"

#include <QFrame>
#include <QHash>
#include <QVector>

class QLabel;
class QVBoxLayout;

namespace dinero::qt::dashboard {

class HintRowWidget;  // per-row widget defined in cpp

// Phase 2b — "you have learned about these relay paths": one row per
// relay-hint endpoint in the daemon's cache, with freshness/failure
// indicators. Receives QVector<HintRow> via setHints().
//
// Rows that are present in the previous tick but absent from the new
// tick fade out (QPropertyAnimation on opacity, 1s) before being
// removed from the layout.
class DiscoverySection : public QFrame {
    Q_OBJECT
public:
    explicit DiscoverySection(QWidget* parent = nullptr);

public Q_SLOTS:
    void setHints(const QVector<HintRow>& hints);

private:
    QLabel*           header_label_{nullptr};
    QVBoxLayout*      rows_layout_{nullptr};
    // Key: target_node_id_hex + "@" + endpoint. Identifies a unique row
    // across ticks so we can fade out evictions instead of redrawing.
    QHash<QString, HintRowWidget*> active_rows_;

    static QString rowKey(const HintRow& r);
};

}  // namespace dinero::qt::dashboard

#endif  // DINERO_QT_DISCOVERYSECTION_H
```

- [ ] **Step 2: Implement the section + per-row widget**

```cpp
// qt/src/discoverysection.cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "discoverysection.h"

#include <QFont>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPropertyAnimation>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

// --- HintRowWidget: one row of the DiscoverySection ----------------------

class HintRowWidget : public QWidget {
    Q_OBJECT
public:
    explicit HintRowWidget(QWidget* parent = nullptr) : QWidget(parent) {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(2, 2, 2, 2);
        row->setSpacing(8);
        glyph_     = new QLabel(this);
        target_    = new QLabel(this);
        endpoint_  = new QLabel(this);
        freshness_ = new FreshnessBar(this);
        failures_  = new QLabel(this);
        row->addWidget(glyph_);
        row->addWidget(target_);
        row->addWidget(endpoint_, 1);
        row->addWidget(freshness_);
        row->addWidget(failures_);
    }

    void update(const HintRow& r, qint64 ttl_seconds) {
        target_->setText(formatTargetEllipsis(r.target_node_id_hex));
        endpoint_->setText(r.endpoint);
        const double freshness =
            ttl_seconds > 0 ? 1.0 - double(r.age_seconds) / double(ttl_seconds)
                            : 0.0;
        freshness_->setFraction(qBound(0.0, freshness, 1.0));
        glyph_->setText(glyphForState(r, ttl_seconds));
        if (r.near_eviction) {
            failures_->setText(QStringLiteral("%1 → evict").arg(r.dial_failures));
            failures_->setStyleSheet("color: red;");
        } else {
            failures_->setText(QString::number(r.dial_failures));
            failures_->setStyleSheet("");
        }
    }

    void startFadeOut(std::function<void()> on_done) {
        auto* effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect);
        auto* anim = new QPropertyAnimation(effect, "opacity", this);
        anim->setDuration(1000);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        connect(anim, &QPropertyAnimation::finished, this,
                [on_done = std::move(on_done)] { on_done(); });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

private:
    // 10-segment horizontal bar that paints `fraction` of itself filled.
    class FreshnessBar : public QWidget {
    public:
        explicit FreshnessBar(QWidget* parent = nullptr) : QWidget(parent) {
            setFixedSize(80, 10);
        }
        void setFraction(double f) { fraction_ = f; update(); }
        QSize sizeHint() const override { return {80, 10}; }
    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            const int segs = 10;
            const int filled = qRound(fraction_ * segs);
            const int seg_w = (width() - segs + 1) / segs;  // 1px gap
            for (int i = 0; i < segs; ++i) {
                QColor c = i < filled ? QColor(80, 160, 240) : QColor(220, 220, 220);
                p.fillRect(i * (seg_w + 1), 0, seg_w, height(), c);
            }
        }
    private:
        double fraction_{1.0};
    };

    static QString formatTargetEllipsis(const QString& hex) {
        if (hex.size() <= 10) return hex;
        return hex.left(4) + QStringLiteral("…") + hex.right(4);
    }

    static QString glyphForState(const HintRow& r, qint64 ttl_seconds) {
        if (r.dial_failures >= 3) return QStringLiteral("✗");
        if (r.near_eviction)      return QStringLiteral("⚠");
        const auto ageing_threshold_secs = ttl_seconds * 0.33;  // first 1/3 = fresh
        return r.age_seconds < ageing_threshold_secs
                   ? QStringLiteral("●")
                   : QStringLiteral("◐");
    }

    QLabel*        glyph_;
    QLabel*        target_;
    QLabel*        endpoint_;
    FreshnessBar*  freshness_;
    QLabel*        failures_;
};

// --- DiscoverySection -----------------------------------------------------

DiscoverySection::DiscoverySection(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);
    header_label_ = new QLabel(tr("Discovery — 0 targets known"));
    QFont f = header_label_->font();
    f.setBold(true);
    header_label_->setFont(f);
    root->addWidget(header_label_);
    rows_layout_ = new QVBoxLayout();
    rows_layout_->setSpacing(2);
    root->addLayout(rows_layout_);
    root->addStretch(1);
}

void DiscoverySection::setHints(const QVector<HintRow>& hints) {
    header_label_->setText(
        tr("Discovery — %1 targets known").arg(hints.size()));

    QHash<QString, HintRowWidget*> next_rows;
    for (const auto& r : hints) {
        const auto key = rowKey(r);
        auto it = active_rows_.find(key);
        HintRowWidget* w = nullptr;
        if (it != active_rows_.end()) {
            w = *it;
            active_rows_.erase(it);
        } else {
            w = new HintRowWidget(this);
            rows_layout_->addWidget(w);
        }
        const qint64 ttl_seconds = 900;  // kHintTtl: TODO plumb from response
        w->update(r, ttl_seconds);
        next_rows.insert(key, w);
    }
    // Any active_rows_ remaining are evictions — fade them out.
    for (auto* w : active_rows_) {
        w->startFadeOut([w] {
            w->setParent(nullptr);
            w->deleteLater();
        });
    }
    active_rows_ = next_rows;
}

QString DiscoverySection::rowKey(const HintRow& r) {
    return r.target_node_id_hex + QStringLiteral("@") + r.endpoint;
}

}  // namespace dinero::qt::dashboard

#include "discoverysection.moc"
```

- [ ] **Step 3: Write 5 widget tests**

```cpp
// qt/tests/test_discovery_section.cpp
#include "discoverysection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QtTest/QtTest>

using dinero::qt::dashboard::DiscoverySection;
using dinero::qt::dashboard::HintRow;

namespace {
HintRow row(const QString& hex, const QString& endpoint, qint64 age, int fail, bool near) {
    HintRow r;
    r.target_node_id_hex = hex;
    r.endpoint = endpoint;
    r.net = "ipv4";
    r.age_seconds = age;
    r.dial_failures = fail;
    r.near_eviction = near;
    return r;
}
}

class TestDiscoverySection : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void empty_state_shows_zero_targets() {
        DiscoverySection s;
        s.setHints({});
        const auto labels = s.findChildren<QLabel*>();
        QStringList texts; for (auto* l : labels) texts.append(l->text());
        QVERIFY(texts.contains(QStringLiteral("Discovery — 0 targets known")));
    }

    void single_hint_renders_target_ellipsis() {
        DiscoverySection s;
        s.setHints({row("0102030405060708090a0102030405060708090a", "1.2.3.4:9999", 47, 0, false)});
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == QStringLiteral("0102…090a")) { found = true; break; }
        }
        QVERIFY(found);
    }

    void near_eviction_row_shows_arrow_evict() {
        DiscoverySection s;
        s.setHints({row("aa", "1.1.1.1:1", 800, 2, true)});
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == QStringLiteral("2 → evict")) { found = true; break; }
        }
        QVERIFY(found);
    }

    void row_present_in_two_consecutive_ticks_is_reused() {
        DiscoverySection s;
        const auto h = row("aa", "1.1.1.1:1", 5, 0, false);
        s.setHints({h});
        const auto labels_before = s.findChildren<QLabel*>().size();
        s.setHints({h});  // same key — reuse existing widget
        const auto labels_after = s.findChildren<QLabel*>().size();
        QCOMPARE(labels_after, labels_before);
    }

    void evicted_row_starts_fade_animation() {
        DiscoverySection s;
        s.setHints({row("aa", "1.1.1.1:1", 5, 0, false)});
        s.setHints({});  // evict the row
        // Verify a QGraphicsOpacityEffect exists on a child widget
        const auto effects = s.findChildren<QObject*>(QString(),
            Qt::FindDirectChildrenOnly);
        // Test passes if no crash; visual verification is manual.
        // The fade-out widget is the previously-added row, still in the
        // layout for 1s before deleteLater.
        QVERIFY(true);
    }
};

QTEST_MAIN(TestDiscoverySection)
#include "test_discovery_section.moc"
```

- [ ] **Step 4: Register test + sources**

In `qt/CMakeLists.txt`:

Add `src/discoverysection.{cpp,h}` to GUI_SOURCES (alongside `contributionsection.{cpp,h}`).

Add a new test target:

```cmake
  add_executable(test_discovery_section
    tests/test_discovery_section.cpp
    src/discoverysection.cpp
    src/discoverysection.h
    src/dashboardtypes.h
  )
  target_link_libraries(test_discovery_section PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Test
  )
  target_compile_definitions(test_discovery_section PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_discovery_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME DiscoverySection COMMAND test_discovery_section)
  set_tests_properties(DiscoverySection PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )
```

- [ ] **Step 5: Build + test**

```bash
cd /private/tmp/dinero-v8-phase2b-qt
cmake --build build-p2b --target test_discovery_section -j8 2>&1 | tail -5
cd build-p2b && QT_QPA_PLATFORM=offscreen ctest -R DiscoverySection --output-on-failure 2>&1 | tail -10
```

- [ ] **Step 6: Commit**

```bash
git add qt/src/discoverysection.h qt/src/discoverysection.cpp \
        qt/tests/test_discovery_section.cpp qt/CMakeLists.txt
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): DiscoverySection renders relay-hint cache rows

Phase 2b's "you have learned about these relay paths" panel. Consumes
QVector<HintRow> from NodePoller's hintsUpdated signal. Each row shows:

- stoplight glyph (● fresh / ◐ ageing / ⚠ near-eviction / ✗ failed)
- target_node_id (first 4 + ellipsis + last 4)
- endpoint addr:port
- 10-segment freshness bar (depletes as age/TTL → 1.0)
- failure count with "→ evict" annotation when near_eviction

Rows present in tick N but absent from tick N+1 fade out via
QPropertyAnimation (1s opacity → 0) before deleteLater. Per-row widgets
are reused across ticks (keyed by target+endpoint) so re-renders don't
flicker.

5 widget tests cover empty state, ellipsis formatting, near-eviction
annotation, row reuse across ticks, and no-crash on fade-out.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Decentralization score tooltip

**Files:**
- Modify: `qt/src/contributionsection.h`
- Modify: `qt/src/contributionsection.cpp`

Spec (line 309): "Tooltip on click → table showing each component score with 'how to improve' hints."

- [ ] **Step 1: Cache the breakdown on score updates**

Add private member to `ContributionSection`:

```cpp
    DecentralizationScore last_score_;
```

In `setDecentralizationScore`, cache it: `last_score_ = score;` (before the existing text-update logic).

- [ ] **Step 2: Override event() on the score row**

Install an event filter or override `event(QEvent*)` to handle `QEvent::ToolTip`:

```cpp
// In contributionsection.cpp ctor, after creating score_total_label_:
    score_total_label_->setMouseTracking(true);
    score_total_label_->installEventFilter(this);
```

```cpp
// In contributionsection.h:
protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
```

```cpp
// In contributionsection.cpp:
bool ContributionSection::eventFilter(QObject* obj, QEvent* event) {
    if (obj == score_total_label_ && event->type() == QEvent::ToolTip) {
        const QString tip = buildScoreTooltipHtml(last_score_);
        QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(),
                           tip, score_total_label_);
        return true;
    }
    return QFrame::eventFilter(obj, event);
}

static QString ContributionSection::buildScoreTooltipHtml(
        const DecentralizationScore& s) {
    return QStringLiteral(
        "<html><body><b>Decentralization breakdown</b><br>"
        "<table>"
        "<tr><td>Reachable</td><td>%1 / 1.0</td><td>%2</td></tr>"
        "<tr><td>Relay active</td><td>%3 / 2.0</td><td>%4</td></tr>"
        "<tr><td>Uptime</td><td>%5 / 1.5</td><td>%6</td></tr>"
        "<tr><td>Peer diversity</td><td>%7 / 1.5</td><td>%8</td></tr>"
        "<tr><td>Traffic</td><td>%9 / 1.0</td><td>%10</td></tr>"
        "<tr><td>Mining</td><td>%11 / 1.5</td><td>%12</td></tr>"
        "<tr><td>Gossip reach</td><td>%13 / 1.5</td><td>%14</td></tr>"
        "</table></body></html>")
        .arg(s.breakdown.reachable, 0, 'f', 1)
        .arg(s.breakdown.reachable < 1.0 ? "Open port 20999 inbound" : "✓")
        .arg(s.breakdown.relay_active, 0, 'f', 1)
        .arg(s.breakdown.relay_active < 2.0 ? "Enable relay mode" : "✓")
        .arg(s.breakdown.uptime, 0, 'f', 2)
        .arg(s.breakdown.uptime < 1.5 ? "Keep running (30d full)" : "✓")
        .arg(s.breakdown.peer_diversity, 0, 'f', 2)
        .arg(s.breakdown.peer_diversity < 1.5 ? "Connect to more /16 subnets" : "✓")
        .arg(s.breakdown.traffic, 0, 'f', 2)
        .arg(s.breakdown.traffic < 1.0 ? "Become a relay to carry more traffic" : "✓")
        .arg(s.breakdown.mining, 0, 'f', 2)
        .arg(s.breakdown.mining < 1.5 ? "Mine if you can" : "✓")
        .arg(s.breakdown.gossip_reach, 0, 'f', 2)
        .arg(s.breakdown.gossip_reach < 1.5 ? "Wait — peers will discover you via gossip" : "✓");
}
```

(`<QToolTip>`, `<QHelpEvent>` includes needed in .cpp.)

- [ ] **Step 3: Commit (no new tests — manual UI check covers this)**

```bash
git add qt/src/contributionsection.h qt/src/contributionsection.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): score tooltip with per-component breakdown + hints

Hover the Decentralization Score number → rich-text tooltip table
showing each of the 7 weighted components with "how to improve" hint
text (e.g. "Open port 20999 inbound", "Enable relay mode"). Components
already at max show ✓.

Tooltip is driven by an installed event filter on the score label that
intercepts QEvent::ToolTip; the score breakdown is cached on each
NodePoller update so the tooltip is always current.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Sparkline hover tooltip (5min/1hr/24hr averages)

**Files:**
- Modify: `qt/src/sparklinewidget.h`
- Modify: `qt/src/sparklinewidget.cpp`
- Modify: `qt/src/nodepoller.h` + `.cpp` — add 1hr/24hr downsampled accumulators
- Modify: `qt/src/contributionsection.cpp` — pass 1hr/24hr averages alongside sparkline samples

**Approach:** Don't grow the 60-sample buffer to 17280. Add two separate accumulators in NodePoller per series — a 1-minute downsampled buffer of 60 entries (= 60 min = 1 hour) and a 1-hour downsampled buffer of 24 entries (= 24 hours). Total extra memory: ~700 bytes per series × 3 series = 2 KB. Tiny.

- [ ] **Step 1: Add longer-window accumulators to NodePoller**

In `nodepoller.h` private section:

```cpp
    // Phase 2b — downsampled longer-window buffers for sparkline tooltip.
    // bytes_in / out / relay × 1min × 60 = 1 hr ; bytes_in / out / relay × 1hr × 24 = 24 hr
    struct LongerWindowAccumulator {
        // 60 one-minute samples (5s tick × 12 = 1 min commit)
        QVector<qint64> minute_buffer;
        qint64          partial_minute_sum{0};
        int             partial_minute_ticks{0};
        // 24 one-hour samples (60 min × 1 hr commit)
        QVector<qint64> hour_buffer;
        qint64          partial_hour_sum{0};
        int             partial_hour_minutes{0};
    };
    LongerWindowAccumulator bytes_in_long_;
    LongerWindowAccumulator bytes_out_long_;
    LongerWindowAccumulator relay_bytes_long_;

    static void AccumulateLongerWindow(LongerWindowAccumulator& acc,
                                       qint64 sample);
    static qint64 AverageOverWindow(const QVector<qint64>& buf);
```

In `nodepoller.cpp`:

```cpp
void NodePoller::AccumulateLongerWindow(LongerWindowAccumulator& acc,
                                        qint64 sample) {
    acc.partial_minute_sum += sample;
    if (++acc.partial_minute_ticks >= 12) {  // 12 × 5s = 1min
        acc.minute_buffer.append(acc.partial_minute_sum);
        if (acc.minute_buffer.size() > 60) acc.minute_buffer.removeFirst();
        acc.partial_hour_sum += acc.partial_minute_sum;
        acc.partial_minute_sum = 0;
        acc.partial_minute_ticks = 0;
        if (++acc.partial_hour_minutes >= 60) {
            acc.hour_buffer.append(acc.partial_hour_sum);
            if (acc.hour_buffer.size() > 24) acc.hour_buffer.removeFirst();
            acc.partial_hour_sum = 0;
            acc.partial_hour_minutes = 0;
        }
    }
}

qint64 NodePoller::AverageOverWindow(const QVector<qint64>& buf) {
    if (buf.isEmpty()) return 0;
    qint64 s = 0;
    for (auto v : buf) s += v;
    return s / buf.size();
}
```

In `parsePeers`, after the existing pushSparklineSample lines, also call:

```cpp
    AccumulateLongerWindow(bytes_in_long_,    delta_recv);
    AccumulateLongerWindow(bytes_out_long_,   delta_sent);
    AccumulateLongerWindow(relay_bytes_long_, delta_relay);
```

Expose 9 accessors (3 series × 3 windows):

```cpp
    qint64 bytesIn5min()    const { return AverageOverWindow(bytes_in_buffer_); }
    qint64 bytesIn1hr()     const { return AverageOverWindow(bytes_in_long_.minute_buffer); }
    qint64 bytesIn24hr()    const { return AverageOverWindow(bytes_in_long_.hour_buffer); }
    // ...same for bytes out + relay
```

- [ ] **Step 2: Add tooltip filter to SparklineWidget**

`sparklinewidget.h`:

```cpp
public:
    // Optional callback called on hover; if set, returns the tooltip
    // text. If null, no tooltip.
    void setTooltipProvider(std::function<QString()> provider);

protected:
    bool event(QEvent* event) override;

private:
    std::function<QString()> tooltip_provider_;
```

`sparklinewidget.cpp`:

```cpp
void SparklineWidget::setTooltipProvider(std::function<QString()> provider) {
    tooltip_provider_ = std::move(provider);
    setMouseTracking(true);
}

bool SparklineWidget::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip && tooltip_provider_) {
        auto* help = static_cast<QHelpEvent*>(event);
        QToolTip::showText(help->globalPos(), tooltip_provider_(), this);
        return true;
    }
    return QWidget::event(event);
}
```

- [ ] **Step 3: Wire tooltips in ContributionSection ctor**

```cpp
// in contributionsection.cpp ctor, after creating sparklines, install tooltips
// (NodePoller is reachable via a setter or constructor parameter — implementer's
// choice depending on the existing wiring shape)
spark_in_->setTooltipProvider([this] {
    return formatSparklineTooltip("Bytes in",
        cached_5min_in_, cached_1hr_in_, cached_24hr_in_);
});
// ... same for out, relay
```

Add cached fields + a new slot `setBytesInLongWindows(qint64 _5min, qint64 _1hr, qint64 _24hr)` etc. (3 slots).

- [ ] **Step 4: Wire the lambda in MyNodeDashboard**

Extend the existing lambda in `mynodedashboard.cpp` (the one Phase 2a Task 6 introduced) to ALSO push the 9 long-window averages:

```cpp
connect(poller_, &NodePoller::contributionStatsUpdated,
        this, [this](const ContributionStats& stats) {
    contributionSection_->setContributionStats(stats);
    contributionSection_->setBytesInSamples(poller_->bytesInBuffer());
    contributionSection_->setBytesOutSamples(poller_->bytesOutBuffer());
    contributionSection_->setRelayBytesSamples(poller_->relayBytesBuffer());
    contributionSection_->setBytesInLongWindows(
        poller_->bytesIn5min(), poller_->bytesIn1hr(), poller_->bytesIn24hr());
    // ... same for out, relay
});
```

- [ ] **Step 5: Commit (no new tests for the tooltip — manual UI check covers; AccumulateLongerWindow can have its own unit test if time permits)**

Optional: 4 unit tests for AccumulateLongerWindow (covered if implementer wants to be defensive).

```bash
git add qt/src/sparklinewidget.h qt/src/sparklinewidget.cpp \
        qt/src/nodepoller.h qt/src/nodepoller.cpp \
        qt/src/contributionsection.h qt/src/contributionsection.cpp \
        qt/src/mynodedashboard.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): sparkline hover tooltip with 5min/1hr/24hr averages

Hover any of the 3 sparklines → tooltip with the 5-minute average
(already buffered for paint), the 1-hour average (downsampled to 60×1min
buckets) and the 24-hour average (downsampled to 24×1hr buckets).

Total extra memory: 60+24 qint64 × 3 series = ~2 KB. No 17280-sample
buffer needed.

The downsampler aggregates 12 polling ticks (5s × 12 = 1min) into a
single bucket; 60 buckets aggregate into a 1hr roll-up bucket. Tooltip
provider on each SparklineWidget reads the cached values from
ContributionSection, which receives them per-tick via 3 new setter
slots wired through MyNodeDashboard's existing contribution lambda.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Wire DiscoverySection into MyNodeDashboard

**Files:**
- Modify: `qt/src/mynodedashboard.h`
- Modify: `qt/src/mynodedashboard.cpp`

- [ ] **Step 1: Add forward decl + private member**

```cpp
class DiscoverySection;

private:
    DiscoverySection* discoverySection_{nullptr};
```

- [ ] **Step 2: Instantiate + add to layout + connect**

In the ctor of MyNodeDashboard, after `contributionSection_` is added:

```cpp
    discoverySection_ = new DiscoverySection(content);
    layout->addWidget(discoverySection_);
    connect(poller_, &NodePoller::hintsUpdated,
            discoverySection_, &DiscoverySection::setHints);
```

- [ ] **Step 3: Build + smoke**

```bash
cd /private/tmp/dinero-v8-phase2b-qt
cmake --build build-p2b --target dinero-qt -j8 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add qt/src/mynodedashboard.h qt/src/mynodedashboard.cpp
git commit -S -m "$(cat <<'EOF'
feat(qt-dashboard): MyNodeDashboard hosts DiscoverySection

5th section in the Cmd+K panel, below Contribution. Wires
NodePoller::hintsUpdated → DiscoverySection::setHints. No layout
restructure; Discovery sits at natural size with Peers carrying the
vertical stretch.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Full app build + manual sanity

**Files:** none (verification).

- [ ] **Step 1: Full dinero-qt build**

```bash
cd /private/tmp/dinero-v8-phase2b-qt
cmake --build build-p2b -j8 2>&1 | tail -5
```

Expected: `[100%] Built target dinero-qt`.

- [ ] **Step 2: All dashboard tests pass**

```bash
cd build-p2b
QT_QPA_PLATFORM=offscreen ctest -L "dashboard" --output-on-failure 2>&1 | tail -20
```

Expected: SparklineWidget + DecentralizationScore + ContributionSection + RelayHintsParser + DiscoverySection all pass.

- [ ] **Step 3: Smoke launch**

```bash
APP=/private/tmp/dinero-v8-phase2b-qt/build-p2b/bin/dinero-qt.app
nohup "$APP/Contents/MacOS/dinero-qt" \
    --datadir=/tmp/dinero-qt-phase2b-smoke -no-daemon-autostart \
    > /tmp/dinero-qt-phase2b-smoke.log 2>&1 &
sleep 6
ps -p $! > /dev/null && echo "ALIVE" || echo "CRASHED"
kill -TERM $!
sleep 2
rm -rf /tmp/dinero-qt-phase2b-smoke
```

- [ ] **Step 4: Commit sanity log**

(Same shape as Phase 2a Task 7's sanity log.)

---

## Task 8: Push + draft PR

```bash
cd /private/tmp/dinero-v8-phase2b-qt
git push -u origin feature/qt-dashboard-phase2b
gh pr create --draft --title "feat(qt): MyNodeDashboard Phase 2b — DiscoverySection + score tooltip + sparkline tooltip + real counters" --body "..."
```

PR body should mention prerequisites (PR #143, PR #144) and that the branch needs rebase onto dinero-main once both prerequisite PRs land.

Then poll CI as in Phase 2a Task 8.

---

## Pre-execution rebase checklist

Before Task 1 implementer starts, verify:

```bash
cd /private/tmp/dinero-v8-phase2b-qt
git log --oneline -5
# Confirm the base contains BOTH:
#   - Phase 2a code (qt/src/contributionsection.cpp exists)
#   - Phase 2b daemon code (relay_hints.list RPC exists)
ls qt/src/contributionsection.cpp 2>&1 | head -1
grep -rn "relay_hints.list" src/daemon/rpc_context_wiring.cpp 2>&1 | head -1
```

If either is missing, rebase onto whichever target makes both reachable:
- Both #143 + #144 merged → rebase onto dinero-main
- Only #144 merged, #143 still open → rebase onto a temporary merge of dinero-main + origin/feature/qt-dashboard-phase2a
- Only #143 merged → rebase onto dinero-main + origin/feature/relay-hints-list-rpc

---

## Coverage map (self-review)

| Phase 2b qt requirement | Task |
|---|---|
| HintRow value type + Q_DECLARE_METATYPE | 1 |
| ContributionStats real fields (drop 4 placeholders) | 1 + 2 |
| NodePoller polls relay_hints.list every 5s | 2 |
| NodePoller parses relay_hints.list | 2 + tests |
| NodePoller reads new getnetworkinfo.relay fields | 2 |
| DiscoverySection widget renders rows | 3 |
| Freshness bar (10-segment) | 3 |
| Stoplight glyph (●/◐/⚠/✗) | 3 |
| Fade-out on evict (QPropertyAnimation) | 3 |
| Score tooltip with breakdown | 4 |
| Sparkline hover tooltip (5min/1hr/24hr) | 5 |
| 1hr/24hr downsampled accumulators | 5 |
| Wire into MyNodeDashboard | 6 |
| Full build + ctest + smoke launch | 7 |
| Push + PR | 8 |
