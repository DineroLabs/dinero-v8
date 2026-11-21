// The Portfolio tab: every coin listed, the Bitcoin wallet off until turned
// on, and a payment sent only as it was reviewed. Offline: no node answers;
// the node's replies are played in.
#include <QtTest/QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>

#include "portfoliopolicy.h"
#include "portfoliowidget.h"
#include "rpcclient.h"
#include "swaplaunchpolicy.h"

namespace {
template <typename T>
T* Find(QWidget* w, const char* name) {
    auto* c = w->findChild<T*>(name);
    if (!c) qFatal("missing widget %s", name);
    return c;
}
const QJsonObject kOn{{"offered", true}, {"enabled", true}, {"address", "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"}};
const QJsonObject kPreview{{"txid", "aa11"}, {"to", "bc1qdestination"}, {"amount_sat", 20000}, {"fee_sat", 153},
                           {"change_sat", 11664}, {"dust_to_fee_sat", 0}, {"feerate_sat_per_vb", 1.0}};
}  // namespace

class PortfolioTabTest : public QObject {
    Q_OBJECT
    RpcClient* rpc_ = nullptr;
    PortfolioWidget* tab_ = nullptr;

private Q_SLOTS:
    void initTestCase() {  // the switch is a stored setting: keep the test's apart from a real install's
        QCoreApplication::setOrganizationName("DineroPortfolioTabTest");
        QCoreApplication::setApplicationName("test_portfolio_tab");
    }
    void cleanupTestCase() { QSettings().clear(); }
    void init() {
        QSettings().clear();
        rpc_ = new RpcClient(this);
        rpc_->setEndpoint(QUrl("http://127.0.0.1:1/"));  // nobody listens
        tab_ = new PortfolioWidget(rpc_);
        tab_->resize(1200, 1000);
        tab_->show();
        QVERIFY(QTest::qWaitForWindowExposed(tab_));
    }
    void cleanup() {
        delete tab_;
        tab_ = nullptr;
    }

    // There was a way to turn the Bitcoin wallet on and none to turn it off.
    void theBitcoinWalletCanBeTurnedOffAndBackOn() {
        auto* off = Find<QPushButton>(tab_, "bitcoinTurnOff");
        auto* on = Find<QPushButton>(tab_, "bitcoinTurnOn");
        auto* intro = Find<QLabel>(tab_, "bitcoinIntro");
        auto* coins = Find<QTableWidget>(tab_, "coinsTable");
        auto* address = Find<QLineEdit>(tab_, "bitcoinAddress");
        QStringList asked;
        bool answer = false;
        tab_->setTurnOffConfirmer([&](const QString& q) { asked << q; return answer; });
        QSettings().setValue(SwapLaunchPolicy::bitcoinWalletKey(), true);
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"sat", 10044}});
        QVERIFY(off->isVisible() && address->isVisible());

        // Asked first, with the balance; "no" changes nothing.
        off->click();
        QCOMPARE(asked.size(), 1);
        QVERIFY2(asked[0].contains("0.00010044 BTC") && asked[0].contains("Nothing is deleted"), qPrintable(asked[0]));
        QVERIFY(QSettings().value(SwapLaunchPolicy::bitcoinWalletKey()).toBool());
        QVERIFY(address->isVisible());

        // "Yes": the switch is stored off (the node is started without it next time), the wallet leaves the page.
        answer = true;
        off->click();
        QVERIFY(QSettings().contains(SwapLaunchPolicy::bitcoinWalletKey()));
        QVERIFY(!QSettings().value(SwapLaunchPolicy::bitcoinWalletKey()).toBool());
        QVERIFY(SwapLaunchPolicy::daemonArgs(false, QSettings().value(SwapLaunchPolicy::bitcoinWalletKey()).toBool()).isEmpty());
        QVERIFY(!address->isVisible() && !off->isVisible());
        QVERIFY2(intro->text().contains("switched off") && intro->text().contains("show again"), qPrintable(intro->text()));
        QCOMPARE(coins->item(1, 2)->text(), QString("Not turned on"));
        QVERIFY2(!coins->item(1, 1)->text().contains("BTC"), "no balance on show while off");
        // The node still runs it until Dinero restarts: its answers do not bring the wallet back by themselves.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        QVERIFY(!address->isVisible());

        // Back on: one click, the same address, nothing to start.
        QVERIFY(on->isVisible());
        on->click();
        QVERIFY(QSettings().value(SwapLaunchPolicy::bitcoinWalletKey()).toBool());
        QVERIFY(address->isVisible() && off->isVisible());
        QCOMPARE(address->text(), QString("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"));

        // A balance that could not be read is said so in the question, not passed over as nothing.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"error", "cannot reach any Bitcoin server"}});
        answer = false;
        off->click();
        QVERIFY2(asked.last().contains("could not be read"), qPrintable(asked.last()));

        // Swaps use the same Bitcoin connection: refused, with the way out, and nothing is asked or stored.
        QSettings().setValue(SwapLaunchPolicy::enabledKey(), true);
        const int before = asked.size();
        answer = true;
        off->click();
        QCOMPARE(asked.size(), before);
        QVERIFY(QSettings().value(SwapLaunchPolicy::bitcoinWalletKey()).toBool());
        QVERIFY2(Find<QLabel>(tab_, "portfolioStatus")->text().contains("Turn swaps off first"), "the way out is said");
        QVERIFY(address->isVisible());
    }

    void amountsAreReadStrictly() {
        using PortfolioPolicy::parseBtc;
        QCOMPARE(parseBtc("0.0002").value_or(-1), qint64(20000));
        QCOMPARE(parseBtc("0,0002").value_or(-1), qint64(20000));
        QCOMPARE(parseBtc(".5").value_or(-1), qint64(50000000));
        QCOMPARE(parseBtc("1").value_or(-1), qint64(100000000));
        QCOMPARE(parseBtc(" 0.00000001 ").value_or(-1), qint64(1));
        for (const char* bad : {"", ".", "0", "0.0", "-1", "1e-4", "0.000000001", "abc", "1.2.3", "22000000", "0x10", "1 000"}) {
            QVERIFY2(!parseBtc(bad).has_value(), bad);
        }
        QCOMPARE(PortfolioPolicy::formatBtc(31817), QString("0.00031817 BTC"));
    }

    // The Bitcoin wallet has its own switch: it does not need swaps turned on.
    void theBitcoinWalletIsSwitchedOnWithoutTheSwapTab() {
        QSettings().remove(SwapLaunchPolicy::bitcoinWalletKey());
        auto* turnOn = Find<QPushButton>(tab_, "bitcoinTurnOn");
        auto* intro = Find<QLabel>(tab_, "bitcoinIntro");
        auto* coins = Find<QTableWidget>(tab_, "coinsTable");
        // A node started without it: offered here, on the Wallet page, not "go to the Swap tab".
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus",
                               QJsonObject{{"error", "the Bitcoin wallet is not running on this node (swap.btc_wallet=1 starts it)"}});
        QVERIFY(turnOn->isVisible() && turnOn->isEnabled());
        QCOMPARE(coins->item(1, 2)->text(), QString("Not switched on"));
        QVERIFY2(intro->text().contains("without any other Bitcoin software"), qPrintable(intro->text()));
        QVERIFY2(!intro->text().contains("swap", Qt::CaseInsensitive), "no word about swaps");
        QVERIFY(!QSettings().value(SwapLaunchPolicy::bitcoinWalletKey(), false).toBool());
        turnOn->click();
        QVERIFY(QSettings().value(SwapLaunchPolicy::bitcoinWalletKey(), false).toBool());  // remembered for later starts
        QVERIFY2(!turnOn->isEnabled(), "one click, one request");
        // The running node starts it: no restart. The page moves straight on to
        // giving this wallet its Bitcoin account.
        Q_EMIT rpc_->rpcResult("portfolio.btcstart", QJsonObject{{"offered", true}, {"enabled", false}});
        QVERIFY2(!intro->text().contains("quit Dinero"), qPrintable(intro->text()));
        Q_EMIT rpc_->rpcResult("portfolio.btcenable", QJsonObject{{"error", "wallet is locked: wallet.unlock first"}});
        QVERIFY2(Find<QLabel>(tab_, "portfolioStatus")->text().contains("locked"), "a locked wallet is said so");
        QVERIFY(turnOn->isVisible() && turnOn->isEnabled());
        turnOn->click();  // after unlocking
        Q_EMIT rpc_->rpcResult("portfolio.btcenable", kOn);
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        QCOMPARE(Find<QLineEdit>(tab_, "bitcoinAddress")->text(), QString("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"));
        // Its first balance comes a little later: the node is still collecting block headers.
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"error", "still catching up with Bitcoin's block headers: try again in a moment"}});
        QVERIFY2(coins->item(1, 2)->text().startsWith("Getting ready"), qPrintable(coins->item(1, 2)->text()));
        // An older node does not know the request: the switch is stored, and the page says a restart does it.
        QSettings().remove(SwapLaunchPolicy::bitcoinWalletKey());
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus",
                               QJsonObject{{"error", "the Bitcoin wallet is not running on this node (swap.btc_wallet=1 starts it)"}});
        turnOn->click();
        Q_EMIT rpc_->rpcError("portfolio.btcstart", -32601, "Method not found: btc.start");
        QVERIFY(!turnOn->isVisible());
        QVERIFY2(intro->text().contains("quit Dinero"), qPrintable(intro->text()));
        // What the node is started with next time: its Bitcoin client only, swaps stay off.
        const QStringList args = SwapLaunchPolicy::daemonArgs(false, true);
        QVERIFY(args.contains("--swap.btc_wallet=1") && args.contains("--swap.optional=1"));
        QVERIFY2(!args.contains("--swap.enable=1"), "the Bitcoin switch must not turn swaps on");
        QVERIFY(SwapLaunchPolicy::daemonArgs(false, false).isEmpty());
        QVERIFY(SwapLaunchPolicy::daemonArgs(true, true).contains("--swap.enable=1"));   // swaps bring Bitcoin with them
        QVERIFY(!SwapLaunchPolicy::daemonArgs(true, true).contains("--swap.btc_wallet=1"));
        // Switched on, but the node could not start it: the reason in words.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus",
                               QJsonObject{{"error", "the Bitcoin wallet is not running on this node: swap.btc_backend=light "
                                                     "needs a dinerod built with DINERO_SWAP_LIGHT_BTC"}});
        QVERIFY2(intro->text().contains("built without it"), qPrintable(intro->text()));
        QVERIFY2(!intro->text().contains("DINERO_SWAP_LIGHT_BTC"), "no build options in front of the user");
        QVERIFY(!turnOn->isVisible());
        // After the restart the node runs it: the wallet's own "turn on" (which needs the unlock) takes over.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", QJsonObject{{"offered", true}, {"enabled", false}});
        QVERIFY(turnOn->isVisible());
        QVERIFY2(intro->text().contains("recovery words"), qPrintable(intro->text()));
        // An older node says "swaps are disabled" to the same question: treated the same way.
        QSettings().remove(SwapLaunchPolicy::bitcoinWalletKey());
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", QJsonObject{{"error", "swaps are disabled: start dinerod with swap.enable=1"}});
        QVERIFY(turnOn->isVisible());
        QSettings().remove(SwapLaunchPolicy::bitcoinWalletKey());
    }

    void everyCoinIsListedAndWhatCannotBeServedSaysWhy() {
        auto* coins = Find<QTableWidget>(tab_, "coinsTable");
        QCOMPARE(coins->rowCount(), 4);
        QCOMPARE(coins->item(0, 0)->text(), QString("Dinero (DIN)"));
        QCOMPARE(coins->item(1, 0)->text(), QString("Bitcoin (BTC)"));
        QCOMPARE(coins->item(2, 2)->text(), QString("Not available yet"));  // Zcash: listed, not hidden
        Q_EMIT rpc_->rpcResult("portfolio.din", QJsonObject{{"total", 1234.5}});
        QCOMPARE(coins->item(0, 1)->text(), QString("1234.50000000 DIN"));
        // A node that cannot serve the wallet at all says why, and there is nothing to click.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus",
                               QJsonObject{{"offered", false}, {"enabled", false},
                                           {"why_not", "the Bitcoin wallet needs this node to reach Bitcoin without Bitcoin Core"}});
        QVERIFY2(coins->item(1, 2)->text().contains("without Bitcoin Core"), qPrintable(coins->item(1, 2)->text()));
        QVERIFY(!Find<QPushButton>(tab_, "bitcoinTurnOn")->isVisible());
        QVERIFY(!Find<QGroupBox>(tab_, "bitcoinSend")->isVisible());
    }

    void theBitcoinWalletIsOffUntilTheUserTurnsItOn() {
        auto* turnOn = Find<QPushButton>(tab_, "bitcoinTurnOn");
        auto* coins = Find<QTableWidget>(tab_, "coinsTable");
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", QJsonObject{{"offered", true}, {"enabled", false}});
        QVERIFY(turnOn->isVisible());
        QCOMPARE(coins->item(1, 2)->text(), QString("Not turned on"));
        // It says what turning it on does, before it is done.
        const QString intro = Find<QLabel>(tab_, "bitcoinIntro")->text();
        QVERIFY2(intro.contains("public Bitcoin servers") && intro.contains("recovery words"), qPrintable(intro));
        QVERIFY(!Find<QGroupBox>(tab_, "bitcoinSend")->isVisible());
        turnOn->click();
        Q_EMIT rpc_->rpcResult("portfolio.btcenable", QJsonObject{{"error", "wallet is locked: wallet.unlock first"}});
        QVERIFY(turnOn->isEnabled());
        QVERIFY2(Find<QLabel>(tab_, "portfolioStatus")->text().contains("locked"), "the refusal is shown");
        // On: the address, and the balance once read.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        QVERIFY(!turnOn->isVisible());
        QCOMPARE(Find<QLineEdit>(tab_, "bitcoinAddress")->text(), QString("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"));
        QCOMPARE(coins->item(1, 2)->text(), QString("Reading"));
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"sat", 31817}, {"addresses", QJsonArray{}}});
        QCOMPARE(coins->item(1, 1)->text(), QString("0.00031817 BTC"));
        QCOMPARE(coins->item(1, 2)->text(), QString("Proven against Bitcoin block headers"));
        // A payment seen by the network but not yet in a block: said to be on its way, not added to the balance.
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"sat", 31817}, {"arriving_sat", 8200}});
        QCOMPARE(coins->item(1, 1)->text(), QString("0.00031817 BTC"));
        QVERIFY2(coins->item(1, 2)->text().startsWith("0.00008200 BTC on its way"), qPrintable(coins->item(1, 2)->text()));
        // Just paid: the rest of the coin is change coming back, and the smaller balance is explained.
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"sat", 0}, {"returning_sat", 4761}});
        QVERIFY2(coins->item(1, 2)->text().startsWith("0.00004761 BTC of yours comes back"), qPrintable(coins->item(1, 2)->text()));
        // Servers unreachable: "not read", never zero.
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"error", "cannot reach any Bitcoin server"}});
        QVERIFY2(coins->item(1, 2)->text().startsWith("Not read"), qPrintable(coins->item(1, 2)->text()));
        QVERIFY(coins->item(1, 1)->text() != QString("0.00000000 BTC"));
    }

    // On the Wallet page the section lists the other coins only.
    void withinTheWalletPageItListsTheOtherCoinsOnly() {
        PortfolioWidget section(rpc_, nullptr, PortfolioWidget::Place::WithinWallet);
        section.resize(1200, 700);
        section.show();
        QVERIFY(QTest::qWaitForWindowExposed(&section));
        QCOMPARE(Find<QGroupBox>(&section, "portfolioCoins")->title(), QString("Other coins in this wallet"));
        auto* coins = Find<QTableWidget>(&section, "coinsTable");
        QCOMPARE(coins->rowCount(), 3);
        QCOMPARE(coins->item(0, 0)->text(), QString("Bitcoin (BTC)"));
        Q_EMIT rpc_->rpcResult("portfolio.din", QJsonObject{{"total", 5.0}});  // not asked for, and ignored
        QCOMPARE(coins->rowCount(), 3);
        QCOMPARE(coins->item(0, 0)->text(), QString("Bitcoin (BTC)"));
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", QJsonObject{{"offered", true}, {"enabled", false}});
        QCOMPARE(coins->item(0, 2)->text(), QString("Not turned on"));
        QVERIFY(Find<QPushButton>(&section, "bitcoinTurnOn")->isVisible());
        // As a section it takes only the height it needs.
        QApplication::processEvents();
        for (auto* box : section.findChildren<QGroupBox*>()) {
            if (!box->isVisible()) continue;
            QVERIFY2(box->height() <= box->sizeHint().height() + 4, qPrintable(box->objectName() + " stretched"));
        }
    }

    void aNodeThatDoesNotAnswerLeavesWhatIsOnShow() {
        auto* coins = Find<QTableWidget>(tab_, "coinsTable");
        // Before the first answer: waiting, not "unavailable".
        Q_EMIT rpc_->rpcError("portfolio.btcstatus", -1, "Connection refused");
        QCOMPARE(coins->item(1, 2)->text(), QString("Waiting for your node"));
        QVERIFY2(!Find<QLabel>(tab_, "bitcoinIntro")->text().contains("Connection refused"), "no transport error as a reason");
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"sat", 31817}});
        // The node restarts: the wallet stays on show with its last balance.
        Q_EMIT rpc_->rpcError("portfolio.btcstatus", -1, "Connection refused");
        Q_EMIT rpc_->rpcError("portfolio.btcbalance", -1, "Connection refused");
        QVERIFY(Find<QGroupBox>(tab_, "bitcoinSend")->isVisible());
        QCOMPARE(coins->item(1, 1)->text(), QString("0.00031817 BTC"));
        QCOMPARE(Find<QLineEdit>(tab_, "bitcoinAddress")->text(), QString("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"));
    }

    void spareHeightStaysBelowTheContent() {
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", QJsonObject{{"offered", true}, {"enabled", false}});
        QApplication::processEvents();
        for (auto* box : tab_->findChildren<QGroupBox*>()) {
            if (!box->isVisible()) continue;
            QVERIFY2(box->height() <= box->sizeHint().height() + 4,
                     qPrintable(box->objectName() + QString(" stretched to %1 (hint %2)").arg(box->height()).arg(box->sizeHint().height())));
        }
    }

    void aPaymentIsSentOnlyAsItWasReviewed() {
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        auto* review = Find<QPushButton>(tab_, "bitcoinReview");
        auto* send = Find<QPushButton>(tab_, "bitcoinSendNow");
        auto* text = Find<QLabel>(tab_, "bitcoinReviewText");
        auto* to = Find<QLineEdit>(tab_, "bitcoinSendTo");
        auto* amount = Find<QLineEdit>(tab_, "bitcoinSendAmount");
        QVERIFY(!review->isEnabled());
        QVERIFY(!send->isVisible());
        to->setText("bc1qdestination");
        amount->setText("abc");
        QVERIFY(!review->isEnabled());
        amount->setText("0.0002");
        QVERIFY(review->isEnabled());
        review->click();
        QVERIFY(!send->isVisible());  // nothing to send until the node has answered
        Q_EMIT rpc_->rpcResult("portfolio.btcpreview", kPreview);
        QVERIFY(send->isVisible() && send->isEnabled());
        QVERIFY2(text->text().contains("They receive: 0.00020000 BTC") && text->text().contains("Network fee: 0.00000153 BTC") &&
                     text->text().contains("Leaves your wallet: 0.00020153 BTC") &&
                     text->text().contains("change: 0.00011664 BTC"),
                 qPrintable(text->text()));
        // Changing anything withdraws the reviewed payment.
        amount->setText("0.0003");
        QVERIFY(!send->isVisible());
        QVERIFY(!text->isVisible());
        amount->setText("0.0002");
        review->click();
        Q_EMIT rpc_->rpcResult("portfolio.btcpreview", kPreview);
        Find<QComboBox>(tab_, "bitcoinSpeed")->setCurrentIndex(1);
        QVERIFY(!send->isVisible());
        Find<QCheckBox>(tab_, "bitcoinSendAll")->setChecked(true);
        QVERIFY(!amount->isEnabled());
        QVERIFY(review->isEnabled());
        // A refusal at send time: reviewed again, not retried blindly.
        review->click();
        Q_EMIT rpc_->rpcResult("portfolio.btcpreview", kPreview);
        send->click();
        QVERIFY(!send->isEnabled());  // one click, one payment
        Q_EMIT rpc_->rpcResult("portfolio.btcsend", QJsonObject{{"error", "the payment is no longer the one that was shown"}});
        QVERIFY(!send->isVisible());
        QVERIFY2(Find<QLabel>(tab_, "portfolioStatus")->text().contains("no longer the one"), "why");
        // Sent: the form is cleared and the id shown.
        review->click();
        Q_EMIT rpc_->rpcResult("portfolio.btcpreview", kPreview);
        send->click();
        Q_EMIT rpc_->rpcResult("portfolio.btcsend", QJsonObject{{"txid", "aa11"}, {"sent", true}});
        QVERIFY(to->text().isEmpty());
        QVERIFY(!send->isVisible());
        QVERIFY2(Find<QLabel>(tab_, "portfolioStatus")->text().contains("aa11"), "the transaction id");
    }

    void theListOffersAHigherFeeOnlyForAnUnconfirmedPaymentOfOurs() {
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        auto* history = Find<QTableWidget>(tab_, "bitcoinHistory");
        auto* raise = Find<QPushButton>(tab_, "bitcoinRaiseFee");
        QVERIFY(!history->isVisible());
        Q_EMIT rpc_->rpcResult("portfolio.btchistory",
                               QJsonArray{QJsonObject{{"txid", "sentpending"}, {"received", false}, {"amount_sat", 10000},
                                                      {"fee_sat", 153}, {"confirmations", 0}},
                                          QJsonObject{{"txid", "sentdone"}, {"received", false}, {"amount_sat", 5000},
                                                      {"fee_sat", 200}, {"confirmations", 3}},
                                          QJsonObject{{"txid", "got"}, {"received", true}, {"amount_sat", 31817},
                                                      {"confirmations", 0}}});
        QVERIFY(history->isVisible());
        QCOMPARE(history->rowCount(), 3);
        QCOMPARE(history->item(0, 0)->text(), QString("Sent"));
        QCOMPARE(history->item(0, 2)->text(), QString("Sent, not confirmed yet"));
        QCOMPARE(history->item(1, 2)->text(), QString("Confirmed (3)"));
        QCOMPARE(history->item(2, 0)->text(), QString("Received"));
        QVERIFY(!raise->isEnabled());
        history->selectRow(1);
        QVERIFY(!raise->isEnabled());  // confirmed
        history->selectRow(2);
        QVERIFY(!raise->isEnabled());  // not ours to re-send
        history->selectRow(0);
        QVERIFY(raise->isEnabled());
        raise->click();
        QJsonObject bump = kPreview;
        bump["replaces"] = "sentpending";
        bump["txid"] = "bb22";
        Q_EMIT rpc_->rpcResult("portfolio.btcbumppreview", bump);
        auto* send = Find<QPushButton>(tab_, "bitcoinSendNow");
        QVERIFY(send->isVisible());
        QCOMPARE(send->text(), QString("Send with the higher fee"));
        QVERIFY2(Find<QLabel>(tab_, "bitcoinReviewText")->text().contains("only one of the two"), "what a replacement is");
    }

    void looksLikeTheApp() {
        for (auto* b : tab_->findChildren<QPushButton*>()) {
            QVERIFY2(!b->toolTip().trimmed().isEmpty(), qPrintable("no hover text on " + b->text()));
            QVERIFY2(b->styleSheet().contains("border-radius: 7px"), qPrintable("not a chrome button: " + b->text()));
        }
        for (auto* t : tab_->findChildren<QTableWidget*>()) {
            QVERIFY2(t->styleSheet().contains("alternate-background-color: #2"), qPrintable(t->objectName()));
        }
        const QString dir = qEnvironmentVariable("PORTFOLIO_TAB_SHOTS");
        if (dir.isEmpty()) return;
        tab_->setStyleSheet(appPageStyleForTest());
        QTest::qWait(300);  // the offline client's own "connection refused" replies arrive first
        Q_EMIT rpc_->rpcResult("portfolio.din", QJsonObject{{"total", 1234.5}});
        Q_EMIT rpc_->rpcResult("portfolio.btcstatus", kOn);
        Q_EMIT rpc_->rpcResult("portfolio.btcbalance", QJsonObject{{"sat", 31817}});
        Q_EMIT rpc_->rpcResult("portfolio.btchistory",
                               QJsonArray{QJsonObject{{"txid", "1afe86fec896f52e75860ab6bdc460a2b25b99b6"}, {"received", false},
                                                      {"amount_sat", 10000}, {"fee_sat", 153}, {"confirmations", 0}},
                                          QJsonObject{{"txid", "237caf59b6f29da7bd8f4c28b42ed48c8b617b91"}, {"received", true},
                                                      {"amount_sat", 29000}, {"confirmations", 12}}});
        Find<QLineEdit>(tab_, "bitcoinSendTo")->setText("bc1qdestination");
        Find<QLineEdit>(tab_, "bitcoinSendAmount")->setText("0.0002");
        Find<QPushButton>(tab_, "bitcoinReview")->click();
        Q_EMIT rpc_->rpcResult("portfolio.btcpreview", kPreview);
        QApplication::processEvents();
        tab_->grab().save(dir + "/portfolio-tab.png");
    }

private:
    static QString appPageStyleForTest();
};

#include "chromestyle.h"
QString PortfolioTabTest::appPageStyleForTest() { return appPageStyle(); }

QTEST_MAIN(PortfolioTabTest)
#include "test_portfolio_tab.moc"
