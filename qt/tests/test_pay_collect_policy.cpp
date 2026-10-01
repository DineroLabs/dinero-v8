#include <QtTest/QtTest>
#include "paycollectpolicy.h"

using namespace PayCollectPolicy;

class PayCollectPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void amountSaysWhatIsMissing() {
        QCOMPARE(amountBlocker(""), QString("Enter an amount"));
        QCOMPARE(amountBlocker("  "), QString("Enter an amount"));
        QCOMPARE(amountBlocker("abc"), QString("Enter a valid amount (up to 8 decimals)"));
        QCOMPARE(amountBlocker("1.000000001"), QString("Enter a valid amount (up to 8 decimals)"));
        QCOMPARE(amountBlocker("0"), QString("Amount must be more than 0"));
        QCOMPARE(amountBlocker("50"), QString());
        QCOMPARE(amountBlocker("0.00000001"), QString());
    }
    void paymentStatusInPlainWords() {
        QCOMPARE(paymentStatus(0, 0).text, QString("Failed"));
        QCOMPARE(paymentStatus(0, 0).tone, Tone::Bad);
        QCOMPARE(paymentStatus(1, 0).text, QString("Verified · waiting for a block"));
        QCOMPARE(paymentStatus(1, 0).tone, Tone::Warn);
        QCOMPARE(paymentStatus(2, 1).text, QString("Confirmed · 1 confirmation"));
        QCOMPARE(paymentStatus(2, 3).text, QString("Confirmed · 3 confirmations"));
        QCOMPARE(paymentStatus(2, 3).tone, Tone::Info);
        QCOMPARE(paymentStatus(3, 6).text, QString("Final · 6 confirmations"));
        QCOMPARE(paymentStatus(3, 6).tone, Tone::Good);
        for (int tier = 0; tier <= 3; ++tier) {
            const Badge b = paymentStatus(tier, 2);
            QVERIFY2(!b.text.contains(QRegularExpression("\\bT[0-3]\\b")), qPrintable(b.text));
            QVERIFY2(b.tooltip.contains(QString("T%1").arg(tier)), qPrintable(b.tooltip));
        }
    }
    void verifyResultInPlainWords() {
        QCOMPARE(verifyResult("T1").text, QString("Payment verified"));
        QCOMPARE(verifyResult("T1").tone, Tone::Good);
        QCOMPARE(verifyResult("T0").text, QString("Not verified"));
        QCOMPARE(verifyResult("T0").tone, Tone::Warn);
        QCOMPARE(verifyResult("").text, QString("Not verified"));
    }
    void coloursComeFromTheAppPalette() {
        QCOMPARE(toneColor(Tone::Good), QString("#51cf66"));
        QCOMPARE(toneColor(Tone::Warn), QString("#f0b429"));
        QCOMPARE(toneColor(Tone::Bad), QString("#ff6b6b"));
        QCOMPARE(toneColor(Tone::Info), QString("#9fb3c8"));
        QCOMPARE(toneColor(Tone::Neutral), QString("#868e96"));
        QVERIFY(pillStyle(Tone::Good).contains("#51cf66"));
        const QString html = badgeHtml({"Payment verified", Tone::Good, "T1"});
        QVERIFY(html.contains("Payment verified"));
        QVERIFY(html.contains("#51cf66"));
        for (const char* material : {"#4CAF50", "#FF9800", "#2196F3", "#f44336"})
            QVERIFY2(!html.contains(material, Qt::CaseInsensitive), material);
    }
};
QTEST_GUILESS_MAIN(PayCollectPolicyTest)
#include "test_pay_collect_policy.moc"
