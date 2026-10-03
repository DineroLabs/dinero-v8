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
    QCOMPARE(dinero::qt::kHashEngineIntervalMs, 200);
    QCOMPARE(dinero::qt::kBlockFoundHighlightMs, 1000);
    QCOMPARE(dinero::qt::hashSampleCapacity(250, 20), 11);
    QCOMPARE(dinero::qt::hashSampleCapacity(0, 0), 1);
  }

  void newBlockStartsWhenTheHeightChanges() {
    QVERIFY(!dinero::qt::startsNewBlock(-1, 122460));  // first sample of a session
    QVERIFY(!dinero::qt::startsNewBlock(122460, 122460));
    QVERIFY(dinero::qt::startsNewBlock(122460, 122461));
  }

  void newBlockRowsFadeFromBlueGreenToGrey() {
    using dinero::qt::hashRowColor;
    const auto grey = hashRowColor(false, false, false, 0);
    QCOMPARE(grey.r, 151); QCOMPARE(grey.g, 163); QCOMPARE(grey.b, 174); QCOMPARE(grey.a, 150);
    const auto fresh = hashRowColor(false, false, true, 0);
    QVERIFY2(fresh.g > fresh.r + 60 && fresh.b > fresh.r + 60, "a new block's rows start blue-green");
    QVERIFY(fresh.a > grey.a);
    const auto half = hashRowColor(false, false, true, dinero::qt::kNewBlockFadeMs / 2);
    QVERIFY(half.r > fresh.r && half.r < grey.r);  // part way back to grey
    const auto done = hashRowColor(false, false, true, dinero::qt::kNewBlockFadeMs);
    QCOMPARE(done.r, grey.r); QCOMPARE(done.g, grey.g); QCOMPARE(done.b, grey.b); QCOMPARE(done.a, grey.a);
    // Your own found block stays orange, even on a fresh block.
    const auto mine = hashRowColor(true, true, true, 0);
    QCOMPARE(mine.r, 213); QCOMPARE(mine.g, 138); QCOMPARE(mine.b, 50); QCOMPARE(mine.a, 255);
    QCOMPARE(hashRowColor(true, false, true, 0).a, 205);
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

  void consolidationIsSingleFlight() {
    QVERIFY(dinero::qt::shouldEnableConsolidation(51, false));
    QVERIFY(!dinero::qt::shouldEnableConsolidation(50, false));
    QVERIFY(!dinero::qt::shouldEnableConsolidation(200, true));
  }

  void recoverableTemplateErrorsExpireFromTheLiveView() {
    QCOMPARE(dinero::qt::kTransientMiningErrorMs, 15000);
    QVERIFY(dinero::qt::isTransientMiningError(
      QStringLiteral("❌ Error: Failed to get block template: [GATE3] mismatch")));
    QCOMPARE(dinero::qt::miningOutputDisplayText(
               QStringLiteral("❌ Error: Failed to get block template: "
                              "CreateNewBlock: [GATE3] mismatch")),
             QStringLiteral("Template refresh delayed — retrying automatically"));
    QCOMPARE(dinero::qt::miningOutputDisplayText(
               QStringLiteral("Miner started successfully")),
             QStringLiteral("Miner started successfully"));
    const QString rejected = QStringLiteral(
      "❌ Error: Block rejected: bad-utreexo-root: computed=abc header=def");
    QVERIFY(dinero::qt::isTransientMiningError(rejected));
    QCOMPARE(dinero::qt::miningOutputDisplayText(rejected), rejected);
    QVERIFY(!dinero::qt::isTransientMiningError(
      QStringLiteral("Daemon RPC is unavailable; stopping embedded miner")));
  }
};

QTEST_MAIN(ResponsiveUiPolicyTest)
#include "test_responsive_ui_policy.moc"
