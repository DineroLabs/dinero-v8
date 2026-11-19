#include <QtTest/QtTest>
#include "swapformpolicy.h"
#include "swaplaunchpolicy.h"

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
                 QString("That is not a Bitcoin address of this network (it starts with bcrt1, m, n or 2)"));
        QCOMPARE(reviewOffer("1", "0.001", "BCRT1PUPPER", "bcrt").blocker, QString());
        // Exchanges and older wallets hand out 1… / 3… addresses.
        QCOMPARE(reviewOffer("1", "0.001", " 1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2 ", "bc").blocker, QString());
        QCOMPARE(reviewOffer("1", "0.001", "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy", "bc").blocker, QString());
        QCOMPARE(reviewOffer("1", "0.001", "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN3", "bc").blocker,  // one wrong character
                 QString("That is not a Bitcoin address of this network (it starts with bc1, 1 or 3)"));
    }
    void bitcoinAddresses() {
        using SwapFormPolicy::looksLikeBtcAddress;
        QVERIFY(looksLikeBtcAddress("bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq", "bc"));
        QVERIFY(looksLikeBtcAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", "bc"));
        QVERIFY(looksLikeBtcAddress("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy", "bc"));
        QVERIFY(looksLikeBtcAddress("1111111111111111111114oLvT2", "bc"));  // leading zero bytes
        for (const char* hrp : {"tb", "bcrt"}) {
            QVERIFY(looksLikeBtcAddress("mrS8eVKXguwufwvsVe9GtgGb7fif9UQeAu", hrp));
            QVERIFY(looksLikeBtcAddress("2N9hLwkSqr1cPQAPxbrGVUjxyjD11G2e1he", hrp));
            QVERIFY(!looksLikeBtcAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", hrp));  // mainnet address
        }
        QVERIFY(!looksLikeBtcAddress("mrS8eVKXguwufwvsVe9GtgGb7fif9UQeAu", "bc"));     // testnet address
        QVERIFY(!looksLikeBtcAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN3", "bc"));     // bad checksum
        QVERIFY(!looksLikeBtcAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN0", "bc"));     // not base58
        QVERIFY(!looksLikeBtcAddress("1bvbmseystwetqtfn5au4m4gfg7xjanvn2", "bc"));     // case matters
        QVERIFY(!looksLikeBtcAddress("", "bc"));
        QVERIFY(!looksLikeBtcAddress(QString(200, '2'), "bc"));
        QCOMPARE(SwapFormPolicy::btcAddressStarts("bc"), QString("bc1, 1 or 3"));
        QCOMPARE(SwapFormPolicy::btcAddressStarts("tb"), QString("tb1, m, n or 2"));
    }
    void dinFundingFeeAuthorization() {
        using SwapFormPolicy::dinFundingFeeRateHint;
        QCOMPARE(dinFundingFeeRateHint(0), qint64(2));      // no estimate yet
        QCOMPARE(dinFundingFeeRateHint(1.0), qint64(2));    // an idle node's floor
        QCOMPARE(dinFundingFeeRateHint(qQNaN()), qint64(2));
        QCOMPARE(dinFundingFeeRateHint(-5), qint64(2));
        QCOMPARE(dinFundingFeeRateHint(2.0), qint64(2));
        QCOMPARE(dinFundingFeeRateHint(2.01), qint64(3));   // rounded up, never below the estimate
        QCOMPARE(dinFundingFeeRateHint(17), qint64(17));
        QCOMPARE(dinFundingFeeRateHint(1e12), qint64(1000));
        QCOMPARE(SwapFormPolicy::formatUnits(SwapFormPolicy::kDinFundingMaximumFeeUna), QString("0.05000000"));
    }
    void bitcoinDeposit() {
        QJsonObject s{{"deposit_coin", "BTC"}, {"confirmations_needed", 2}, {"deposit_due_zat", 1003000}, {"deposit_seen_zat", 0},
                      {"deposit_confirmed_zat", 0}, {"deposit_confirmations", 0}};
        auto v = SwapFormPolicy::depositView(s);
        QCOMPARE(v.coin, QString("BTC"));
        QCOMPARE(v.needed, 2);
        QCOMPARE(v.hint, QString("pay 0.01003000 BTC to the deposit address"));
        s["deposit_seen_zat"] = 1003000;
        s["deposit_confirmations"] = 1;
        v = SwapFormPolicy::depositView(s);
        QCOMPARE(v.hint, QString("wait for 1 more Bitcoin confirmations of the deposit"));
        QVERIFY(!v.ready);
        s["deposit_confirmed_zat"] = 1003000;
        s["deposit_confirmations"] = 2;
        QVERIFY(SwapFormPolicy::depositView(s).ready);
        s["deposit_status"] = "no Bitcoin server reachable";
        v = SwapFormPolicy::depositView(s);
        QVERIFY(!v.ready);
        QCOMPARE(v.hint, QString("your node cannot reach any Bitcoin server; the deposit address still stands"));
        // An older node that names no coin is Zcash, at ten confirmations.
        v = SwapFormPolicy::depositView(QJsonObject{{"deposit_due_zat", 100}});
        QCOMPARE(v.coin, QString("ZEC"));
        QCOMPARE(v.needed, SwapFormPolicy::kZecDepositConfirmations);
        // A BTC deposit from a node that does not say how deep: two.
        QCOMPARE(SwapFormPolicy::depositView(QJsonObject{{"deposit_coin", "BTC"}}).needed, 2);
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
        // Done on chain, DIN not yet in the wallet: never shown as plain "Done".
        QCOMPARE(SwapFormPolicy::stateLabel("sweeping", "btc-seller"), QString("Done — moving the DIN to your wallet"));
        QVERIFY(!SwapFormPolicy::finished("sweeping"));
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

    // --- Zcash (docs/design/din-zec-atomic-swaps.md) ---
    void zecOfferBlockers() {
        using SwapFormPolicy::reviewZecOffer;
        const QString tm = "tm9iNYCVAhLLa4rJtfqqHauR5xL1REdpiDs";  // regtest P2PKH
        const QString t1 = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";  // mainnet P2PKH
        auto ok = reviewZecOffer("10", "2.5", tm, "regtest");
        QVERIFY2(ok.blocker.isEmpty(), qPrintable(ok.blocker));
        QCOMPARE(ok.btcSat, 250000000LL);
        QCOMPARE(reviewZecOffer("10", "", tm, "regtest").blocker, QString("Enter the ZEC amount you want"));
        QCOMPARE(reviewZecOffer("10", "0.00399999", tm, "regtest").blocker,
                 QString("ZEC amount is too small (at least 0.004 ZEC)"));
        QVERIFY(reviewZecOffer("10", "0.004", tm, "regtest").blocker.isEmpty());
        QCOMPARE(reviewZecOffer("10", "1", "", "regtest").blocker,
                 QString("Enter your Zcash address (where you receive the ZEC)"));
        QCOMPARE(reviewZecOffer("10", "1", t1, "regtest").blocker,
                 QString("Enter a transparent Zcash address (tm… or t2…)"));  // a mainnet address on regtest
        QVERIFY(reviewZecOffer("10", "1", t1, "main").blocker.isEmpty());
        QVERIFY(!reviewZecOffer("10", "1", "zs1shieldedaddressnotallowedhere000", "main").blocker.isEmpty());
        QVERIFY(!reviewZecOffer("10", "1", tm.left(34), "regtest").blocker.isEmpty());  // wrong length
    }
    void zecTextsAndStates() {
        QCOMPARE(SwapFormPolicy::kindOfText("dinswap2o0201"), QString("offer"));
        QVERIFY(SwapFormPolicy::isZecOffer(" dinswap2o0201"));
        QVERIFY(!SwapFormPolicy::isZecOffer("dinswap1o0101"));
        QVERIFY(SwapFormPolicy::canCancel("awaiting-deposit"));
        QVERIFY(SwapFormPolicy::canCancel("deposit-confirmed"));
        QCOMPARE(SwapFormPolicy::stateLabel("btc-locked", "btc-seller", "zec"),
                 QString::fromUtf8("ZEC locked — waiting for the seller to claim"));
        QCOMPARE(SwapFormPolicy::stateLabel("btc-locked", "btc-seller"),
                 QString::fromUtf8("BTC locked — waiting for the seller to claim"));
        QCOMPARE(SwapFormPolicy::stateLabel("awaiting-deposit", "btc-seller", "zec"), QString("Waiting for your ZEC deposit"));
        QCOMPARE(SwapFormPolicy::rate(1000000000, 2500000, "zat"), QString("250000 zat per DIN"));
    }
    void depositHints() {
        QJsonObject s{{"deposit_due_zat", 2520000}};
        QCOMPARE(SwapFormPolicy::depositView(s).hint, QString("pay 0.02520000 ZEC to the deposit address"));
        s["deposit_seen_zat"] = 1000000;
        QCOMPARE(SwapFormPolicy::depositView(s).hint, QString("0.01520000 ZEC still due"));
        s["deposit_seen_zat"] = 2520000;
        s["deposit_confirmations"] = 7;
        auto v = SwapFormPolicy::depositView(s);
        QCOMPARE(v.hint, QString("wait for 3 more Zcash confirmations of the deposit"));
        QVERIFY(!v.ready);
        s["deposit_confirmed_zat"] = 2520000;
        s["deposit_confirmations"] = 10;
        v = SwapFormPolicy::depositView(s);
        QVERIFY(v.ready);
        QVERIFY(v.hint.isEmpty());
        s["deposit_status"] = "Zcash node unreachable";
        QVERIFY(!SwapFormPolicy::depositView(s).ready);
    }
    void zecAcceptReviewTalksAboutTheDeposit() {
        QJsonObject d{{"kind", "offer"}, {"network", "regtest"}, {"chain", "zec"}, {"acceptable_now", true},
                      {"din_amount_una", 1000000000}, {"btc_amount_sat", 2500000}, {"t_btc_unix", 1800172800},
                      {"t_din_unix", 1800345600}, {"n_din", 30}};
        const auto r = SwapFormPolicy::reviewDecodedOffer(d, "regtest", 1800000000);
        QVERIFY(r.blocker.isEmpty());
        QVERIFY2(r.text.startsWith("You send 0.02500000 ZEC and receive 10.00000000 DIN"), qPrintable(r.text));
        QVERIFY(r.text.contains("0.02520000 ZEC"));  // amount + funding fee
        QVERIFY(r.text.contains("deposit"));
        QVERIFY(!r.text.contains("BTC"));
    }

    // --- The "Enable swaps" switch (swaplaunchpolicy.h) ---
    void theSwitchAddsNothingToTheNodeWhenOff() { QVERIFY(SwapLaunchPolicy::daemonArgs(false).isEmpty()); }
    void theSwitchStartsTheNodeForSwapsWithoutRiskingItsStart() {
        const QStringList args = SwapLaunchPolicy::daemonArgs(true);
        QVERIFY(args.contains("--swap.enable=1"));
        // A refused swap setting must leave the wallet's node running.
        QVERIFY(args.contains("--swap.optional=1"));
        QVERIFY(args.contains("--swap.btc_backend=light"));  // no Bitcoin Core to install
        QVERIFY(args.contains("--swap.mainnet_beta=1"));     // accepted in the switch's question
        // The mainnet gate for swaps without Bitcoin Core is the node's to lift, not the switch's.
        for (const auto& a : args) QVERIFY2(!a.contains("rehearsal"), qPrintable(a));
    }
    void theNodesReasonIsReadFromItsRefusal() {
        using SwapLaunchPolicy::refusalReason;
        QCOMPARE(refusalReason("swaps are disabled: start dinerod with swap.enable=1 and swap.btc_rpc=HOST:PORT"),
                 QString());  // never asked for: no reason
        QCOMPARE(refusalReason("swaps are disabled: swap.btc_backend must be core or light"),
                 QString("swap.btc_backend must be core or light"));
        QCOMPARE(refusalReason("no active wallet"), QString());
    }
    void theSwitchSaysWhereThingsStand() {
        using SwapLaunchPolicy::Node;
        using SwapLaunchPolicy::switchView;
        auto v = switchView(false, Node::Off, "", false);
        QVERIFY2(v.text.startsWith("Swaps are off."), qPrintable(v.text));
        QCOMPARE(v.button, QString("Turn on swaps"));
        QVERIFY(v.turnsOn && v.buttonEnabled);

        v = switchView(true, Node::Off, "", false);  // switched on, node not restarted yet
        QVERIFY2(v.text.contains("next time Dinero starts"), qPrintable(v.text));
        QCOMPARE(v.button, QString("Turn off swaps"));
        QVERIFY(!v.turnsOn);

        v = switchView(true, Node::Off, "swap.btc_backend=light is not offered on mainnet yet (testnet first); use "
                                        "swap.btc_rpc with Bitcoin Core", false);
        QVERIFY2(v.text.contains("cannot start here") && v.text.contains("not available on mainnet yet"),
                 qPrintable(v.text));
        QVERIFY2(!v.text.contains("swap.btc_rpc"), "no node options in front of the user");

        v = switchView(true, Node::Off, "swap.btc_backend=light needs a dinerod built with DINERO_SWAP_LIGHT_BTC", false);
        QVERIFY2(v.text.contains("built without them"), qPrintable(v.text));

        v = switchView(true, Node::On, "", false);
        QCOMPARE(v.text, QString("Swaps are on."));
        QVERIFY(v.buttonEnabled && !v.turnsOn);

        v = switchView(true, Node::On, "", true);  // turning swaps off would abandon an open swap
        QVERIFY(!v.buttonEnabled);
        QVERIFY2(v.text.contains("still open"), qPrintable(v.text));

        v = switchView(false, Node::On, "", false);
        QVERIFY2(v.text.contains("until Dinero is restarted"), qPrintable(v.text));
        QVERIFY(v.turnsOn);
    }
};

QTEST_MAIN(SwapFormPolicyTest)
#include "test_swap_form_policy.moc"
