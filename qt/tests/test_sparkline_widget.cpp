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
        QCOMPARE(w.samples().size(), 4);
    }
};

QTEST_MAIN(TestSparklineWidget)
#include "test_sparkline_widget.moc"
