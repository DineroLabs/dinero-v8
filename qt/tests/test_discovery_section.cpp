// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "discoverysection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QtTest/QtTest>

using dinero::qt::dashboard::DiscoverySection;
using dinero::qt::dashboard::HintRow;

namespace {
HintRow mkRow(const QString& hex, const QString& endpoint,
              qint64 age, int fail, bool near) {
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
        QStringList texts;
        for (auto* l : labels) texts.append(l->text());
        QVERIFY(texts.contains(QStringLiteral("Discovery — 0 targets known")));
    }

    void single_hint_renders_target_ellipsis() {
        DiscoverySection s;
        s.setHints({mkRow("0102030405060708090a0102030405060708090a",
                          "1.2.3.4:9999", 47, 0, false)});
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == QStringLiteral("0102…090a")) { found = true; break; }
        }
        QVERIFY(found);
    }

    void near_eviction_row_shows_arrow_evict() {
        DiscoverySection s;
        s.setHints({mkRow("aa", "1.1.1.1:1", 800, 2, true)});
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == QStringLiteral("2 → evict")) { found = true; break; }
        }
        QVERIFY(found);
    }

    void row_present_in_two_consecutive_ticks_is_reused() {
        DiscoverySection s;
        const auto h = mkRow("aa", "1.1.1.1:1", 5, 0, false);
        s.setHints({h});
        const auto labels_before = s.findChildren<QLabel*>().size();
        s.setHints({h});
        const auto labels_after = s.findChildren<QLabel*>().size();
        QCOMPARE(labels_after, labels_before);
    }

    void evicted_row_does_not_crash() {
        DiscoverySection s;
        s.setHints({mkRow("aa", "1.1.1.1:1", 5, 0, false)});
        s.setHints({});
        // No assertion: just verifying no crash during fade-out setup.
        QVERIFY(true);
    }
};

QTEST_MAIN(TestDiscoverySection)
#include "test_discovery_section.moc"
