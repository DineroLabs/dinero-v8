#include "responsiveuipolicy.h"

#include <QJsonObject>
#include <QtTest/QtTest>

class ResponsiveUiPolicyTest : public QObject {
  Q_OBJECT

private Q_SLOTS:
  void largeWalletIsBoundedToOnePage() {
    QJsonArray rows;
    for (int i = 0; i < 4200; ++i) rows.append(QJsonObject{{"vout", i}});

    const auto first = dinero::qt::paginateUtxos(rows, 0);
    QCOMPARE(first.total_rows, 4200);
    QCOMPARE(first.rows.size(), dinero::qt::kUtxoPageSize);
    QCOMPARE(first.page_count, 21);
    QCOMPARE(first.first_row, 0);

    const auto last = dinero::qt::paginateUtxos(rows, 999);
    QCOMPARE(last.page_index, 20);
    QCOMPARE(last.rows.size(), dinero::qt::kUtxoPageSize);
    QCOMPARE(last.first_row, 4000);
  }

  void emptyWalletHasStablePageMetadata() {
    const auto page = dinero::qt::paginateUtxos({}, -4);
    QCOMPARE(page.total_rows, 0);
    QCOMPARE(page.page_index, 0);
    QCOMPARE(page.page_count, 1);
    QVERIFY(page.rows.isEmpty());
  }

  void hiddenUtxoPanelDoesNotPollWithoutExplicitRequest() {
    QVERIFY(!dinero::qt::shouldPollUtxos(false, false));
    QVERIFY(dinero::qt::shouldPollUtxos(true, false));
    QVERIFY(dinero::qt::shouldPollUtxos(false, true));
  }

  void miningCinematicRequiresVisibleMiningPanel() {
    QVERIFY(!dinero::qt::shouldRunMiningCinematic(true, false));
    QVERIFY(!dinero::qt::shouldRunMiningCinematic(false, true));
    QVERIFY(dinero::qt::shouldRunMiningCinematic(true, true));
  }

  void hashEngineUsesBoundedTenHertzPolicy() {
    QCOMPARE(dinero::qt::kHashEngineIntervalMs, 100);
    QCOMPARE(dinero::qt::kBlockFoundHighlightMs, 1000);
    QCOMPARE(dinero::qt::hashSampleCapacity(250, 20), 11);
    QCOMPARE(dinero::qt::hashSampleCapacity(0, 0), 1);
  }

  void compactDifficultyMatchesMinerOutput() {
    QCOMPARE(dinero::qt::compactDifficultyText(0x1d00cf7e),
             QStringLiteral("0x1d00cf7e"));
    QCOMPARE(dinero::qt::compactDifficultyText(0), QStringLiteral("-"));
  }

  void blockHighlightNeverPausesVisibleHashEngine() {
    QVERIFY(dinero::qt::shouldRunHashEngine(true, true, false));
    QVERIFY(dinero::qt::shouldRunHashEngine(true, true, true));
    QVERIFY(!dinero::qt::shouldRunHashEngine(true, false, true));
  }
};

QTEST_MAIN(ResponsiveUiPolicyTest)
#include "test_responsive_ui_policy.moc"
