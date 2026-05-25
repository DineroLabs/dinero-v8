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
    void widget_constructs_without_crash() {
        ContributionSection w;
        QVERIFY(w.isWidgetType());
    }

    void stats_update_propagates_to_labels() {
        ContributionSection w;
        ContributionStats s;
        s.circuits_active     = 3;
        s.blocks_served_today = 42;
        s.hints_sent          = 7;
        s.peers_via_gossip    = 11;
        s.bytes_in_rate       = 5120;   // ~1 KB/s after /5
        s.bytes_out_rate      = 0;
        s.relay_bytes_rate    = 0;
        w.setContributionStats(s);
        // Confirm the widget redisplays the values somewhere visible.
        const auto labels = w.findChildren<QLabel*>();
        QStringList texts;
        for (auto* l : labels) texts.append(l->text());
        QVERIFY(texts.contains(QStringLiteral("3")));
        QVERIFY(texts.contains(QStringLiteral("42")));
        QVERIFY(texts.contains(QStringLiteral("7")));
        QVERIFY(texts.contains(QStringLiteral("11")));
    }

    void score_update_renders_total_and_phrase() {
        ContributionSection w;
        DecentralizationScore s;
        s.total = 7.5;
        s.label = QStringLiteral("you're carrying real weight");
        w.setDecentralizationScore(s);
        const auto labels = w.findChildren<QLabel*>();
        bool found_total = false, found_phrase = false;
        for (auto* l : labels) {
            if (l->text() == QStringLiteral("7.5 / 10")) found_total = true;
            if (l->text() == s.label) found_phrase = true;
        }
        QVERIFY(found_total);
        QVERIFY(found_phrase);
    }

    void sparkline_samples_round_trip_without_crash() {
        ContributionSection w;
        QVector<qint64> samples;
        for (int i = 0; i < 60; ++i) samples.append(i * 1000);
        w.setBytesInSamples(samples);
        w.setBytesOutSamples(samples);
        w.setRelayBytesSamples(samples);
        // No assertion beyond not crashing; sparkline internals tested
        // in test_sparkline_widget.
    }
};

QTEST_MAIN(TestContributionSection)
#include "test_contribution_section.moc"
