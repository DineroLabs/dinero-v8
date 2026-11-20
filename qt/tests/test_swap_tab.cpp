// The Swap tab follows the app's design: chrome buttons with hover text, the
// app palette, "To continue:" hints instead of buttons that fail, results
// hidden until there is one, a form beside its live review at 3:2, and spare
// height below the content. Offline: no node answers, nothing is sent.
#include <QtTest/QtTest>

#include <QApplication>
#include <QFrame>
#include <QGroupBox>
#include <QJsonArray>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <QToolTip>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>

#include "chromestyle.h"
#include "rpcclient.h"
#include "swapwidget.h"

namespace {
template <typename T>
T* Find(QWidget* w, const char* name) {
    auto* c = w->findChild<T*>(name);
    if (!c) qFatal("missing widget %s", name);
    return c;
}

// Same chain as a mainnet node: Bitcoin addresses start with bc1.
const char* kBtcAddress = "bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq";
}  // namespace

class SwapTabTest : public QObject {
    Q_OBJECT
    RpcClient* rpc_ = nullptr;
    SwapWidget* tab_ = nullptr;

private Q_SLOTS:
    void initTestCase() {  // the switch is a stored setting: keep the test's apart from a real install's
        QCoreApplication::setOrganizationName("DineroSwapTabTest");
        QCoreApplication::setApplicationName("test_swap_tab");
    }
    void cleanupTestCase() { QSettings().clear(); }
    void init() {
        QSettings().clear();
        rpc_ = new RpcClient(this);
        rpc_->setEndpoint(QUrl("http://127.0.0.1:1/"));  // nobody listens: offline
        tab_ = new SwapWidget(rpc_);
        tab_->resize(1440, 1400);
        tab_->show();
        QVERIFY(QTest::qWaitForWindowExposed(tab_));
    }
    void cleanup() {
        delete tab_;
        tab_ = nullptr;
    }

    void everyButtonHasHoverTextAndTheAppLook() {
        const auto buttons = tab_->findChildren<QPushButton*>();
        QVERIFY(buttons.size() >= 6);
        for (auto* b : buttons) {
            QVERIFY2(!b->toolTip().trimmed().isEmpty(), qPrintable("no hover text on " + b->text()));
            QVERIFY2(b->styleSheet().contains("border-radius: 7px"), qPrintable("not a chrome button: " + b->text()));
        }
    }

    void usesTheAppColours() {
        const QStringList banned{"#e0a64a", "#9fd59f", "#4CAF50", "#FF9800", "#2196F3", "#f44336", "#999;"};
        Find<QLineEdit>(tab_, "dinAmount")->setText("abc");  // a hint is showing
        for (auto* w : tab_->findChildren<QWidget*>()) {
            QString text = w->styleSheet();
            if (auto* l = qobject_cast<QLabel*>(w)) text += l->text();
            for (const auto& c : banned) {
                QVERIFY2(!text.contains(c, Qt::CaseInsensitive), qPrintable(w->objectName() + " uses " + c));
            }
        }
    }

    // Every second row of a table is painted with the palette's alternate
    // colour: light, unless the table says otherwise, under the app's pale text.
    void tablesKeepTheirRowsDark() {
        const auto tables = tab_->findChildren<QTableWidget*>();
        QVERIFY(!tables.isEmpty());
        for (auto* t : tables) {
            if (!t->alternatingRowColors()) continue;
            QVERIFY2(t->styleSheet().contains("alternate-background-color: #2"),
                     qPrintable(t->objectName() + " has light alternate rows"));
        }
    }

    void resultsStayHiddenUntilThereIsOne() {
        for (const char* name : {"offerOut", "acceptOut", "swapEvents"}) {
            QVERIFY2(!Find<QPlainTextEdit>(tab_, name)->isVisible(), name);
        }
        for (const char* name : {"copyOffer", "copyAccept"}) {
            QVERIFY2(!Find<QPushButton>(tab_, name)->isVisible(), name);
        }
        QVERIFY(!Find<QTableWidget>(tab_, "swapTable")->isVisible());
        auto* empty = Find<QLabel>(tab_, "swapListEmpty");
        QVERIFY(empty->isVisible());
        QVERIFY(empty->text().startsWith("No swaps yet"));
    }

    void buttonsSayWhatIsMissing() {
        auto* create = Find<QPushButton>(tab_, "createOffer");
        auto* hint = Find<QLabel>(tab_, "swapOfferHint");
        QVERIFY(!create->isEnabled());
        QVERIFY2(hint->text().startsWith("To continue: "), qPrintable(hint->text()));
        Find<QLineEdit>(tab_, "dinAmount")->setText("10");
        Find<QLineEdit>(tab_, "btcAmount")->setText("0.001");
        Find<QLineEdit>(tab_, "btcAddress")->setText(kBtcAddress);
        QVERIFY(create->isEnabled());
        QVERIFY2(hint->text().startsWith("Ready."), qPrintable(hint->text()));

        auto* review = Find<QPushButton>(tab_, "reviewPasted");
        auto* pasteHint = Find<QLabel>(tab_, "swapPasteHint");
        QVERIFY(!review->isEnabled());
        QVERIFY(pasteHint->text().startsWith("To continue: "));
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("hello");
        QVERIFY(!review->isEnabled());
        QVERIFY2(pasteHint->text().contains("not a swap offer"), qPrintable(pasteHint->text()));
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1o00");  // an offer needs a refund address
        QVERIFY(!review->isEnabled());
        QVERIFY2(pasteHint->text().contains("refund address"), qPrintable(pasteHint->text()));
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText(kBtcAddress);
        QVERIFY(review->isEnabled());
        // A 1… address from an exchange is as good; a mistyped one is not.
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN3");
        QVERIFY(!review->isEnabled());
        QVERIFY2(pasteHint->text().contains("bc1, 1 or 3"), qPrintable(pasteHint->text()));
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2");
        QVERIFY(review->isEnabled());
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText(kBtcAddress);
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1a00");  // a reply needs nothing more
        Find<QLineEdit>(tab_, "btcRefundAddress")->clear();
        QVERIFY(review->isEnabled());
    }

    void reviewShowsTheTermsOrADash() {
        auto* sell = Find<QLabel>(tab_, "swapReviewSell");
        auto* rate = Find<QLabel>(tab_, "swapReviewRate");
        QCOMPARE(sell->text(), QString::fromUtf8("—"));
        Find<QLineEdit>(tab_, "dinAmount")->setText("10");
        Find<QLineEdit>(tab_, "btcAmount")->setText("0.001");
        Find<QLineEdit>(tab_, "btcAddress")->setText(kBtcAddress);
        QCOMPARE(sell->text(), QString("10.00000000 DIN"));
        QCOMPARE(Find<QLabel>(tab_, "swapReviewReceive")->text(), QString("0.00100000 BTC"));
        QVERIFY(rate->text().contains("sat per DIN"));
    }

    void formAndReviewShareARow() {
        for (const int width : {1100, 1440, 1920}) {
            tab_->resize(width, 1400);
            QApplication::processEvents();
            auto* form = Find<QGroupBox>(tab_, "swapSellForm");
            auto* review = Find<QGroupBox>(tab_, "swapOfferReview");
            const QPoint f = form->mapTo(tab_, QPoint(0, 0)), r = review->mapTo(tab_, QPoint(0, 0));
            QVERIFY2(r.x() > f.x() + form->width() - 1, qPrintable(QString("review not beside form at %1").arg(width)));
            QVERIFY2(qAbs(r.y() - f.y()) <= 20, qPrintable(QString("tops differ at %1").arg(width)));
            const double share = double(form->width()) / double(form->width() + review->width());
            QVERIFY2(share > 0.57 && share < 0.63, qPrintable(QString("form share %1 at %2").arg(share).arg(width)));
        }
    }

    void shotsForReview() {
        const QString dir = qEnvironmentVariable("SWAP_TAB_SHOTS");
        if (dir.isEmpty()) QSKIP("set SWAP_TAB_SHOTS=<dir> to save pictures");
        tab_->setStyleSheet(appPageStyle());  // as inside the app's window
        Find<QLabel>(tab_, "swapStatus")->setVisible(false);  // "no node" in this offline test
        tab_->resize(1440, 900);
        QApplication::processEvents();
        auto quiet = [this] { Find<QLabel>(tab_, "swapStatus")->setVisible(false); QApplication::processEvents(); };
        quiet();
        tab_->grab().save(dir + "/swap-tab-empty.png");

        Find<QLineEdit>(tab_, "dinAmount")->setText("10");
        Find<QLineEdit>(tab_, "btcAmount")->setText("0.001");
        Find<QLineEdit>(tab_, "btcAddress")->setText(kBtcAddress);
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1o00");
        quiet();
        tab_->grab().save(dir + "/swap-tab-ready.png");

        // Hover text over "Create offer", drawn where the tooltip appears.
        auto* create = Find<QPushButton>(tab_, "createOffer");
        const QPoint at = create->mapTo(tab_, QPoint(create->width() / 2, create->height() + 6));
        QToolTip::showText(create->mapToGlobal(QPoint(create->width() / 2, create->height() + 6)), create->toolTip(), create);
        quiet();
        QPixmap page = tab_->grab();
        for (auto* w : QApplication::topLevelWidgets()) {
            if (w->inherits("QTipLabel") && w->isVisible()) {
                QPainter p(&page);
                p.drawPixmap(at, w->grab());
            }
        }
        page.save(dir + "/swap-tab-hover.png");
        QToolTip::hideText();

        // The same tab on a node that can swap ZEC: selling for ZEC, and a buyer's deposit.
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc", "zec"}}});
        Find<QPushButton>(tab_, "chainZec")->click();
        Find<QLineEdit>(tab_, "dinAmount")->setText("100");
        Find<QLineEdit>(tab_, "btcAmount")->setText("2.5");
        Find<QLineEdit>(tab_, "btcAddress")->setText("t1a6nTJkrNCib1gXugKBnNaRh5zAw7UNid5");
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap2o020201 (a ZEC offer: 100 DIN for 2.5 ZEC)");
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText("t1UTHATrzbyAyCLU4Yd5Q2rjcLcKX6EwqgH");
        Q_EMIT rpc_->rpcResult("swaptab.deposit",
                               QJsonObject{{"id", "7c1e9a0400000000"},
                                           {"deposit_address", "t1Y5CHzCc5pGGUJha1jJTBGFh4Qxx2evjdQ"},
                                           {"deposit_due_zat", 250020000}, {"deposit_seen_zat", 250020000},
                                           {"deposit_confirmed_zat", 0}, {"deposit_confirmations", 7}});
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{
            QJsonObject{{"id", "7c1e9a0400000000"}, {"role", "btc-seller"}, {"chain", "zec"}, {"state", "awaiting-deposit"},
                        {"din_amount_una", 10000000000LL}, {"btc_amount_sat", 250000000},
                        {"t_btc_unix", QDateTime::currentSecsSinceEpoch() + 42 * 3600},
                        {"t_din_unix", QDateTime::currentSecsSinceEpoch() + 90 * 3600},
                        {"deposit_address", "t1Y5CHzCc5pGGUJha1jJTBGFh4Qxx2evjdQ"}, {"deposit_due_zat", 250020000},
                        {"deposit_seen_zat", 250020000}, {"deposit_confirmed_zat", 0}, {"deposit_confirmations", 7}},
            QJsonObject{{"id", "a90c55e300000000"}, {"role", "din-seller"}, {"chain", "btc"}, {"state", "done"},
                        {"din_amount_una", 25000000000LL}, {"btc_amount_sat", 420000}}});
        tab_->resize(1440, 1350);
        quiet();
        tab_->grab().save(dir + "/swap-tab-zec.png");
        tab_->resize(1440, 900);

        // The final review before anything is created (Cancel is the default).
        QTimer::singleShot(300, [dir] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->grab().save(dir + "/swap-review-dialog.png");
                box->reject();
            }
        });
        create->click();
    }

    // A failed background refresh is shown, and goes away when the node answers again.
    // (Seen on mainnet: "Connection refused" from the seconds a node was restarting
    // stayed on screen above a list that was loading fine.)
    void aFailedRefreshClearsWhenTheNodeAnswersAgain() {
        auto* status = Find<QLabel>(tab_, "swapStatus");
        Q_EMIT rpc_->rpcError("swaptab.list", -1, "Connection refused");
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), QString("Connection refused"));
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{QJsonObject{{"id", "aa"}, {"state", "offer-sent"}}});
        QVERIFY2(!status->isVisible(), qPrintable(status->text()));
        // What an action answered stays: a refresh does not wipe it.
        Q_EMIT rpc_->rpcError("swaptab.offer", -32603, "not enough DIN");
        QVERIFY(status->isVisible());
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{});
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), QString("not enough DIN"));
        // And a new refresh failure after an action's message is again taken down.
        Q_EMIT rpc_->rpcError("swaptab.list", -1, "Connection refused");
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{});
        QVERIFY(!status->isVisible());
    }

    // --- The "Enable swaps" switch ---
    void theSwitchAsksStoresAndExplains() {
        auto* text = Find<QLabel>(tab_, "swapSwitchText");
        auto* button = Find<QPushButton>(tab_, "swapSwitchButton");
        const QString key = "swap/enabled_v1";
        QVERIFY2(text->text().startsWith("Swaps are off."), qPrintable(text->text()));
        QCOMPARE(button->text(), QString("Turn on swaps"));

        int asked = 0;
        bool answer = false;
        tab_->setEnableConfirmer([&] { ++asked; return answer; });
        button->click();  // asked, declined: nothing changes
        QCOMPARE(asked, 1);
        QVERIFY(!QSettings().value(key, false).toBool());
        QCOMPARE(button->text(), QString("Turn on swaps"));

        answer = true;
        button->click();
        QCOMPARE(asked, 2);
        QVERIFY(QSettings().value(key, false).toBool());
        QCOMPARE(button->text(), QString("Turn off swaps"));
        QVERIFY2(text->text().contains("next time Dinero starts"), qPrintable(text->text()));

        // The node was started for swaps but refused the settings: the tab says why, in plain words.
        // (A running node answers swap.list with RPC error -32603 while swaps are off.)
        Q_EMIT rpc_->rpcError("swaptab.list", -32603,
                              "swaps are disabled: swap.btc_backend=light needs a dinerod built with "
                              "DINERO_SWAP_LIGHT_BTC");
        QVERIFY2(text->text().contains("cannot start here") && text->text().contains("built without them"),
                 qPrintable(text->text()));
        QVERIFY2(!Find<QLabel>(tab_, "swapStatus")->isVisible(), "no second, technical message");

        // Swaps running, one still open: they cannot be turned off under it.
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{QJsonObject{{"id", "aa"}, {"state", "din-locked"}}});
        QVERIFY2(text->text().contains("still open"), qPrintable(text->text()));
        QVERIFY(!button->isEnabled());
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{QJsonObject{{"id", "aa"}, {"state", "done"}}});
        QCOMPARE(text->text(), QString("Swaps are on."));
        QVERIFY(button->isEnabled());

        // The same refusal arriving inside a result is read the same way.
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonObject{{"error", "swaps are disabled: swap.btc_backend must be core or light"}});
        QVERIFY2(text->text().contains("cannot start here: swap.btc_backend must be core or light"), qPrintable(text->text()));
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{});
        QCOMPARE(text->text(), QString("Swaps are on."));

        button->click();  // turning off needs no question
        QCOMPARE(asked, 2);
        QVERIFY(!QSettings().value(key, false).toBool());
        QVERIFY2(text->text().contains("until Dinero is restarted"), qPrintable(text->text()));
    }

    // --- Zcash: only when the node says it can (swap.capabilities) ---
    void zecIsHiddenUnlessTheNodeOffersIt() {
        QVERIFY(!Find<QPushButton>(tab_, "chainZec")->isVisible());  // offline / older node: BTC only
        QVERIFY(!Find<QLabel>(tab_, "swapIntro")->text().contains("ZEC"));
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc"}}});
        QVERIFY(!Find<QPushButton>(tab_, "chainZec")->isVisible());  // a node built without Zcash swaps
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"error", "swaps are disabled"}});
        QVERIFY(!Find<QPushButton>(tab_, "chainZec")->isVisible());
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc", "zec"}}});
        QVERIFY(Find<QPushButton>(tab_, "chainZec")->isVisible());
        QVERIFY(Find<QLabel>(tab_, "swapIntro")->text().contains("BTC or ZEC"));
    }

    // A node that reaches Bitcoin without Bitcoin Core says so, and buys through a deposit address.
    void lightBitcoinNodePaysFromADeposit() {
        auto* notice = Find<QLabel>(tab_, "swapLightNotice");
        auto* review = Find<QPushButton>(tab_, "reviewPasted");
        auto* hint = Find<QLabel>(tab_, "swapPasteHint");
        auto* box = Find<QFrame>(tab_, "swapDeposit");
        QVERIFY(!notice->isVisible());  // unknown, or Bitcoin Core: nothing to say
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc"}}, {"btc_backend", "core"}});
        QVERIFY(!notice->isVisible());
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1o00");
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText(kBtcAddress);
        QVERIFY(review->isEnabled());
        QVERIFY2(hint->text().contains("before you accept"), qPrintable(hint->text()));  // Bitcoin Core pays directly

        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc"}}, {"btc_backend", "light"}});
        QVERIFY(notice->isVisible());
        QVERIFY2(notice->text().contains("without Bitcoin Core") && notice->text().contains("deposit address"), qPrintable(notice->text()));
        QVERIFY2(notice->text().contains("more private"), qPrintable(notice->text()));
        QVERIFY(review->isEnabled());  // buying works here too, by another road
        QVERIFY2(hint->text().contains("then the deposit to pay"), qPrintable(hint->text()));
        Find<QLineEdit>(tab_, "btcRefundAddress")->clear();
        QVERIFY(!review->isEnabled());  // the refund address is where an unused deposit returns
        QVERIFY2(hint->text().contains("refund address"), qPrintable(hint->text()));

        // The node's answer to swap.deposit: the box, in Bitcoin's terms.
        QVERIFY(!box->isVisible());
        QJsonObject d{{"id", "ab12"}, {"deposit_address", "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"}, {"deposit_coin", "BTC"},
                      {"confirmations_needed", 2}, {"deposit_due_zat", 1003000}, {"deposit_seen_zat", 0},
                      {"deposit_confirmed_zat", 0}, {"deposit_confirmations", 0}};
        Q_EMIT rpc_->rpcResult("swaptab.deposit", d);
        QVERIFY(box->isVisible());
        QVERIFY2(Find<QLabel>(tab_, "swapDepositTitle")->text().contains("any Bitcoin wallet or exchange"),
                 qPrintable(Find<QLabel>(tab_, "swapDepositTitle")->text()));
        QCOMPARE(Find<QLabel>(tab_, "swapDepositAddress")->text(), QString("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"));
        // Exchanges offer several networks and default to the wrong one: the box says which.
        const QString network = Find<QLabel>(tab_, "swapDepositNetwork")->text();
        QVERIFY2(network.contains("<b>Bitcoin</b> network only") && network.contains("not Lightning") &&
                     network.contains("Ethereum") && network.contains("may be lost"),
                 qPrintable(network));
        QVERIFY2(Find<QLabel>(tab_, "swapDepositDue")->text().startsWith("0.01003000 BTC"), qPrintable(Find<QLabel>(tab_, "swapDepositDue")->text()));
        QVERIFY2(Find<QLabel>(tab_, "swapDepositDue")->text().contains("what is not used comes back"), "the fee allowance is explained");
        QVERIFY2(Find<QLabel>(tab_, "swapDepositHint")->text().contains("pay 0.01003000 BTC"), qPrintable(Find<QLabel>(tab_, "swapDepositHint")->text()));
        QVERIFY(!Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
        QVERIFY(!Find<QPushButton>(tab_, "withdrawDeposit")->isEnabled());
        QVERIFY2(Find<QLabel>(tab_, "swapStatus")->text().contains("2 confirmations"), qPrintable(Find<QLabel>(tab_, "swapStatus")->text()));

        // Part paid, then all of it one confirmation deep, then two.
        d["deposit_seen_zat"] = 400000;
        d["deposit_confirmations"] = 1;
        Q_EMIT rpc_->rpcResult("swaptab.deposit", d);
        QVERIFY2(Find<QLabel>(tab_, "swapDepositHint")->text().contains("0.00603000 BTC still due"), qPrintable(Find<QLabel>(tab_, "swapDepositHint")->text()));
        QVERIFY(Find<QPushButton>(tab_, "withdrawDeposit")->isEnabled());
        d["deposit_seen_zat"] = 1150000;
        Q_EMIT rpc_->rpcResult("swaptab.deposit", d);
        QVERIFY2(Find<QLabel>(tab_, "swapDepositReceived")->text().contains("1.15000000 BTC") ||
                     Find<QLabel>(tab_, "swapDepositReceived")->text().contains("0.01150000 BTC"),
                 qPrintable(Find<QLabel>(tab_, "swapDepositReceived")->text()));
        QVERIFY2(Find<QLabel>(tab_, "swapDepositReceived")->text().contains("1 of 2 confirmations"), qPrintable(Find<QLabel>(tab_, "swapDepositReceived")->text()));
        QVERIFY2(Find<QLabel>(tab_, "swapDepositHint")->text().contains("1 more Bitcoin confirmation"), qPrintable(Find<QLabel>(tab_, "swapDepositHint")->text()));
        QVERIFY(!Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
        d["deposit_confirmed_zat"] = 1150000;
        d["deposit_confirmations"] = 2;
        Q_EMIT rpc_->rpcResult("swaptab.deposit", d);
        QVERIFY(Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
        QCOMPARE(Find<QProgressBar>(tab_, "swapDepositBar")->maximum(), 2);  // two confirmations fill it, not Zcash's ten
        QCOMPARE(Find<QProgressBar>(tab_, "swapDepositBar")->value(), 2);

        // No server reachable: the address still stands, nothing can be accepted on a guess.
        d["deposit_status"] = "no Bitcoin server reachable";
        Q_EMIT rpc_->rpcResult("swaptab.deposit", d);
        QVERIFY(!Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
        QVERIFY2(Find<QLabel>(tab_, "swapDepositHint")->text().contains("cannot reach any Bitcoin server"), qPrintable(Find<QLabel>(tab_, "swapDepositHint")->text()));

        // Selling is untouched, to a legacy address too.
        Find<QLineEdit>(tab_, "dinAmount")->setText("10");
        Find<QLineEdit>(tab_, "btcAmount")->setText("0.001");
        Find<QLineEdit>(tab_, "btcAddress")->setText("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2");
        QVERIFY(Find<QPushButton>(tab_, "createOffer")->isEnabled());
    }

    void sellingForZec() {
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc", "zec"}}});
        Find<QPushButton>(tab_, "chainZec")->click();
        Find<QLineEdit>(tab_, "dinAmount")->setText("100");
        Find<QLineEdit>(tab_, "btcAmount")->setText("2.5");
        Find<QLineEdit>(tab_, "btcAddress")->setText(kBtcAddress);  // a Bitcoin address does not do for ZEC
        QVERIFY(!Find<QPushButton>(tab_, "createOffer")->isEnabled());
        QVERIFY2(Find<QLabel>(tab_, "swapOfferHint")->text().contains("transparent Zcash address"),
                 qPrintable(Find<QLabel>(tab_, "swapOfferHint")->text()));
        Find<QLineEdit>(tab_, "btcAddress")->setText("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi");  // this node: mainnet
        QVERIFY(Find<QPushButton>(tab_, "createOffer")->isEnabled());
        QCOMPARE(Find<QLabel>(tab_, "swapReviewReceive")->text(), QString("2.50000000 ZEC"));
        QVERIFY(Find<QLabel>(tab_, "swapReviewRate")->text().contains("zat per DIN"));
        Find<QPushButton>(tab_, "chainBtc")->click();  // back to BTC: the same address is refused again
        QVERIFY(!Find<QPushButton>(tab_, "createOffer")->isEnabled());
    }

    void buyingWithZecShowsTheDeposit() {
        auto* review = Find<QPushButton>(tab_, "reviewPasted");
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap2o00");
        QVERIFY(!review->isEnabled());  // this node has not said it can swap ZEC
        QVERIFY(Find<QLabel>(tab_, "swapPasteHint")->text().contains("cannot swap ZEC"));
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc", "zec"}}});
        QVERIFY(!review->isEnabled());
        QVERIFY(Find<QLabel>(tab_, "swapPasteHint")->text().contains("Zcash refund address"));
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi");
        QVERIFY(review->isEnabled());

        auto* box = Find<QFrame>(tab_, "swapDeposit");
        QVERIFY(!box->isVisible());
        Q_EMIT rpc_->rpcResult("swaptab.deposit",
                               QJsonObject{{"id", "7c1e9a0400000000"},
                                           {"deposit_address", "t1Y5CHzCc5pGGUJha1jJTBGFh4Qxx2evjdQ"},
                                           {"deposit_due_zat", 250020000},
                                           {"deposit_seen_zat", 0}});
        QVERIFY(box->isVisible());
        QVERIFY(!Find<QLabel>(tab_, "swapDepositQr")->pixmap().isNull());
        QCOMPARE(Find<QLabel>(tab_, "swapDepositAddress")->text(), QString("t1Y5CHzCc5pGGUJha1jJTBGFh4Qxx2evjdQ"));
        QVERIFY(Find<QLabel>(tab_, "swapDepositHint")->text().contains("pay 2.50020000 ZEC"));
        QVERIFY(!Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
        QVERIFY(!Find<QPushButton>(tab_, "withdrawDeposit")->isEnabled());  // nothing paid yet

        // The list shows the confirmed deposit: the reply can go.
        QJsonObject row{{"id", "7c1e9a0400000000"}, {"role", "btc-seller"}, {"chain", "zec"},
                        {"state", "deposit-confirmed"}, {"din_amount_una", 10000000000LL},
                        {"btc_amount_sat", 250000000}, {"deposit_address", "t1Y5CHzCc5pGGUJha1jJTBGFh4Qxx2evjdQ"},
                        {"deposit_due_zat", 250020000}, {"deposit_seen_zat", 250020000},
                        {"deposit_confirmed_zat", 250020000}, {"deposit_confirmations", 12}};
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{row});
        QVERIFY(Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
        QVERIFY(Find<QPushButton>(tab_, "withdrawDeposit")->isEnabled());
        auto* table = Find<QTableWidget>(tab_, "swapTable");
        QCOMPARE(table->item(0, 4)->text(), QString("2.50000000 ZEC"));
        QVERIFY(table->item(0, 2)->text().startsWith("Deposit confirmed"));

        Q_EMIT rpc_->rpcResult("swaptab.accept", QJsonObject{{"id", "7c1e9a0400000000"}, {"accept", "dinswap1a00"}});
        QVERIFY(!box->isVisible());  // the deposit became the swap
    }

    void aRestartedTabResumesThePendingDeposit() {
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc", "zec"}}});
        auto* box = Find<QFrame>(tab_, "swapDeposit");
        QVERIFY(!box->isVisible());
        QJsonObject row{{"id", "7c1e9a0400000000"}, {"role", "btc-seller"}, {"chain", "zec"},
                        {"state", "deposit-confirmed"}, {"offer", "dinswap2o0102"},
                        {"deposit_address", "t1Y5CHzCc5pGGUJha1jJTBGFh4Qxx2evjdQ"}, {"deposit_due_zat", 250020000},
                        {"deposit_seen_zat", 250020000}, {"deposit_confirmed_zat", 250020000},
                        {"deposit_confirmations", 11}};
        Q_EMIT rpc_->rpcResult("swaptab.list", QJsonArray{row});
        QVERIFY(box->isVisible());  // no swap.deposit in this session: restored from the list
        QVERIFY(Find<QPushButton>(tab_, "acceptDeposit")->isEnabled());
    }

    void aZcashOnlyNodeOffersOnlyZec() {
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"zec"}}});
        QVERIFY(Find<QPushButton>(tab_, "chainZec")->isVisible());
        QVERIFY(!Find<QPushButton>(tab_, "chainBtc")->isVisible());
        QVERIFY(Find<QPushButton>(tab_, "chainZec")->isChecked());
        QCOMPARE(Find<QGroupBox>(tab_, "swapSellForm")->title(), QString("Sell DIN for ZEC"));
    }

    void spareHeightStaysBelowTheContent() {
        tab_->resize(1440, 1400);
        QApplication::processEvents();
        for (auto* box : tab_->findChildren<QGroupBox*>()) {
            QVERIFY2(box->height() <= box->sizeHint().height() + 4,
                     qPrintable(box->objectName() + QString(" stretched to %1 (hint %2)")
                                                        .arg(box->height()).arg(box->sizeHint().height())));
        }
    }
};

QTEST_MAIN(SwapTabTest)
#include "test_swap_tab.moc"
