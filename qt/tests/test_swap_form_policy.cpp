#include <QtTest/QtTest>
#include "swapformpolicy.h"

class SwapFormPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void exactAmountsNoFloatingPoint() {
        const auto r = SwapFormPolicy::reviewOffer("10.12345678", "0.00100001", "bcrt1pxyz", "bcrt");
        QVERIFY2(r.blocker.isEmpty(), qPrintable(r.blocker));
        QCOMPARE(r.dinUna, 1012345678LL);
        QCOMPARE(r.btcSat, 100001LL);
        QCOMPARE(SwapFormPolicy::formatUnits(r.dinUna), QString("10.12345678"));
        QCOMPARE(SwapFormPolicy::formatUnits(1), QString("0.00000001"));
    }
    void offerBlockers() {
        using SwapFormPolicy::reviewOffer;
        QCOMPARE(reviewOffer("", "1", "bcrt1q", "bcrt").blocker, QString("Enter the DIN amount you sell"));
        QCOMPARE(reviewOffer("1.000000001", "1", "bcrt1q", "bcrt").blocker,
                 QString("Enter a valid DIN amount (up to 8 decimals)"));
        QCOMPARE(reviewOffer("0", "1", "bcrt1q", "bcrt").blocker, QString("Enter a valid DIN amount (up to 8 decimals)"));
        QCOMPARE(reviewOffer("1", "-1", "bcrt1q", "bcrt").blocker, QString("Enter a valid BTC amount (up to 8 decimals)"));
        QCOMPARE(reviewOffer("1", "0.00009999", "bcrt1q", "bcrt").blocker,
                 QString("BTC amount is too small (at least 0.00010000 BTC)"));
        QCOMPARE(reviewOffer("1", "0.001", " ", "bcrt").blocker,
                 QString("Enter your Bitcoin address (where you receive the BTC)"));
        QCOMPARE(reviewOffer("1", "0.001", "bc1qmainnet", "bcrt").blocker,
                 QString("Enter a Bitcoin bcrt1… address (P2WPKH, P2WSH or Taproot)"));
        QCOMPARE(reviewOffer("1", "0.001", "BCRT1PUPPER", "bcrt").blocker, QString());
    }
    void networkHrp() {
        QCOMPARE(SwapFormPolicy::btcHrpForChain("mainnet"), QString("bc"));
        QCOMPARE(SwapFormPolicy::btcHrpForChain("main"), QString("bc"));
        QCOMPARE(SwapFormPolicy::btcHrpForChain("testnet"), QString("tb"));
        QCOMPARE(SwapFormPolicy::btcHrpForChain("regtest"), QString("bcrt"));
    }
    void rateAndTime() {
        QCOMPARE(SwapFormPolicy::rate(1000000000LL, 1000000LL), QString("100000 sat per DIN"));
        QCOMPARE(SwapFormPolicy::rate(0, 1), QString());
        QCOMPARE(SwapFormPolicy::timeLeft(0), QString("passed"));
        QCOMPARE(SwapFormPolicy::timeLeft(59), QString("1 min"));
        QCOMPARE(SwapFormPolicy::timeLeft(3 * 3600 + 120), QString("3 h 2 min"));
        QCOMPARE(SwapFormPolicy::timeLeft(2 * 86400 + 5 * 3600), QString("2 d 5 h"));
    }
    void cancelOnlyBeforeLocking() {
        QVERIFY(SwapFormPolicy::canCancel("offer-sent"));
        QVERIFY(SwapFormPolicy::canCancel("accepted"));
        for (const char* s : {"din-lock-broadcast", "din-locked", "btc-lock-broadcast", "btc-locked",
                              "btc-claim-broadcast", "done", "refunded"}) {
            QVERIFY2(!SwapFormPolicy::canCancel(s), s);
        }
        QVERIFY(SwapFormPolicy::finished("done") && SwapFormPolicy::finished("lost"));
        QVERIFY(!SwapFormPolicy::finished("btc-locked"));
        QVERIFY(SwapFormPolicy::stateLabel("lost", "btc-seller").startsWith("ATTENTION"));
        QVERIFY(SwapFormPolicy::stateLabel("paused: wallet locked", "").startsWith("Paused"));
    }
    void pastedTextKind() {
        QCOMPARE(SwapFormPolicy::kindOfText("  dinswap1o0102 "), QString("offer"));
        QCOMPARE(SwapFormPolicy::kindOfText("dinswap1a01"), QString("accept"));
        QCOMPARE(SwapFormPolicy::kindOfText("hello"), QString());
    }
    void acceptReviewRefusesWhatMustNotBeAccepted() {
        QJsonObject d{{"kind", "offer"}, {"network", "regtest"}, {"acceptable_now", true},
                      {"din_amount_una", 1000000000}, {"btc_amount_sat", 1000000},
                      {"t_btc_unix", 1800172800}, {"t_din_unix", 1800345600}, {"n_din", 30}};
        const auto ok = SwapFormPolicy::reviewDecodedOffer(d, "regtest", 1800000000);
        QVERIFY2(ok.blocker.isEmpty(), qPrintable(ok.blocker));
        QVERIFY(ok.text.contains("You send 0.01000000 BTC and receive 10.00000000 DIN"));
        QVERIFY(ok.text.contains("2 d 0 h"));
        QVERIFY(ok.text.contains("30 confirmations"));
        QVERIFY(ok.text.contains("watchtower"));

        auto other = d;
        other["network"] = "mainnet";
        QVERIFY(SwapFormPolicy::reviewDecodedOffer(other, "regtest", 1800000000).blocker.contains("mainnet"));
        auto stale = d;
        stale["acceptable_now"] = false;
        stale["reason"] = "offer has expired";
        QVERIFY(SwapFormPolicy::reviewDecodedOffer(stale, "regtest", 1800000000).blocker.contains("expired"));
        QVERIFY(!SwapFormPolicy::reviewDecodedOffer(QJsonObject{{"kind", "accept"}}, "regtest", 0).blocker.isEmpty());
    }
};

QTEST_MAIN(SwapFormPolicyTest)
#include "test_swap_form_policy.moc"
