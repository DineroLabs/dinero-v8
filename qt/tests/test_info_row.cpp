#include <QtTest/QtTest>
#include "inforow.h"
#include "scoreboardlabel.h"

class InfoRowTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void groupsWholeNumbersOnly() {
        QCOMPARE(groupLongIntegers("121208"), QString("121,208"));
        QCOMPARE(groupLongIntegers("121207 / 121208"), QString("121,207 / 121,208"));
        QCOMPARE(groupLongIntegers("0 txs, 1234 bytes"), QString("0 txs, 1,234 bytes"));
        QCOMPARE(groupLongIntegers("100.00000000 DIN"), QString("100.00000000 DIN"));
        QCOMPARE(groupLongIntegers("12,120,900 DIN"), QString("12,120,900 DIN"));
        QCOMPARE(groupLongIntegers("5"), QString("5"));
        QCOMPARE(groupLongIntegers("block 1314001"), QString("block 1,314,001"));
    }
    void splitsNameAndValue() {
        InfoRow row;
        row.setText("Height: 121208");
        QCOMPARE(row.nameText(), QString("Height"));
        QCOMPARE(row.valueText(), QString("121,208"));
        QCOMPARE(row.text(), QString("Height: 121208"));  // raw text kept for exports
        row.setText("Supply: unavailable");
        QCOMPARE(row.valueText(), QString("unavailable"));
        row.setText("Loading");
        QCOMPARE(row.nameText(), QString());
        QCOMPARE(row.valueText(), QString("Loading"));
    }
    void longDigitalValueFitsNarrowRow() {
        InfoRow row("Supply: 123456789012345678 DIN");
        row.setValueStyle(InfoRow::ValueStyle::SmallScoreboard);
        row.resize(160, 32);
        row.show();
        QCoreApplication::processEvents();

        const MiningScoreboardLabel* number = nullptr;
        for (const auto* label : row.findChildren<QLabel*>()) {
            if (const auto* scoreboard = dynamic_cast<const MiningScoreboardLabel*>(label)) {
                QVERIFY(number == nullptr);
                number = scoreboard;
            }
        }
        QVERIFY(number != nullptr);
        QVERIFY(number->width() < number->sizeHint().width());
        QVERIFY(number->parentWidget()->geometry().right() < row.width());
        QVERIFY(number->geometry().right() < number->parentWidget()->width());
    }
};
QTEST_MAIN(InfoRowTest)
#include "test_info_row.moc"
