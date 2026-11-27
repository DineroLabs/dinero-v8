#include <QtTest/QtTest>
#include <QGroupBox>
#include <QTableWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QComboBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QFile>
#include <algorithm>
#include <memory>
#include "mainwindow.h"
#include "connection_manager.h"
#include "rpcclient.h"
#include "walletwizard.h"
#if defined(DIN_ENABLE_ORCHARD_UI) && DIN_ENABLE_ORCHARD_UI
#include "orchardwidget.h"
#include "walletcatchup.h"
#include "walletcreaterefusal.h"
#endif

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
    // A node that can never credit a new wallet says so in its own words;
    // the wizard shows that sentence as it is, calmly, and offers no Retry
    // (waiting does not help). Mainnet 2026-11-24: 1,100 DIN mined to a
    // wallet a Windows node had created and could never show.
    void creationRefusalWordingFollowsTheNodesReason() {
        const QString why = "This node cannot show money in a new wallet: its data was started from an older snapshot. "
                            "No wallet was created.";
        const auto never = WalletCreateRefusal::shown("native_wallet_unservable", why, QString());
        QCOMPARE(never.text, why);
        QVERIFY(!never.retry);
        QVERIFY(never.calm);
        QVERIFY(!never.text.contains("Error generating seed"));
        const auto later = WalletCreateRefusal::shown("snapshot_bootstrap_pending", "x", QString());
        QVERIFY(later.retry && later.calm);
        QVERIFY(later.text.contains("No wallet was created"));
        const auto orchard = WalletCreateRefusal::shown("orchard_history_pending", "x", QString());
        QVERIFY(orchard.retry && orchard.calm);
        const auto broke = WalletCreateRefusal::shown("", "disk full", "could not reopen");
        QVERIFY(!broke.retry && !broke.calm);
        QVERIFY(broke.text.contains("disk full") && broke.text.contains("could not reopen"));
        // A refusal code with no sentence must not leave an empty box.
        QVERIFY(!WalletCreateRefusal::shown("native_wallet_unservable", " ", QString()).text.trimmed().isEmpty());
        const auto transport = WalletCreateRefusal::shownFromTransportError(
            "This node cannot show money in a new wallet: its data was started from an older snapshot. "
            "No wallet was created.", QString());
        QCOMPARE(transport.text, why);
        QVERIFY(transport.calm && !transport.retry);
        QVERIFY(!transport.text.contains("ensure daemon is running", Qt::CaseInsensitive));
    }
    void wizardRetriesOnlyWhilePendingAndOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile cookie(directory.filePath(".cookie"));
        QVERIFY(cookie.open(QIODevice::WriteOnly));
        cookie.write("test:owned-fixture");
        cookie.close();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int walletRequests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (auto* socket = server.nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes]() {
                    bytes->append(socket->readAll());
                    const int split = bytes->indexOf("\r\n\r\n");
                    if (split < 0) return;
                    int length = 0;
                    for (const auto& line : bytes->left(split).split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (bytes->size() < split + 4 + length) return;
                    const auto request = QJsonDocument::fromJson(bytes->mid(split + 4, length)).object();
                    const bool wallet = request.value("method").toString() == "wallet.createhd";
                    if (wallet) ++walletRequests;
                    const QByteArray body = request.value("method").toString() == "blockchain.getsnapshotbootstrapstatus"
                        ? R"({"result":{"orchard_history":{"activation_height":150000,"verified_height":150042,"target_height":150100,"requested":true,"ready":false}},"error":null})"
                        : wallet && walletRequests < 3
                        ? R"({"result":null,"error":{"code":-32603,"message":"Orchard history is not ready; wallet was not created. Retry after history sync.","data":{"wallet_creation":{"error_code":"orchard_history_pending","retryable":true}}}})"
                        : wallet
                        ? R"({"result":{"success":true,"mnemonic":"abandon ability able about above absent absorb abstract absurd abuse access accident","wallet_name":"FreshGateWallet","first_address":"din1pfixture","first_p2mr_address":"din1rfixture","first_orchard_address":"din1sfixture","addresses_ready":true},"error":null})"
                        : R"({"result":142767,"error":null})";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        ConnectionManager manager;
        manager.setCookiePath(cookie.fileName());
        manager.setDaemonUrl(QString("http://127.0.0.1:%1").arg(server.serverPort()));
        manager.setHealthCheckInterval(30000);
        manager.connectToDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::Connected, 3000);
        WalletWizard wizard(&manager, nullptr);
        wizard.setStartId(WalletWizard::Page_CreateSeed);
        wizard.setField("walletName", "FreshGateWallet");
        wizard.show();
        auto* page = qobject_cast<CreateSeedPage*>(wizard.page(WalletWizard::Page_CreateSeed));
        QVERIFY(page);
        auto* retry = page->findChild<QTimer*>("walletCreationPendingRetry");
        QVERIFY(retry);
        auto* progress = page->findChild<QProgressBar*>("orchardHistoryProgress");
        QVERIFY(progress);
        QTRY_VERIFY_WITH_TIMEOUT(progress->isVisible(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(progress->maximum(), 100, 3000);
        QCOMPARE(progress->value(), 42);
        QTRY_VERIFY_WITH_TIMEOUT(retry->isActive(), 3000);
        QCOMPARE(walletRequests, 1);
        retry->setInterval(200);
        QTRY_VERIFY_WITH_TIMEOUT(walletRequests >= 3, 3000);
        QLabel* seedLabel = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (auto* label : page->findChildren<QLabel*>()) {
                if (label->text() == "[Seed hidden. Click Reveal Seed]") {
                    seedLabel = label;
                    return true;
                }
            }
            return false;
        }()), 3000);
        QVERIFY(seedLabel);
        QVERIFY(!retry->isActive());
        QVERIFY(!seedLabel->text().contains("abandon"));
        QPushButton* reveal = nullptr;
        for (auto* button : page->findChildren<QPushButton*>()) {
            if (button->text().contains("Reveal Seed")) reveal = button;
        }
        QVERIFY(reveal);
        QVERIFY(reveal->isEnabled());
        reveal->click();
        QVERIFY(seedLabel->text().contains("abandon"));
        reveal->click();
        QCOMPARE(seedLabel->text(), QString("[Seed hidden. Click Reveal Seed]"));
        wizard.reject();
        QVERIFY(!retry->isActive());
        const int atClose = walletRequests;
        QTest::qWait(300);
        QCOMPARE(walletRequests, atClose);
        manager.disconnectFromDaemon();
    }
    void initTestCase() {
        QVERIFY(dir_.isValid());
        QVERIFY(endpoint_.listen(QHostAddress::LocalHost));
        // Keep the isolated RPC socket open. Tests deliver explicit RPC
        // replies; closing the test socket would trigger connection-loss
        // banners and obscure the wallet state under test during waits.
        connect(&endpoint_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = endpoint_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
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

    void walletWheelScrollsBackAtNormalMouseWheelRate() {
        auto window = makeWindow(1440, 600);
        QVERIFY(window);
        auto* area = qobject_cast<QScrollArea*>(window->findChild<QTabWidget*>()->currentWidget());
        auto* balance = named<QLabel>(*window, "walletTotalBalance");
        QVERIFY(area && balance);
        auto* bar = area->verticalScrollBar();
        QVERIFY(bar && bar->maximum() > 0);
        bar->setValue(bar->maximum());
        const int before = bar->value();
        const QPoint at = balance->rect().center();
        QWheelEvent wheel(at, balance->mapToGlobal(at), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(balance, &wheel);
        const int expected = QApplication::wheelScrollLines() * std::max(1, bar->singleStep());
        QCOMPARE(before - bar->value(), expected);
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

    void briefCatchUpPausesSendingWithoutLeavingAStaleGlobalWarning() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        QSignalBlocker connectionSignals(window->connectionMgr_);
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        auto* scope = named<QLabel>(*window, "walletBalanceScope");
        auto* error = named<QLabel>(*window, "walletGlobalError");
        auto* send = named<QPushButton>(*window, "walletSendButton");
        QVERIFY(total && scope && error && send);
        window->checkRescanStatus();
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"hd_enabled", true}, {"wallet_name", "RecoveryTestWallet"},
            {"unlocked", true}, {"scanning", false}});
        const QJsonObject balance{{"walletname", "RecoveryTestWallet"},
                                  {"confirmed", 12.0}, {"unconfirmed", 0.0},
                                  {"immature", 5.0}, {"send_ready", true}};
        Q_EMIT rpc->rpcResult("wallet.getbalance", balance);
        QVERIFY(send->isEnabled());

        Q_EMIT rpc->rpcError("wallet.getinfo", -32603,
            "Legacy transparent wallet is catching up to the selected chain");
        QVERIFY(!send->isEnabled());
        QVERIFY(window->walletBalanceUnavailable_);
        QCOMPARE(total->text(), QString("12.00000000 DIN"));
        QVERIFY(!scope->isHidden());
        QVERIFY2(error->isHidden() || !error->text().startsWith("Wallet balance unavailable;"),
                 "a one-block catch-up showed the global balance warning immediately");

        Q_EMIT rpc->rpcResult("wallet.getbalance", balance);
        QVERIFY(send->isEnabled());
        QVERIFY(!window->walletBalanceUnavailable_);
        QVERIFY(scope->isHidden());
        QTest::qWait(6100);
        QVERIFY2(error->isHidden() || !error->text().startsWith("Wallet balance unavailable;"),
                 "an old catch-up timer revived the warning after balance recovery");
    }

    void prolongedCatchUpShowsTheWarningAndKeepsSendingPaused() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        QSignalBlocker connectionSignals(window->connectionMgr_);
        auto* error = named<QLabel>(*window, "walletGlobalError");
        auto* send = named<QPushButton>(*window, "walletSendButton");
        QVERIFY(error && send);
        window->checkRescanStatus();
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"hd_enabled", true}, {"wallet_name", "RecoveryTestWallet"},
            {"unlocked", true}, {"scanning", false}});
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 5.0}, {"send_ready", true}});
        QVERIFY(send->isEnabled());
        QVERIFY(window->walletBalanceAccepted_);
        QVERIFY(!window->walletBalanceUnavailable_);
        const qint64 verifiedAt = window->walletBalanceVerifiedAtMs_;
        const QString wallet = window->currentWalletName_;
        Q_EMIT rpc->rpcError("wallet.getbalance", -32603,
            "Legacy transparent wallet is catching up to the selected chain");
        QVERIFY(!send->isEnabled());
        QVERIFY(window->walletBalanceUnavailable_);
        QTest::qWait(7000);
        QVERIFY2(error->isVisible() && error->text().startsWith("Wallet balance unavailable;"),
                 qPrintable(QString("global warning after grace: visible=%1 text='%2' balanceUnavailable=%3 accepted=%4 sameWallet=%5 sameTimestamp=%6")
                     .arg(error->isVisible()).arg(error->text())
                     .arg(window->walletBalanceUnavailable_).arg(window->walletBalanceAccepted_)
                     .arg(window->currentWalletName_ == wallet)
                     .arg(window->walletBalanceVerifiedAtMs_ == verifiedAt)));
        QVERIFY(!send->isEnabled());
    }

    void addressAndHistoryCatchUpStayInTheirPanelsAndClearOnSuccess() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        auto* error = named<QLabel>(*window, "walletGlobalError");
        auto* addresses = named<QLabel>(*window, "addressListStatus");
        auto* history = named<QLabel>(*window, "transactionListStatus");
        QVERIFY(rpc && error && addresses && history);
        const QString catchingUp = "Legacy transparent wallet is catching up to the selected chain";

        Q_EMIT rpc->rpcError("wallet.listaddresses", -32603, catchingUp);
        Q_EMIT rpc->rpcError("wallet.listtransactions", -32603, catchingUp);
        QVERIFY(!addresses->isHidden());
        QVERIFY(!history->isHidden());
        QVERIFY(addresses->text().contains("refresh automatically"));
        QVERIFY(history->text().contains("refresh automatically"));
        QVERIFY(error->isHidden() ||
                (!error->text().contains("wallet.listaddresses") &&
                 !error->text().contains("wallet.listtransactions")));

        Q_EMIT rpc->rpcResult("wallet.listaddresses", QJsonArray{});
        Q_EMIT rpc->rpcResult("wallet.listtransactions", QJsonArray{});
        QVERIFY(addresses->isHidden());
        QVERIFY(history->isHidden());
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

    void recoveredV3ScopeEnablesOnlyPublicSend() {
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
            {"unconfirmed", 0.0}, {"immature", 5.0},
            {"send_ready", true}, {"send_scope", "transparent_recovered_v3"}});
        QVERIFY(window->btnSend_->isEnabled());
        auto* scope = named<QLabel>(*window, "walletBalanceScope");
        QVERIFY(scope);
        QVERIFY(!scope->isHidden());
        QVERIFY(scope->text().contains("Public sends supported"));
        QVERIFY(!window->btnHardwareWalletSend_->isEnabled());
        QVERIFY(!window->btnConsolidate_->isEnabled());
        QVERIFY(!window->btnNewAddress_->isEnabled());
        QVERIFY(!window->btnDeriveAddress_->isEnabled());

        // The balance poll must not re-enable another send mode while the
        // recovered wallet has only the transparent payment capability.
        auto* mode = named<QComboBox>(*window, "sendMode");
        QVERIFY(mode);
        mode->setCurrentIndex(mode->findData("public_contract"));
        QVERIFY(!window->btnSend_->isEnabled());
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 5.0},
            {"send_ready", true}, {"send_scope", "transparent_recovered_v3"}});
        QVERIFY(!window->btnSend_->isEnabled());

        mode->setCurrentIndex(mode->findData("public_transfer"));
        QVERIFY(window->btnSend_->isEnabled());
        Q_EMIT rpc->rpcError("wallet.getbalance", -32603,
            "Legacy transparent wallet is catching up to the selected chain");
        QVERIFY(!window->btnSend_->isEnabled());
    }

#if defined(DIN_ENABLE_ORCHARD_UI) && DIN_ENABLE_ORCHARD_UI
    void recoveredV3ScopeCanSetUpOrchardAccount() {
        auto window = makeWindow();
        QVERIFY(window);
        QVERIFY(bindTestWallet(*window, "RecoveryTestWallet"));
        auto* rpc = window->findChild<RpcClient*>();
        auto* orchard = window->orchardWidget_;
        QVERIFY(rpc && orchard);

        window->checkRescanStatus();
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"hd_enabled", true}, {"wallet_name", "RecoveryTestWallet"},
            {"unlocked", true}, {"scanning", false}});
        Q_EMIT rpc->rpcResult("wallet.getbalance", QJsonObject{
            {"walletname", "RecoveryTestWallet"}, {"confirmed", 12.0},
            {"unconfirmed", 0.0}, {"immature", 0.0},
            {"send_ready", true}, {"send_scope", "transparent_recovered_v3"}});

        QSignalSpy requests(orchard, &OrchardWidget::requestSent);
        orchard->setWalletScope("RecoveryTestWallet");
        orchard->setChain("regtest");
        auto reply = [&](const QString& method, const QJsonObject& value) {
            for (int i = requests.size() - 1; i >= 0; --i) {
                if (requests.at(i).at(0).toString() == method) {
                    return QMetaObject::invokeMethod(orchard, "onRpcResult", Qt::DirectConnection,
                        Q_ARG(QString, requests.at(i).at(2).toString()),
                        Q_ARG(QJsonValue, QJsonValue(value)));
                }
            }
            return false;
        };
        QVERIFY(reply("wallet.orchard.getwalletbinding", QJsonObject{
            {"wallet_name", "RecoveryTestWallet"}, {"wallet_binding", QString(64, 'a')}}));
        QVERIFY(reply("wallet.orchard.listaccounts", QJsonObject{
            {"captured_source_sequence", 3}, {"captured_source_digest", QString(64, 'c')},
            {"accounts", QJsonArray{}}}));
        QVERIFY(reply("orchard.getactivationstatus", QJsonObject{
            {"network", "regtest"}, {"activation_state", "active"},
            {"activation_height", 10}, {"branch_id", 42},
            {"tip_height", 20}, {"tip_hash", QString(64, 'd')},
            {"next_block_height", 21}, {"rule_active_at_tip", true},
            {"rule_active_for_next_block", true}, {"wallet_backend_compiled", true},
            {"storage_mode", "full"}, {"node_supported", true},
            {"node_notice", ""}, {"storage_upgrade_refused", false},
            {"storage_upgrade_reason", "none"}}));
        auto* setup = named<QPushButton>(*orchard, "orchardCreateAccount");
        QVERIFY(setup);
        QVERIFY2(setup->isEnabled(), "Unlocked v3 wallets must be able to set up Orchard account 0");
    }
#endif

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

    // Owner, 2026-11-23: after shielding 25 DIN the Wallet page said
    // "Private: 0.00000000" and the total dropped by 25. That line read the
    // retired pool; Orchard money was not on the page at all.
    void orchardMoneyIsShownAndCountedOnTheWalletPage() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);  // 636,799.99999731 public
        auto* orchard = named<QLabel>(*window, "lblOrchardBalance");
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        auto* retired = named<QLabel>(*window, "lblShieldedBalance");
        auto* retiredCaption = named<QLabel>(*window, "lblRetiredPoolCaption");
        QVERIFY(orchard && total && retired && retiredCaption);
        QVERIFY(named<QWidget>(*window, "walletPrivateColumn")->isAncestorOf(orchard));
        if (!window->findChild<OrchardWidget*>()) QSKIP("built without the Orchard page");
        // Not reported yet: said so, and not counted.
        QCOMPARE(orchard->text(), QString::fromUtf8("Reading…"));
        QCOMPARE(total->text(), QString("636,799.99999731 DIN"));
        // The retired pool is not listed for a wallet that holds nothing in it.
        QVERIFY(retired->isHidden() && retiredCaption->isHidden());

        QVERIFY(QMetaObject::invokeMethod(window.get(), "onOrchardBalanceChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(quint64, quint64(2500000000)), Q_ARG(bool, true), Q_ARG(quint64, quint64(165213))));
        QCOMPARE(orchard->text(), QString("25.00000000 DIN"));
        QCOMPARE(total->text(), QString("636,824.99999731 DIN"));
        QVERIFY(orchard->alignment() & Qt::AlignRight);

        // The node is busy: the figure stays, with its block, and stays counted.
        QVERIFY(QMetaObject::invokeMethod(window.get(), "onOrchardBalanceChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(quint64, quint64(2500000000)), Q_ARG(bool, false), Q_ARG(quint64, quint64(165213))));
        QCOMPARE(orchard->text(), QString("25.00000000 DIN (block 165213)"));
        QCOMPARE(total->text(), QString("636,824.99999731 DIN"));

        // Another wallet / nothing known: never the previous figure.
        QVERIFY(QMetaObject::invokeMethod(window.get(), "onOrchardBalanceChanged", Qt::DirectConnection,
            Q_ARG(bool, false), Q_ARG(quint64, quint64(0)), Q_ARG(bool, false), Q_ARG(quint64, quint64(0))));
        QCOMPARE(orchard->text(), QString::fromUtf8("Reading…"));
        QCOMPARE(total->text(), QString("636,799.99999731 DIN"));
    }

    // Owner, 2026-11-26, v8.2.6: after switching from a wallet holding 25 DIN
    // in Orchard to a legacy wallet whose balance was still catching up, the
    // Wallet page said "Balance unavailable" everywhere except the Orchard
    // line, which still showed the FIRST wallet's 25 DIN. The branch that
    // clears every amount on a switch did not know about the Orchard line.
    void theOrchardLineNeverShowsThePreviousWalletsMoney() {
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        auto* rpc = window->findChild<RpcClient*>();
        auto* orchard = named<QLabel>(*window, "lblOrchardBalance");
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        QVERIFY(rpc && orchard && total);
        if (!window->findChild<OrchardWidget*>()) QSKIP("built without the Orchard page");
        QVERIFY(QMetaObject::invokeMethod(window.get(), "onOrchardBalanceChanged", Qt::DirectConnection,
            Q_ARG(bool, true), Q_ARG(quint64, quint64(2500000000)), Q_ARG(bool, true), Q_ARG(quint64, quint64(165213))));
        QCOMPARE(orchard->text(), QString("25.00000000 DIN"));

        // Another wallet is selected: nothing of the first one stays on screen.
        QVERIFY(bindTestWallet(*window, "Other"));
        QCOMPARE(total->text(), QString("Loading..."));
        QVERIFY2(!orchard->text().contains("25"), qPrintable(orchard->text()));
        QCOMPARE(orchard->text(), QString("Loading..."));

        // Its balance is refused (a legacy wallet still catching up).
        Q_EMIT rpc->rpcError("wallet.getbalance", -32603,
            "Legacy transparent wallet is catching up to the selected chain");
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), QString("Balance unavailable"));
        QVERIFY2(!orchard->text().contains("25"), qPrintable(orchard->text()));
        QCOMPARE(orchard->text(), QString("Balance unavailable"));
        // The Orchard page reporting "nothing known" for the new wallet does
        // not bring the old figure back either.
        QVERIFY(QMetaObject::invokeMethod(window.get(), "onOrchardBalanceChanged", Qt::DirectConnection,
            Q_ARG(bool, false), Q_ARG(quint64, quint64(0)), Q_ARG(bool, false), Q_ARG(quint64, quint64(0))));
        QCOMPARE(orchard->text(), QString("Balance unavailable"));
    }

    // Owner, 2026-11-26, v8.2.6: a legacy wallet that had been closed for a
    // while showed only "Balance unavailable" for about an hour while the
    // node read the blocks it had missed. The node reports how far it is;
    // the page says it, and says nothing it was not told.
    void aWalletThatIsCatchingUpSaysHowFarItIs() {
        using WalletCatchUp::parse;
        const QJsonObject live{{"balance_available", false}, {"blocks_remaining", 10127}, {"error_code", "wallet_balance_not_current"},
            {"is_synced", false}, {"is_syncing", true}, {"last_scanned_height", 158138}, {"target_height", 168265},
            {"wallet_loaded", true}};
        const auto progress = parse(live);
        QVERIFY(progress.has_value());
        QCOMPARE(progress->scanned, quint64(158138));
        QCOMPARE(progress->target, quint64(168265));
        const QString text = WalletCatchUp::banner(*progress);
        QVERIFY2(text.contains("block 158,138 of 168,265"), qPrintable(text));
        QVERIFY2(text.contains("10,127 to go"), qPrintable(text));
        // Not said, or not sensible: no progress is shown.
        const auto without = [&](const char* key) { auto o = live; o.remove(key); return QJsonValue(o); };
        const auto with = [&](const char* key, const QJsonValue& v) { auto o = live; o[key] = v; return QJsonValue(o); };
        QVERIFY(!parse(with("is_syncing", false)));
        QVERIFY(!parse(without("is_syncing")));
        QVERIFY(!parse(with("is_syncing", "true")));
        QVERIFY(!parse(without("last_scanned_height")));
        QVERIFY(!parse(with("last_scanned_height", "158138")));
        QVERIFY(!parse(with("last_scanned_height", 158138.5)));
        QVERIFY(!parse(with("last_scanned_height", -1)));
        QVERIFY(!parse(with("last_scanned_height", 168266)));   // beyond the target
        QVERIFY(!parse(with("target_height", 0)));
        QVERIFY(!parse(with("wallet_loaded", false)));
        QVERIFY(!parse(QJsonValue(QString("syncing"))));

        // On the page: the banner carries it while the balance is refused,
        // the amounts still say "Balance unavailable", and it goes away with
        // the first accepted balance and on a wallet switch.
        auto window = makeWindow();
        QVERIFY(window);
        feedBalance(*window);
        auto* rpc = window->findChild<RpcClient*>();
        auto* total = named<QLabel>(*window, "walletTotalBalance");
        auto* banner = named<QLabel>(*window, "walletGlobalError");
        QVERIFY(rpc && total && banner);
        QVERIFY(bindTestWallet(*window, "Other"));
        Q_EMIT rpc->rpcError("wallet.getbalance", -32603, "Legacy transparent wallet is catching up to the selected chain");
        QCoreApplication::processEvents();
        QCOMPARE(total->text(), QString("Balance unavailable"));
        QVERIFY(!banner->text().contains("catching up"));
        Q_EMIT rpc->rpcResult("wallet.getsyncstate", live);
        QCoreApplication::processEvents();
        QVERIFY2(banner->text().contains("block 158,138 of 168,265"), qPrintable(banner->text()));
        QVERIFY(!banner->isHidden());
        QCOMPARE(total->text(), QString("Balance unavailable"));
        // The node stops saying it: the page stops saying it.
        Q_EMIT rpc->rpcResult("wallet.getsyncstate", with("is_syncing", false));
        QCoreApplication::processEvents();
        QVERIFY2(!banner->text().contains("catching up"), qPrintable(banner->text()));
        Q_EMIT rpc->rpcResult("wallet.getsyncstate", live);
        QCoreApplication::processEvents();
        QVERIFY(banner->text().contains("catching up"));
        // Another wallet: the first one's progress is never shown for it.
        QVERIFY(bindTestWallet(*window, "Third"));
        QVERIFY2(!banner->text().contains("158,138"), qPrintable(banner->text()));
    }

    // Owner, 2026-11-23: the shield row "showed once" and was gone. The page
    // asked for the newest 200 rows only, a miner adds a row a minute, and
    // there was no filter for shields and no way to older rows.
    void historyOffersShieldFiltersAndOlderRows() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        auto* filter = named<QComboBox>(*window, "transactionTypeFilter");
        auto* older = named<QPushButton>(*window, "transactionsShowOlder");
        auto* footer = named<QLabel>(*window, "transactionListFooter");
        QVERIFY(rpc && filter && older && footer);
        QVERIFY(filter->findData("shield") >= 0);
        QVERIFY(filter->findData("unshield") >= 0);

        const auto rows = [](int n) {
            QJsonArray a;
            for (int i = 0; i < n; ++i)
                a.append(QJsonObject{{"txid", QString("%1").arg(i, 64, 16, QChar('0'))}, {"type", "generate"}, {"amount", 100.0},
                                     {"confirmations", i + 1}, {"time", 1795473738 - i * 60}, {"address", "din1ptest"}});
            return a;
        };
        // A full page: older rows may exist.
        Q_EMIT rpc->rpcResult("wallet.listtransactions", rows(200));
        QCoreApplication::processEvents();
        QVERIFY(!older->isHidden());
        QVERIFY2(footer->text().contains("newest 200"), qPrintable(footer->text()));

        older->click();
        QCOMPARE(footer->property("askedCount").toInt(), 600);
        // Fewer rows than asked for: that is everything.
        Q_EMIT rpc->rpcResult("wallet.listtransactions", rows(281));
        QCoreApplication::processEvents();
        QVERIFY(older->isHidden());
        QVERIFY2(footer->text().contains("all 281"), qPrintable(footer->text()));

        // The shield filter asks the node for shields, from the newest again.
        filter->setCurrentIndex(filter->findData("shield"));
        QCOMPARE(footer->property("askedType").toString(), QString("shield"));
        QCOMPARE(footer->property("askedCount").toInt(), 200);
        Q_EMIT rpc->rpcResult("wallet.listtransactions", QJsonArray{QJsonObject{
            {"txid", QString(64, 'a')}, {"type", "shield"}, {"amount", -25.0001}, {"confirmations", 280},
            {"time", 1795473738}, {"address", "dinorch1test"}}});
        QCoreApplication::processEvents();
        auto* table = named<QTableWidget>(*window, "tblTransactions");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QVERIFY2(table->item(0, 2)->text().contains("Shield"), qPrintable(table->item(0, 2)->text()));
        QVERIFY2(footer->text().contains("all 1 shield"), qPrintable(footer->text()));
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

    // Issue #939: a locked, not-yet-enrolled wallet on a full-archive v3 node
    // is reported with its own text. The window must show it as locked, ask
    // for an unlock, and never keep a stale "Unlocked" state.
    void unlockRequiredRefusalShowsLockedWallet() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "Sierra"));
        window->walletUnlocked_ = true;
        window->btnWalletLock_->setText("Unlocked | Lock");
        window->lblErrorMessage_->setVisible(false);
        Q_EMIT rpc->rpcError("wallet.getinfo", -32603,
            "Wallet is locked: unlock it once so this node can finish setting it up. "
            "Addresses and balance appear when setup completes.");
        QCoreApplication::processEvents();
        QVERIFY(!window->walletUnlocked_);
        QVERIFY(window->walletRecoveryUnlockNeeded_);
        QCOMPARE(window->btnWalletLock_->text(), QString("Locked | Unlock"));
        // The user sees the unlock request, not a raw RPC failure.
        QVERIFY(window->lblErrorMessage_->text().contains("Unlock this wallet"));
        QVERIFY(!window->lblErrorMessage_->text().contains("code: -32603"));
    }

    // The Swap tab polls swap.list; a node with swaps off (the default) refuses
    // every time. That refusal belongs to the tab, not to the window's error bar.
    void swapTabRefusalsNeverReachTheErrorBar() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        window->lblErrorMessage_->clear();
        window->lblErrorMessage_->setVisible(false);
        Q_EMIT rpc->rpcError("swaptab.list", -32603,
            "swaps are disabled: start dinerod with swap.enable=1 and swap.btc_rpc=HOST:PORT");
        // (Checked at once: the window's own startup calls fail too in this
        // test and may fill the bar afterwards.)
        QVERIFY2(!window->lblErrorMessage_->text().contains("swap"), qPrintable(window->lblErrorMessage_->text()));
        // Any other failure is still shown.
        Q_EMIT rpc->rpcError("getpeerinfo", -32603, "boom");
        QVERIFY2(window->lblErrorMessage_->text().contains("getpeerinfo failed - boom"),
                 qPrintable(window->lblErrorMessage_->text()));
    }

    // Issue #939 interim: a wallet this v3 node cannot track yet must never
    // trigger the unlock prompt (unlocking cannot help) and must explain why.
    void nativeUnsupportedRefusalNeverAsksForUnlock() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "Sierra"));
        window->walletUnlocked_ = false;
        Q_EMIT rpc->rpcError("wallet.getinfo", -32603,
            "This wallet was created or restored by Dinero 8.2 and this node's wallet index (v3) "
            "cannot track it yet. Your funds are safe on-chain: use its seed phrase in the phone app "
            "or on another node until the next update.");
        QCoreApplication::processEvents();
        QVERIFY(!window->walletRecoveryUnlockNeeded_);
        QCOMPARE(window->lblConnectionStatus_->text(), QString("Wallet not supported on this node yet"));
        QVERIFY(window->lblErrorMessage_->text().contains("cannot track it yet"));
        QVERIFY(!window->lblErrorMessage_->text().contains("Unlock this wallet"));
    }

    // Issue #939: the setup wizard encrypts the new wallet itself. The main
    // window must retire its Encrypt action from that reply (and from an
    // "already encrypted" refusal), not leave it offered.
    void encryptActionRetiredOnceWalletIsEncrypted() {
        auto window = makeWindow();
        QVERIFY(window);
        auto* rpc = window->findChild<RpcClient*>();
        QVERIFY(rpc);
        QVERIFY(bindTestWallet(*window, "Sierra"));
        window->btnEncryptWallet_->setVisible(true);
        Q_EMIT rpc->rpcResult("wallet.encrypt", QJsonObject{{"success", true}});
        QCoreApplication::processEvents();
        QVERIFY(window->btnEncryptWallet_->isHidden());

        window->btnEncryptWallet_->setVisible(true);
        Q_EMIT rpc->rpcError("wallet.encrypt", -15, "Wallet is already encrypted");
        QCoreApplication::processEvents();
        QVERIFY(window->btnEncryptWallet_->isHidden());

        // An unencrypted wallet keeps the action.
        Q_EMIT rpc->rpcResult("wallet.getinfo", QJsonObject{
            {"hd_enabled", true}, {"wallet_name", "Sierra"}, {"encrypted", false}, {"unlocked", true}});
        QCoreApplication::processEvents();
        QVERIFY(!window->btnEncryptWallet_->isHidden());
    }

    // Issue #939: "Reload" of the active wallet made the daemon close and
    // reopen it LOCKED, discarding the unlock that v3 enrollment waits for.
    void activeWalletIsNeverReopened() {
        auto window = makeWindow();
        QVERIFY(window);
        QVERIFY(bindTestWallet(*window, "Sierra"));
        QVERIFY(window->cmbWalletSelector_);
        window->cmbWalletSelector_->setCurrentText("Sierra");
        window->walletUnlocked_ = true;
        window->updateWalletSwitcherState();
        QCOMPARE(window->btnLoadWallet_->text(), QString("Refresh Wallet"));
        window->onLoadSelectedWallet();
        QCoreApplication::processEvents();
        QVERIFY(!window->walletSwitchInFlight_);
        QVERIFY(window->pendingWalletOpenName_.isEmpty());

        window->walletUnlocked_ = false;
        window->updateWalletSwitcherState();
        QCOMPARE(window->btnLoadWallet_->text(), QString("Unlock Wallet"));
    }
};
QTEST_MAIN(WalletLayoutTest)
#include "test_wallet_layout.moc"
