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
    void classifiesWhatWasPasted() {
        const QString addr = "din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0jqg6t5y8";
        const qint64 now = 1790000000;
        const QString future = QString::number(now + 600);

        PayTarget full = classifyPayInput("dinero:" + addr + "?amount=1.5&rid=r-1&exp=" + future +
                                          "&desc=Coffee%20beans", now);
        QCOMPARE(full.kind, PayTarget::Kind::Link);
        QCOMPARE(full.address, addr);
        QCOMPARE(full.amount, QString("1.5"));
        QCOMPARE(full.requestId, QString("r-1"));
        QCOMPARE(full.expiresAt, now + 600);
        QCOMPARE(full.label, QString("Coffee beans"));

        PayTarget plain = classifyPayInput("  DINERO:" + addr + "  ", now);  // receive-screen link
        QCOMPARE(plain.kind, PayTarget::Kind::Link);
        QCOMPARE(plain.address, addr);
        QVERIFY(plain.amount.isEmpty());
        QCOMPARE(plain.expiresAt, 0LL);

        QCOMPARE(classifyPayInput(addr, now).kind, PayTarget::Kind::Address);
        QCOMPARE(classifyPayInput(" " + addr + "\n", now).address, addr);
        QCOMPARE(classifyPayInput("din1r" + addr.mid(5), now).kind, PayTarget::Kind::Address);
        QCOMPARE(classifyPayInput("aW52b2ljZSBieXRlcw==", now).kind, PayTarget::Kind::Invoice);
        QCOMPARE(classifyPayInput("   ", now).kind, PayTarget::Kind::Empty);
    }
    void explainsLinksThatCannotBePaid() {
        const QString addr = "din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0jqg6t5y8";
        const qint64 now = 1790000000;
        auto error = [&](const QString& text) {
            const PayTarget t = classifyPayInput(text, now);
            return t.kind == PayTarget::Kind::Invalid ? t.error : QString("<not invalid>");
        };
        QCOMPARE(error("dinero:" + addr + "?amount=1&exp=" + QString::number(now - 1)),
                 QString("This payment request has expired"));
        QCOMPARE(error("dinero:" + addr + "?amount=1&exp=soon"), QString("The link's expiry is not valid"));
        for (const char* bad : {"1e5", "-1", "1.123456789", "0", "abc"})
            QCOMPARE(error("dinero:" + addr + "?amount=" + bad), QString("The link's amount is not valid"));
        QCOMPARE(error("dinero:"), QString("The link has no address"));
        QCOMPARE(error("dinero:?amount=1"), QString("The link has no address"));
        QCOMPARE(error("dinero:dins1qqqsyqcyq5rqwzqfpg9scrgwpu"), QString("The link is not for a public Dinero address"));
    }
};
QTEST_GUILESS_MAIN(PayCollectPolicyTest)
#include "test_pay_collect_policy.moc"
