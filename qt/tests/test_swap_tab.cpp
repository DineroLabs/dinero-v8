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
#include <QPushButton>
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
    void init() {
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

    // A node that reaches Bitcoin without Bitcoin Core sells DIN for BTC and says it cannot buy.
    void lightBitcoinNodeSellsButDoesNotBuy() {
        auto* notice = Find<QLabel>(tab_, "swapLightNotice");
        auto* review = Find<QPushButton>(tab_, "reviewPasted");
        auto* hint = Find<QLabel>(tab_, "swapPasteHint");
        QVERIFY(!notice->isVisible());  // unknown, or Bitcoin Core: nothing to say
        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc"}}, {"btc_backend", "core"}});
        QVERIFY(!notice->isVisible());
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1o00");
        Find<QLineEdit>(tab_, "btcRefundAddress")->setText(kBtcAddress);
        QVERIFY(review->isEnabled());

        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc"}}, {"btc_backend", "light"}});
        QVERIFY(notice->isVisible());
        QVERIFY2(notice->text().contains("without Bitcoin Core") && notice->text().contains("sell DIN for BTC"), qPrintable(notice->text()));
        QVERIFY2(notice->text().contains("more private"), qPrintable(notice->text()));
        QVERIFY(!review->isEnabled());  // the pasted offer would make this node pay BTC
        QVERIFY2(hint->text().contains("cannot buy DIN with BTC"), qPrintable(hint->text()));
        // The buyer's reply to this node's own offer is still welcome: that is selling.
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1a00");
        QVERIFY(review->isEnabled());
        // Selling is untouched.
        Find<QLineEdit>(tab_, "dinAmount")->setText("10");
        Find<QLineEdit>(tab_, "btcAmount")->setText("0.001");
        Find<QLineEdit>(tab_, "btcAddress")->setText("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2");
        QVERIFY(Find<QPushButton>(tab_, "createOffer")->isEnabled());

        Q_EMIT rpc_->rpcResult("swaptab.caps", QJsonObject{{"chains", QJsonArray{"btc"}}, {"btc_backend", "core"}});
        QVERIFY(!notice->isVisible());
        Find<QPlainTextEdit>(tab_, "pasteIn")->setPlainText("dinswap1o00");
        QVERIFY(review->isEnabled());
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
