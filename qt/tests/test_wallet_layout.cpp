#include <QtTest/QtTest>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include "mainwindow.h"
#include "rpcclient.h"

// The Wallet tab must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in wallet layout test"); }

class WalletLayoutTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QTcpServer endpoint_;

    template <class T>
    static T* named(QWidget& w, const char* name) { return w.findChild<T*>(QString::fromLatin1(name)); }

    std::unique_ptr<MainWindow> makeWindow(int width = 1440, int height = 1000) {
        auto window = std::make_unique<MainWindow>(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        if (auto* rpc = window->findChild<RpcClient*>()) {
            rpc->setDatadir(dir_.path());
            rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort())));
        }
        window->resize(width, height);
        window->show();
        if (!QTest::qWaitForWindowExposed(window.get())) return nullptr;
        openTab(*window, "Wallet");
        return window;
    }
    static void openTab(MainWindow& window, const QString& name) {
        auto* tabs = window.findChild<QTabWidget*>();
        for (int i = 0; i < tabs->count(); ++i) {
            const QString stored = tabs->tabBar()->tabData(i).toString();
            if ((stored.isEmpty() ? tabs->tabText(i) : stored) == name) tabs->setCurrentIndex(i);
        }
        QCoreApplication::processEvents();
    }
    // The node's wallet.getbalance reply: 636,799.99999731 DIN total in the example
    // the owner saw, 7% of it quantum-safe, 100 DIN maturing, 2.5 DIN unconfirmed.
    static void feedBalance(MainWindow& window) {
        auto* rpc = window.findChild<RpcClient*>();
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"confirmed", 636799.99999731}, {"unconfirmed", 2.5}, {"immature", 100.0},
            {"pq_balance_din", 42900.0}, {"walletname", ""}});
        QCoreApplication::processEvents();
    }

    static bool bindTestWallet(MainWindow& window, const QString& name) {
        QTimer::singleShot(0, [] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* message = qobject_cast<QMessageBox*>(widget)) message->accept();
            }
        });
        return QMetaObject::invokeMethod(&window, "onWalletCreated", Qt::DirectConnection,
            Q_ARG(QString, name), Q_ARG(QString, QStringLiteral("test-fingerprint")),
            Q_ARG(bool, false));
    }

    static bool hasSelectedWallet(MainWindow& window, const QString& name) {
        for (const auto* selector : window.findChildren<QComboBox*>()) {
            if (selector->currentText() == name && selector->findText(name) >= 0) return true;
        }
        return false;
    }

    static bool hasLabelText(MainWindow& window, const QString& text) {
        for (const auto* label : window.findChildren<QLabel*>()) {
            if (label->text() == text) return true;
        }
        return false;
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        QVERIFY(endpoint_.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint_.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroWalletLayoutTest");
        QCoreApplication::setApplicationName("IsolatedWalletLayout");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings().setValue("updates/check_enabled", false);
    }

    void balanceReadsWithThousandsSeparators() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("636,799.99999731 DIN"));
        QCOMPARE(named<QLabel>(*window, "lblTransparentTaprootBalance")->text(), QString("593,899.99999731 DIN"));
        QCOMPARE(named<QLabel>(*window, "lblTransparentP2mrBalance")->text(), QString("42,900.00000000 DIN"));
    }

    void recoveryRefusalDoesNotReplaceKnownBalanceWithZero() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        auto* transparent = named<QLabel>(*window, "lblTransparentTaprootBalance");
        auto* maturing = named<QLabel>(*window, "lblImmature");
        QVERIFY(total && transparent && maturing);
        const QString priorTotal = total->text();
        const QString priorTransparent = transparent->text();
        const QString priorMaturing = maturing->text();

        // Core returns financial-read refusals inside the RPC result object.
        // A missing amount must never be interpreted as a real zero balance.
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"error", "Balance query error: orchard_wallet_delivery_pending"}});
        Q_EMIT rpc->rpcResult("wallet.gettotalbalance", QJsonObject{
            {"error", "Balance query error: orchard_wallet_delivery_pending"}});
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), priorTotal);
        QCOMPARE(transparent->text(), priorTransparent);
        QCOMPARE(maturing->text(), priorMaturing);

        // A complete numeric response with an actual zero is still accepted.
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", ""}, {"confirmed", 0.0},
            {"unconfirmed", 0.0}, {"immature", 0.0}});
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), QString("0.00000000 DIN"));
    }

    void recoveryRefusalBeforeFirstBalanceIsUnavailable() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"error", "Balance query error: orchard_wallet_delivery_pending"}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("Balance unavailable"));
    }

    void productionBalanceRpcErrorMarksVerifiedAmountStaleAndDisablesSend() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        window->checkRescanStatus();
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"hd_enabled", true}, {"wallet_name", "RecoveryTestWallet"},
            {"unlocked", true}, {"scanning", false}});
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 5.0}, {"send_ready", true}});
        QCoreApplication::processEvents();
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        auto* maturing = named<QLabel>(*window, "lblImmature");
        auto* scope = named<QLabel>(*window, "walletBalanceScope");
        auto* send = named<QPushButton>(*window, "walletSendButton");
        QVERIFY(total && maturing && scope && send);
        QCOMPARE(total->text(), QString("12.00000000 DIN"));
        QCOMPARE(maturing->text(), QString("5.00000000 DIN"));
        QVERIFY(window->walletSendReady_);
        QVERIFY(send->isEnabled());

        // The live HTTP dispatcher promotes wallet_balance_not_current to
        // this top-level RPC error, rather than delivering an error-shaped
        // rpcResult object to the existing balance handler.
        Q_EMIT rpc->rpcError("wallet.getbalance", -32603,
            "Legacy transparent wallet is catching up to the selected chain");
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), QString("12.00000000 DIN"));
        QCOMPARE(maturing->text(), QString("5.00000000 DIN"));
        QVERIFY(!scope->isHidden());
        QVERIFY(scope->text().contains("Last verified"));
        QVERIFY(scope->text().contains("UTC"));
        QVERIFY(scope->text().contains("waiting for current wallet balance"));
        QVERIFY(!window->walletSendReady_);
        QVERIFY(!send->isEnabled());

        // Other wallet-scoped polling must not rewrite a stale display.
        Q_EMIT rpc->rpcResult("wallet.shieldedbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"balance_una", 200000000.0},
            {"spend_enabled", true}});
        Q_EMIT rpc->rpcResult("wallet.gettotalbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"total_balance", 1000.0}});
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), QString("12.00000000 DIN"));

        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 13.0},
            {"unconfirmed", 0.0}, {"immature", 6.0}, {"send_ready", true}});
        QCoreApplication::processEvents();
        QVERIFY(scope->isHidden());
        QVERIFY(send->isEnabled());
        QCOMPARE(maturing->text(), QString("6.00000000 DIN"));

        QVERIFY(bindTestWallet(*window, "Other"));
        QCOMPARE(total->text(), QString("Loading..."));
        Q_EMIT rpc->rpcError("wallet.getbalance", -32603,
            "Legacy transparent wallet is catching up to the selected chain");
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), QString("Balance unavailable"));
        QVERIFY(scope->isHidden());
        QVERIFY(!send->isEnabled());
    }

    void lateOldWalletReplyCannotBeAcceptedAfterSwitch() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 5.0}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("12.00000000 DIN"));
        QCOMPARE(named<QLabel>(*window, "lblImmature")->text(), QString("5.00000000 DIN"));

        QVERIFY(bindTestWallet(*window, "Other"));
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("Loading..."));
        QCOMPARE(named<QLabel>(*window, "lblImmature")->text(), QString("Loading..."));
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 99.0},
            {"unconfirmed", 0.0}, {"immature", 0.0}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("Loading..."));
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "Other"}, {"confirmed", 7.0},
            {"unconfirmed", 0.0}, {"immature", 0.0}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("7.00000000 DIN"));
        Q_EMIT rpc->rpcResult("wallet.shieldedbalance", QJsonObject{
            {"walletname", "Other"}, {"balance_una", 200000000.0},
            {"spend_enabled", true}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("9.00000000 DIN"));
        // A chain-level error without wallet identity may be an old reply.
        Q_EMIT rpc->rpcResult("wallet.shieldedbalance", QJsonObject{
            {"error", "shielded_not_active"}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("9.00000000 DIN"));
        Q_EMIT rpc->rpcResult("wallet.shieldedbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"balance_una", 18000000000.0},
            {"spend_enabled", true}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("9.00000000 DIN"));
    }

    void incompleteShieldedReplyCannotEraseKnownAmount() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        Q_EMIT rpc->rpcResult("wallet.shieldedbalance", QJsonObject{
            {"walletname", ""}, {"balance_una", 18000000000.0},
            {"spend_enabled", true}});
        QCoreApplication::processEvents();
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        QVERIFY(total);
        const QString priorTotal = total->text();
        QVERIFY(priorTotal.contains("636,979."));

        Q_EMIT rpc->rpcResult("wallet.shieldedbalance", QJsonObject{});
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), priorTotal);
    }

    void pendingWalletInfoDoesNotUnloadOrLockSelectedWallet() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        QVERIFY(hasSelectedWallet(*window, "RecoveryTestWallet"));
        // refresh attaches the normal wallet.getinfo rescan/status listener.
        QVERIFY(QMetaObject::invokeMethod(window.get(), "refresh", Qt::DirectConnection));
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"error", "wallet_balance_not_current"}, {"retryable", true},
            {"walletname", "RecoveryTestWallet"}});
        QCoreApplication::processEvents();
        QVERIFY(hasSelectedWallet(*window, "RecoveryTestWallet"));
        QVERIFY(!hasLabelText(*window, "No wallet loaded"));
    }

    void transparentOnlyBalanceCannotEnableSend() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        window->checkRescanStatus();
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"hd_enabled", true}, {"wallet_name", "RecoveryTestWallet"},
            {"unlocked", true}, {"scanning", false}});
        QCoreApplication::processEvents();
        auto* send = named<QPushButton>(*window, "walletSendButton");
        QVERIFY(send);

        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 5.0},
            {"wallet_scope", "legacy_transparent_only"},
            {"send_ready", false},
            {"send_unavailable_reason", "Orchard wallet enrollment is required before sending"}});
        QCoreApplication::processEvents();
        QCOMPARE(named<QLabel>(*window, "walletTotalBalance")->text(), QString("12.00000000 DIN"));
        auto* scope = named<QLabel>(*window, "walletBalanceScope");
        QVERIFY(scope);
        QVERIFY(!scope->isHidden());
        QVERIFY(scope->text().contains("Sending paused"));
        QVERIFY(!send->isEnabled());
        QVERIFY(send->toolTip().contains("Orchard wallet enrollment"));

        // A later enrolled response can restore normal send controls without
        // changing the selected wallet or its displayed balance.
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 5.0}, {"send_ready", true}});
        QCoreApplication::processEvents();
        QVERIFY(send->isEnabled());
        QVERIFY(scope->isHidden());
    }

    void maxStillSendsTheWholeBalance() {
        // Max reads the balance back from its label; separators must not change the amount.
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        openTab(*window, "Send");
        auto* max = named<QPushButton>(*window, "sendMax");
        QVERIFY(max);
        max->setEnabled(true);
        QTest::mouseClick(max, Qt::LeftButton);
        QCOMPARE(named<QLineEdit>(*window, "sendAmount")->text(), QString("636799.99998731"));
    }

    void columnsAndNotYetSpendable() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        auto* publicColumn = named<QWidget>(*window, "walletPublicColumn");
        auto* privateColumn = named<QWidget>(*window, "walletPrivateColumn");
        auto* pendingRow = named<QWidget>(*window, "walletNotYetSpendable");
        QVERIFY(publicColumn && privateColumn && pendingRow);
        // Public and private side by side, like the Overview's columns.
        const QPoint pub = publicColumn->mapTo(window.get(), QPoint(0, 0));
        const QPoint priv = privateColumn->mapTo(window.get(), QPoint(0, 0));
        QCOMPARE(pub.y(), priv.y());
        QVERIFY(priv.x() > pub.x() + publicColumn->width() - 1);
        // Maturing mining rewards and unconfirmed funds are not shielded money.
        auto* mining = named<QLabel>(*window, "lblImmature");
        auto* pending = named<QLabel>(*window, "lblUnconfirmed");
        QVERIFY(mining && pending);
        QVERIFY(!privateColumn->isAncestorOf(mining));
        QVERIFY(!privateColumn->isAncestorOf(pending));
        QVERIFY(pendingRow->isAncestorOf(mining) && pendingRow->isAncestorOf(pending));
        QCOMPARE(mining->text(), QString("100.00000000 DIN"));
        QCOMPARE(pending->text(), QString("+2.50000000 DIN"));
        // Amounts line up on the right edge of their column.
        QVERIFY(named<QLabel>(*window, "lblTransparentTaprootBalance")->alignment() & Qt::AlignRight);
        QVERIFY(named<QLabel>(*window, "lblShieldedBalance")->alignment() & Qt::AlignRight);
    }

    void quantumSafeShareIsANudgeNotAnAlarm() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);  // 7% quantum-safe
        auto* bar = window->findChild<QProgressBar*>("walletPqBar");
        QVERIFY(bar);
        QCOMPARE(bar->value(), 7);
        QVERIFY2(!bar->styleSheet().contains("#c0392b"), "the low share is still painted alarm red");
        QVERIFY2(bar->styleSheet().contains("#f0b429"), qPrintable(bar->styleSheet()));
    }

    void seedExplainerIsOneLineUntilAsked() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* details = named<QLabel>(*window, "walletSeedDetails");
        auto* about = named<QPushButton>(*window, "walletSeedAbout");
        QVERIFY(details && about);
        QVERIFY(!details->isVisible());
        QTest::mouseClick(about, Qt::LeftButton);
        QVERIFY(details->isVisible());
        bool backup = false;
        for (auto* b : window->findChildren<QPushButton*>())
            if (b->text().contains("Seed Backup") && b->isVisible()) backup = true;
        QVERIFY2(backup, "Seed Backup / Mobile Restore must stay one click away");
    }

    void walletButtonsShareOneRowAndSize() {
        auto window = makeWindow();
        QVERIFY(window);
        QList<QPushButton*> buttons;
        for (const char* name : {"walletLoad", "walletLock", "walletEncrypt", "walletRescan", "walletCreate"}) {
            auto* b = named<QPushButton>(*window, name);
            QVERIFY2(b, name);
            if (b->isVisible()) buttons << b;  // Load is hidden in some wallet states
        }
        QVERIFY(buttons.size() >= 4);
        const int y = buttons.first()->mapTo(window.get(), QPoint(0, 0)).y();
        const int w = buttons.first()->width();
        for (auto* b : buttons) {
            QCOMPARE(b->mapTo(window.get(), QPoint(0, 0)).y(), y);
            QVERIFY2(qAbs(b->width() - w) <= 2,
                     qPrintable(QString("'%1' is %2 px, others %3 px").arg(b->text()).arg(b->width()).arg(w)));
        }
    }

    void noEmptyStripsOrStretchedBoxes() {
        auto window = makeWindow(1440, 2000);  // much taller than the content
        QVERIFY(window);
        feedBalance(*window);
        auto* assets = named<QLabel>(*window, "lblAssets");
        QVERIFY(assets);
        QVERIFY2(!assets->isVisible(), "an empty assets line still draws a dark strip");
        for (auto* box : window->findChildren<QGroupBox*>()) {
            if (!box->isVisible()) continue;
            const QString title = box->title();
            if (!title.contains("Balance") && !title.contains("Receive Address") && !title.contains("HD Wallet"))
                continue;
            QVERIFY2(box->height() <= box->sizeHint().height() + 4,
                     qPrintable(QString("%1 is %2 px tall, needs %3").arg(title).arg(box->height())
                                    .arg(box->sizeHint().height())));
        }
    }
};
QTEST_MAIN(WalletLayoutTest)
#include "test_wallet_layout.moc"
