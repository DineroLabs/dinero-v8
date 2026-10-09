// The real Swap tab, offscreen, against two regtest dinerods (Alice's and Bob's)
// started by tests/swap_widget_regtest.sh. Every step goes through the widget:
// typing, buttons, the review dialogs (accepted by a helper), copy-paste of the
// offer and accept between the two tabs.
//   SWAP_QT_MODE=setup  : offer -> accept -> start, until both tables list the swap
//   SWAP_QT_MODE=verify : both tables show the swap as Done
// Env: SWAP_QT_{ALICE,BOB}_{PORT,DIR}, SWAP_QT_ALICE_BTC, SWAP_QT_BOB_BTC_REFUND.
#include <QtTest/QtTest>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>

#include "chromestyle.h"
#include "rpcclient.h"
#include "swapwidget.h"

namespace {
QString Env(const char* k) { return qEnvironmentVariable(k); }

RpcClient* Client(const QString& port, const QString& dir, QObject* parent) {
    auto* c = new RpcClient(parent);
    c->setDatadir(dir);
    c->loadCookie();  // the test datadir holds a .cookie, checked first
    c->setEndpoint(QUrl("http://127.0.0.1:" + port + "/"));
    return c;
}

template <typename T>
T* Find(QWidget* w, const char* name) {
    auto* c = w->findChild<T*>(name);
    if (!c) qFatal("missing widget %s", name);
    return c;
}

// Optional evidence: SWAP_QT_SHOTS=<dir> saves what each tab shows.
void Shot(QWidget* w, const QString& name) {
    const QString dir = qEnvironmentVariable("SWAP_QT_SHOTS");
    if (dir.isEmpty()) return;
    w->setStyleSheet(appPageStyle());  // as inside the app's window
    w->resize(1440, 900);
    QApplication::processEvents();
    w->grab().save(dir + "/" + name + ".png");
}

QString RowState(QWidget* w) {
    auto* t = Find<QTableWidget>(w, "swapTable");
    return t->rowCount() == 1 && t->item(0, 2) ? t->item(0, 2)->text() : QString();
}
}  // namespace

class SwapWidgetRegtest : public QObject {
    Q_OBJECT
    SwapWidget* alice_ = nullptr;
    SwapWidget* bob_ = nullptr;
    QTimer dialogs_;
    QStringList answered_;

private Q_SLOTS:
    void initTestCase() {
        alice_ = new SwapWidget(Client(Env("SWAP_QT_ALICE_PORT"), Env("SWAP_QT_ALICE_DIR"), this));
        bob_ = new SwapWidget(Client(Env("SWAP_QT_BOB_PORT"), Env("SWAP_QT_BOB_DIR"), this));
        alice_->show();
        bob_->show();
        // Confirm every review dialog with its action button (Cancel is the
        // default there), remembering what it said.
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                answered_ << box->text() + "\n" + box->informativeText();
                for (auto* b : box->buttons()) {
                    if (box->buttonRole(b) == QMessageBox::AcceptRole) return b->click();
                }
                qFatal("review dialog without an action button: %s", qPrintable(box->text()));
            }
        });
        dialogs_.start(50);
    }

    void run() {
        if (Env("SWAP_QT_MODE") == "verify") {
            for (auto* w : {alice_, bob_}) Find<QPushButton>(w, "refreshSwaps")->click();
            QTRY_COMPARE_WITH_TIMEOUT(RowState(alice_), QString("Done"), 20000);
            QTRY_COMPARE_WITH_TIMEOUT(RowState(bob_), QString("Done"), 20000);
            Find<QTableWidget>(alice_, "swapTable")->selectRow(0);
            Shot(alice_, "alice-done");
            return;
        }
        // Alice fills the form at once; the button must enable once the tab
        // knows the chain (regtest: bcrt1… addresses).
        Find<QLineEdit>(alice_, "dinAmount")->setText("10");
        Find<QLineEdit>(alice_, "btcAmount")->setText("0.01");
        Find<QLineEdit>(alice_, "btcAddress")->setText(Env("SWAP_QT_ALICE_BTC"));
        QTRY_VERIFY_WITH_TIMEOUT(Find<QPushButton>(alice_, "createOffer")->isEnabled(), 15000);
        Find<QPushButton>(alice_, "createOffer")->click();
        auto* offerOut = Find<QPlainTextEdit>(alice_, "offerOut");
        QTRY_VERIFY_WITH_TIMEOUT(offerOut->toPlainText().startsWith("dinswap1o"), 15000);
        QVERIFY(!answered_.isEmpty() && answered_.last().contains("You sell 10.00000000 DIN for 0.01000000 BTC"));

        // Bob pastes the offer, reviews the decoded terms, accepts.
        Find<QPlainTextEdit>(bob_, "pasteIn")->setPlainText(offerOut->toPlainText());
        Find<QLineEdit>(bob_, "btcRefundAddress")->setText(Env("SWAP_QT_BOB_BTC_REFUND"));
        Find<QPushButton>(bob_, "reviewPasted")->click();
        auto* acceptOut = Find<QPlainTextEdit>(bob_, "acceptOut");
        QTRY_VERIFY_WITH_TIMEOUT(acceptOut->toPlainText().startsWith("dinswap1a"), 15000);
        QVERIFY(answered_.last().contains("You send 0.01000000 BTC and receive 10.00000000 DIN"));

        // Alice pastes Bob's accept and starts the swap.
        Find<QPlainTextEdit>(alice_, "pasteIn")->setPlainText(acceptOut->toPlainText());
        Find<QPushButton>(alice_, "reviewPasted")->click();
        QTRY_VERIFY_WITH_TIMEOUT(Find<QLabel>(alice_, "swapStatus")->text().startsWith("Swap started"), 15000);
        QVERIFY(answered_.last().contains("Starting now locks your DIN"));

        for (auto* w : {alice_, bob_}) Find<QPushButton>(w, "refreshSwaps")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!RowState(alice_).isEmpty() && !RowState(bob_).isEmpty(), 15000);
        // Cancel is offered only before anything is locked.
        Find<QTableWidget>(bob_, "swapTable")->selectRow(0);
        Shot(alice_, "alice-started");
        Shot(bob_, "bob-accepted");
        qInfo() << "alice:" << RowState(alice_) << "| bob:" << RowState(bob_)
                << "| bob cancel enabled:" << Find<QPushButton>(bob_, "cancelSwap")->isEnabled();
    }

    void cleanupTestCase() {
        delete alice_;
        delete bob_;
    }
};

QTEST_MAIN(SwapWidgetRegtest)
#include "test_swap_widget_regtest.moc"
