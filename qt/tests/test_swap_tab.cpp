// The Swap tab follows the app's design: chrome buttons with hover text, the
// app palette, "To continue:" hints instead of buttons that fail, results
// hidden until there is one, a form beside its live review at 3:2, and spare
// height below the content. Offline: no node answers, nothing is sent.
#include <QtTest/QtTest>

#include <QApplication>
#include <QGroupBox>
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

        // The final review before anything is created (Cancel is the default).
        QTimer::singleShot(300, [dir] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->grab().save(dir + "/swap-review-dialog.png");
                box->reject();
            }
        });
        create->click();
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
