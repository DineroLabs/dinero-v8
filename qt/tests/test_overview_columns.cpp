#include <QtTest/QtTest>
#include <QGroupBox>
#include <QTabWidget>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include "mainwindow.h"
#include "rpcclient.h"
#include "connection_manager.h"
#include "inforow.h"
#include <QMap>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QTableWidget>
#include <QComboBox>
#include <QToolTip>
#include <QPushButton>
#include <QHelpEvent>
#include <QTabBar>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QFile>
#include <QDir>

// The Overview tab must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in overview layout test"); }

class OverviewColumnsTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
    void connectionHealthChecksUseTheSelectedRpcEndpoint() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTcpServer endpoint, sink;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        QVERIFY(sink.listen(QHostAddress::LocalHost));
        const QUrl isolated(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()));
        qputenv("DINERO_RPC_URL", isolated.toString().toUtf8());
        // The sink is a process-local proxy. Any accidental request to the
        // default RPC port is trapped here instead of reaching a live node.
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
            QStringLiteral("127.0.0.1"), sink.serverPort()));
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* rpc = window.findChild<RpcClient*>();
        auto* health = window.findChild<ConnectionManager*>();
        QVERIFY(rpc && health);
        QCOMPARE(rpc->currentServer(), isolated.toString());
        QCOMPARE(QUrl(health->daemonUrl()), isolated);
    }
    void rightColumnPanelsShareOneEdgeAndWidth() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTcpServer endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroOverviewColumnsTest");
        QCoreApplication::setApplicationName("IsolatedOverviewColumns");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("updates/check_enabled", false);

        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        if (auto* rpc = window.findChild<RpcClient*>()) {
            rpc->setDatadir(dir.path());
            rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));
        }

        auto box = [&](const QString& title) -> QGroupBox* {
            for (auto* g : window.findChildren<QGroupBox*>())
                if (g->title() == title) return g;
            return nullptr;
        };
        QGroupBox* consensus = box("v7 Consensus Health");
        QGroupBox* mempool = box("📦 Mempool");
        QGroupBox* resources = box("Resources & mining");
        QGroupBox* info = box("Network Info");
        QGroupBox* blocks = box("Latest Blocks");
        QGroupBox* node = box("Node operation");
        QGroupBox* rewards = box("My mining rewards");
        QVERIFY(consensus && mempool && resources && info && blocks && node);
        QVERIFY2(rewards, "My mining rewards panel missing");

        for (const QSize size : {QSize(1440, 1000), QSize(1100, 900), QSize(1920, 1200)}) {
            window.resize(size);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QCoreApplication::processEvents();

            auto left = [&](QWidget* w) { return w->mapTo(&window, QPoint(0, 0)).x(); };
            const QString at = QString(" at %1x%2").arg(size.width()).arg(size.height());
            // Right column: Mempool and Resources line up under v7 Consensus.
            QVERIFY2(qAbs(left(mempool) - left(consensus)) <= 1, qPrintable("mempool left edge" + at));
            QVERIFY2(qAbs(left(resources) - left(consensus)) <= 1, qPrintable("resources left edge" + at));
            QVERIFY2(qAbs(mempool->width() - consensus->width()) <= 1, qPrintable("mempool width" + at));
            QVERIFY2(qAbs(resources->width() - consensus->width()) <= 1, qPrintable("resources width" + at));
            // My mining rewards sits under Resources in the same column and
            // ends level with Node operation.
            auto top = [&](QWidget* w) { return w->mapTo(&window, QPoint(0, 0)).y(); };
            QVERIFY2(qAbs(left(rewards) - left(consensus)) <= 1, qPrintable("rewards left edge" + at));
            QVERIFY2(qAbs(rewards->width() - consensus->width()) <= 1, qPrintable("rewards width" + at));
            QVERIFY2(top(rewards) > top(resources) + resources->height(), qPrintable("rewards below resources" + at));
            QVERIFY2(qAbs((top(rewards) + rewards->height()) - (top(node) + node->height())) <= 2,
                     qPrintable(QString("rewards bottom %1 vs node bottom %2").arg(top(rewards) + rewards->height())
                                    .arg(top(node) + node->height()) + at));
            // The peers summary ("3 peers · 3 outbound…") keeps its natural
            // height; extra row height goes to the peers table, not the labels.
            auto* peersCount = window.findChild<QLabel*>("overviewPeersCount");
            QVERIFY(peersCount);
            QVERIFY2(peersCount->height() <= peersCount->sizeHint().height() + 4,
                     qPrintable(QString("peers summary is %1 px tall").arg(peersCount->height()) + at));
            // Left column stays aligned too.
            QVERIFY2(qAbs(blocks->width() - info->width()) <= 1, qPrintable("blocks width" + at));
            QVERIFY2(qAbs(node->width() - info->width()) <= 1, qPrintable("node width" + at));
            // Optional visual check: OVERVIEW_SNAPSHOT_DIR=/path saves one PNG per size.
            const QString snapshotDir = qEnvironmentVariable("OVERVIEW_SNAPSHOT_DIR");
            if (!snapshotDir.isEmpty())
                window.grab().save(QString("%1/overview-%2.png").arg(snapshotDir).arg(size.width()));
        }
    }
    void rewardsPanelShowsTheWalletsRewards() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTcpServer endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()).toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* rpc = window.findChild<RpcClient*>();
        QVERIFY(rpc);
        auto* headline = window.findChild<QLabel*>("miningRewardsHeadline");
        auto* period = window.findChild<QLabel*>("miningRewardsPeriod");
        auto* maturing = window.findChild<QLabel*>("miningRewardsMaturing");
        QVERIFY(headline && period && maturing);

        const double now = double(QDateTime::currentSecsSinceEpoch());
        const QJsonArray rewards{
            QJsonObject{{"type", "mined"}, {"category", "immature"}, {"amount", 100.0}, {"confirmations", 5}, {"time", now - 600}},
            QJsonObject{{"type", "mined"}, {"category", "immature"}, {"amount", 100.0}, {"confirmations", 97}, {"time", now - 3600}},
        };
        const QString priorAlias = window.miningRewardsReplyAlias();
        Q_EMIT rpc->rpcResult(priorAlias, rewards);
        QCOMPARE(headline->text(), QString("2 blocks · 200 DIN"));
        QVERIFY2(maturing->text().startsWith("Maturing: 200 DIN · next unlock in 3 blocks"), qPrintable(maturing->text()));

        // A wallet history refusal is a valid RPC result object. It must
        // replace the prior reward numbers, not leave them looking current.
        Q_EMIT rpc->rpcResult(priorAlias,
                             QJsonObject{{"error", "Legacy transparent wallet migration pending"}});
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        QCOMPARE(period->text(), QString("Wallet history is not ready"));
        QVERIFY(maturing->text().isEmpty());

        // The Transactions tab must not be touched by this reply.
        Q_EMIT rpc->rpcError(priorAlias, -1, "No wallet loaded");
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));

        Q_EMIT rpc->rpcResult(priorAlias, rewards);
        QCOMPARE(headline->text(), QString("2 blocks · 200 DIN"));
        window.miningRewardsRequestedAtMs_ = 1234;
        window.miningRewardsRequestedHeight_ = 42;
        window.clearWalletScopedUiState();
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        QCOMPARE(window.miningRewardsRequestedAtMs_, qint64(0));
        QCOMPARE(window.miningRewardsRequestedHeight_, -1);
        const QString nextAlias = window.miningRewardsReplyAlias();
        QVERIFY(priorAlias != nextAlias);
        // An old in-flight response must not repaint the prior wallet after a switch.
        Q_EMIT rpc->rpcResult(priorAlias, rewards);
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        Q_EMIT rpc->rpcError(priorAlias, -1, "old wallet timeout");
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        Q_EMIT rpc->rpcResult(nextAlias, rewards);
        QCOMPARE(headline->text(), QString("2 blocks · 200 DIN"));
        QVERIFY(maturing->text().startsWith("Maturing: 200 DIN"));
    }
    void retryableRewardHistoryRefusalKeepsLastVerifiedWalletSnapshot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTcpServer endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()).toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* rpc = window.findChild<RpcClient*>();
        auto* headline = window.findChild<QLabel*>("miningRewardsHeadline");
        auto* period = window.findChild<QLabel*>("miningRewardsPeriod");
        auto* maturing = window.findChild<QLabel*>("miningRewardsMaturing");
        QVERIFY(rpc && headline && period && maturing && window.lblRewardsLastFound_);
        const QString alias = window.miningRewardsReplyAlias();
        const QString pending = "Legacy transparent wallet is catching up to the selected chain";

        // A fresh window has no verified reward amount to retain.
        Q_EMIT rpc->rpcError(alias, -32603, pending);
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        QCOMPARE(period->text(), QString("Waiting for current wallet history"));
        QVERIFY(maturing->text().isEmpty());

        const double now = double(QDateTime::currentSecsSinceEpoch());
        const QJsonArray rewards{
            QJsonObject{{"type", "mined"}, {"category", "immature"}, {"amount", 100.0},
                        {"confirmations", 97}, {"time", now - 180}},
        };
        Q_EMIT rpc->rpcResult(alias, rewards);
        const QString verifiedHeadline = headline->text();
        const QString verifiedMaturing = maturing->text();
        const QString verifiedLastFound = window.lblRewardsLastFound_->text();
        QCOMPARE(verifiedHeadline, QString("1 block · 100 DIN"));
        QVERIFY(!verifiedLastFound.isEmpty());

        // The live HTTP RPC promotes the retryable wallet refusal to an RPC
        // error. The old amounts remain visible only as an explicitly dated
        // last verification, including its maturity and last-found estimates.
        Q_EMIT rpc->rpcError(alias, -32603, pending);
        QCOMPARE(headline->text(), verifiedHeadline);
        QVERIFY(period->text().startsWith("Last 24 hours as of "));
        QVERIFY(period->text().contains("waiting for current wallet history"));
        QCOMPARE(maturing->text(), verifiedMaturing + " (at last verification)");
        QCOMPARE(window.lblRewardsLastFound_->text(), verifiedLastFound + " (at last verification)");
        Q_EMIT rpc->rpcError(alias, -32603, pending);
        QCOMPARE(maturing->text(), verifiedMaturing + " (at last verification)");

        // A fresh verified reply removes the stale annotation.
        Q_EMIT rpc->rpcResult(alias, rewards);
        QCOMPARE(period->text(), QString("found in the last 24 hours"));
        QCOMPARE(maturing->text(), verifiedMaturing);
        QCOMPARE(window.lblRewardsLastFound_->text(), verifiedLastFound);

        // A verified empty history is also a snapshot, with its own wording.
        Q_EMIT rpc->rpcResult(alias, QJsonArray{});
        QCOMPARE(headline->text(), QString("No mining rewards yet"));
        Q_EMIT rpc->rpcError(alias, -32603, pending);
        QCOMPARE(headline->text(), QString("No mining rewards yet"));
        QVERIFY(period->text().startsWith("Wallet history as of "));
        QVERIFY(maturing->text().isEmpty());

        // A wallet switch discards the old snapshot and rejects its late reply.
        window.clearWalletScopedUiState();
        const QString nextAlias = window.miningRewardsReplyAlias();
        QVERIFY(nextAlias != alias);
        Q_EMIT rpc->rpcError(alias, -32603, pending);
        Q_EMIT rpc->rpcResult(alias, rewards);
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        Q_EMIT rpc->rpcError(nextAlias, -32603, pending);
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        QCOMPARE(period->text(), QString("Waiting for current wallet history"));
    }
    void realDaemonMinedHistoryFeedsRewardsPanel() {
        if (qEnvironmentVariable("DINERO_QT_REWARDS_REGTEST") != "1")
            QSKIP("Requires the isolated mining-rewards regtest fixture");
        const QString datadir = qEnvironmentVariable("DINERO_QT_REWARDS_DATADIR");
        const QUrl endpoint(qEnvironmentVariable("DINERO_RPC_URL"));
        QCOMPARE(endpoint.scheme(), QString("http"));
        QCOMPARE(endpoint.host(), QString("127.0.0.1"));
        QVERIFY(endpoint.port() >= 32768 && endpoint.port() != 20998);
        QFile marker(QDir(datadir).filePath("owner.marker"));
        QVERIFY(marker.open(QIODevice::ReadOnly));
        QCOMPARE(marker.readAll(), QByteArray("qt-mining-rewards-regtest-v1\n"));
        QString sourceCookie = QDir(datadir).filePath(".cookie");
        if (!QFile::exists(sourceCookie)) sourceCookie = QDir(datadir).filePath("regtest/.cookie");
        QVERIFY(QFile::exists(sourceCookie));
#if defined(Q_OS_MAC)
        const QString fakeWalletDir = QDir::home().filePath("Library/Application Support/Dinero");
#elif defined(Q_OS_WIN)
        const QString fakeWalletDir = QDir::home().filePath("Dinero");
#else
        const QString fakeWalletDir = QDir::home().filePath(".dinero");
#endif
        QVERIFY(QDir().mkpath(fakeWalletDir));
        QVERIFY(QFile::copy(sourceCookie, QDir(fakeWalletDir).filePath(".cookie")));

        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* rpc = window.findChild<RpcClient*>();
        auto* health = window.findChild<ConnectionManager*>();
        auto* headline = window.findChild<QLabel*>("miningRewardsHeadline");
        auto* period = window.findChild<QLabel*>("miningRewardsPeriod");
        auto* maturing = window.findChild<QLabel*>("miningRewardsMaturing");
        QVERIFY(rpc && health && headline && period && maturing);
        QCOMPARE(rpc->currentServer(), endpoint.toString());
        QCOMPARE(QUrl(health->daemonUrl()), endpoint);
        QVERIFY(rpc->loadCookie());
        QTRY_VERIFY_WITH_TIMEOUT(health->isConnected(), 15000);
        window.checkRescanStatus();
        QTRY_COMPARE_WITH_TIMEOUT(window.currentWalletName_, QString("reward-first"), 15000);

        const QString firstAlias = window.miningRewardsReplyAlias();
        QSignalSpy replies(rpc, &RpcClient::rpcResult);
        window.requestMiningRewards(true);
        QTRY_VERIFY_WITH_TIMEOUT(headline->text().startsWith("2 blocks · "), 15000);
        QJsonArray firstRows;
        for (const auto& args : replies)
            if (args.at(0).toString() == firstAlias) firstRows = args.at(1).toJsonValue().toArray();
        QCOMPARE(firstRows.size(), 2);
        QVERIFY(maturing->text().startsWith("Maturing: "));
        QCOMPARE(period->text(), QString("found in the last 24 hours"));

        // Switch through the real UI route. The request and response must now
        // belong to the second named wallet; a late first-wallet reply is stale.
        auto* selector = window.cmbWalletSelector_;
        QVERIFY(selector);
        QTRY_VERIFY_WITH_TIMEOUT(selector->findText("reward-second") >= 0, 15000);
        selector->setCurrentIndex(selector->findText("reward-second"));
        window.onLoadSelectedWallet();
        QTRY_COMPARE_WITH_TIMEOUT(window.currentWalletName_, QString("reward-second"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.walletSwitchInFlight_, 15000);
        const QString secondAlias = window.miningRewardsReplyAlias();
        QVERIFY(firstAlias != secondAlias);
        Q_EMIT rpc->rpcResult(firstAlias, firstRows);
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        replies.clear();
        window.requestMiningRewards(true);
        QTRY_VERIFY_WITH_TIMEOUT(headline->text().startsWith("1 block · "), 15000);
        QJsonArray secondRows;
        for (const auto& args : replies)
            if (args.at(0).toString() == secondAlias) secondRows = args.at(1).toJsonValue().toArray();
        QCOMPARE(secondRows.size(), 1);
        QVERIFY(firstRows.at(0).toObject().value("txid") !=
                secondRows.at(0).toObject().value("txid"));

        // The real daemon refuses history without an active wallet. Qt must
        // show unavailable, not the last successful reward from wallet two.
        rpc->callNamed("wallet.unload", QJsonObject{{"name", "reward-second"}});
        QTRY_VERIFY_WITH_TIMEOUT(window.currentWalletName_.isEmpty(), 15000);
        const QString refusalAlias = window.miningRewardsReplyAlias();
        QSignalSpy failures(rpc, &RpcClient::rpcError);
        replies.clear();
        window.requestMiningRewards(true);
        const auto refused = [&] {
            for (const auto& args : replies)
                if (args.at(0).toString() == refusalAlias && !args.at(1).toJsonValue().isArray()) return true;
            for (const auto& args : failures)
                if (args.at(0).toString() == refusalAlias) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(refused(), 15000);
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
        QCOMPARE(period->text(), QString("Wallet history is not ready"));
    }
    void poolIconShowsACrowdOfFour() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        QTabWidget* tabs = nullptr;
        int pool = -1;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i)
                if (t->tabToolTip(i) == "Pool" || t->tabText(i) == "Pool") { tabs = t; pool = i; }
        QVERIFY(tabs && pool >= 0);
        const QImage img = tabs->tabIcon(pool).pixmap(QSize(40, 40)).toImage()
                               .scaled(40, 40, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QString snapshotDir = qEnvironmentVariable("OVERVIEW_SNAPSHOT_DIR");
        if (!snapshotDir.isEmpty()) {
            QImage big(img.size() * 8, QImage::Format_ARGB32);
            big.fill(QColor("#181b20"));
            QPainter p(&big);
            p.drawImage(big.rect(), img);
            p.end();
            big.save(snapshotDir + "/pool-icon.png");
        }
        auto ink = [&](int x, int y) { return qAlpha(img.pixel(x, y)) > 60; };
        // Tops of four heads: two small at the back (outer), two larger in front.
        QVERIFY2(ink(8, 7), "back-left head");
        QVERIFY2(ink(32, 7), "back-right head");
        QVERIFY2(ink(15, 10), "front-left head");
        QVERIFY2(ink(25, 10), "front-right head");
        // The back figures' shoulders reach the icon edges (a crowd, not a pair).
        QVERIFY2(ink(2, 21), "back-left shoulders");
        QVERIFY2(ink(37, 21), "back-right shoulders");
        // Faces stay open (outlined heads, not blobs).
        QVERIFY2(!ink(15, 14), "front-left head is filled");
        QVERIFY2(!ink(25, 14), "front-right head is filled");
    }
    void swapIconShowsTwoOpposingArrows() {
#if defined(DIN_ENABLE_SWAP_UI) && DIN_ENABLE_SWAP_UI
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        QTabWidget* tabs = nullptr;
        int swap = -1, transactions = -1;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i) {
                if (t->tabToolTip(i) == "Swap" || t->tabText(i) == "Swap") { tabs = t; swap = i; }
                if (t->tabToolTip(i) == "Transactions" || t->tabText(i) == "Transactions") transactions = i;
            }
        QVERIFY(tabs && swap >= 0 && transactions >= 0);
        auto image = [&](int i) {
            return tabs->tabIcon(i).pixmap(QSize(40, 40)).toImage()
                .scaled(40, 40, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        };
        const QImage img = image(swap);
        const QString snapshotDir = qEnvironmentVariable("OVERVIEW_SNAPSHOT_DIR");
        if (!snapshotDir.isEmpty()) {
            QImage big(img.size() * 8, QImage::Format_ARGB32);
            big.fill(QColor("#181b20"));
            QPainter p(&big);
            p.drawImage(big.rect(), img);
            p.end();
            big.save(snapshotDir + "/swap-icon.png");
        }
        auto ink = [&](int x, int y) { return qAlpha(img.pixel(x, y)) > 60; };
        QVERIFY2(ink(20, 13) && ink(20, 27), "two shafts");
        QVERIFY2(ink(29, 10) && ink(29, 16), "upper arrow points right");
        QVERIFY2(ink(11, 24) && ink(11, 30), "lower arrow points left");
        QVERIFY2(!ink(11, 10) && !ink(29, 30), "heads on opposite ends");
        QVERIFY2(image(transactions) != img, "Swap must not borrow the Transactions icon");
#else
        QSKIP("built without the Swap tab");
#endif
    }
    void miningIconIsAPickaxe() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        QTabWidget* tabs = nullptr;
        int mining = -1;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i)
                if (t->tabToolTip(i) == "Mining" || t->tabText(i) == "Mining") { tabs = t; mining = i; }
        QVERIFY(tabs && mining >= 0);
        const QImage img = tabs->tabIcon(mining).pixmap(QSize(40, 40)).toImage()
                               .scaled(40, 40, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QString snapshotDir = qEnvironmentVariable("OVERVIEW_SNAPSHOT_DIR");
        if (!snapshotDir.isEmpty()) {
            QImage big(img.size() * 8, QImage::Format_ARGB32);
            big.fill(QColor("#181b20"));
            QPainter p(&big);
            p.drawImage(big.rect(), img);
            p.end();
            big.save(snapshotDir + "/mining-icon.png");
        }
        auto ink = [&](int x, int y) { return qAlpha(img.pixel(x, y)) > 60; };
        // A curved pick head with two pointed tips, the handle meeting its middle.
        QVERIFY2(ink(16, 7), "upper-left tip of the pick head");
        QVERIFY2(ink(33, 24), "lower-right tip of the pick head");
        QVERIFY2(ink(28, 11), "curved top of the pick head");
        QVERIFY2(ink(12, 28), "handle");
        // Not a cross: nothing past the end of the handle.
        QVERIFY2(!ink(4, 35), "handle extends into a cross bar");
    }
    void miningThreadsStartAtTwo() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* threads = window.findChild<QLineEdit*>("miningThreads");
        QVERIFY(threads);
        QCOMPARE(threads->text(), QString("2"));
    }
    void recentAlertsStayCompactUntilThereAreAlerts() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        window.resize(1440, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* box = window.findChild<QGroupBox*>("overviewAlertsBox");
        auto* alerts = window.findChild<QTextEdit*>("overviewAlerts");
        QVERIFY(box && alerts);
        QCoreApplication::processEvents();
        QVERIFY2(box->height() <= 80, qPrintable(QString("empty alerts box is %1 px tall").arg(box->height())));
        QVERIFY(alerts->isHidden());

        for (int i = 0; i < 3; ++i) alerts->append(QString("⚠️ Test alert %1").arg(i));
        QCoreApplication::processEvents();
        QVERIFY(!alerts->isHidden());
        QVERIFY2(alerts->height() >= 3 * alerts->fontMetrics().lineSpacing(),
                 qPrintable(QString("alerts list %1 px for 3 lines").arg(alerts->height())));
        QVERIFY2(box->height() <= 220, qPrintable(QString("alerts box grew to %1 px").arg(box->height())));
    }
    void networkInfoReadsAsAList() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* rpc = window.findChild<RpcClient*>();
        QVERIFY(rpc);
        QGroupBox* info = nullptr;
        for (auto* g : window.findChildren<QGroupBox*>()) if (g->title() == "Network Info") info = g;
        QVERIFY(info);
        Q_EMIT rpc->rpcResult("blockchain.getinfo", QJsonObject{{"blocks", 121208}, {"headers", 121208}, {"chain", "main"}});
        Q_EMIT rpc->rpcResult("economics.getinfo", QJsonObject{{"current_halving_epoch", 0}, {"halving_interval", 1314000},
                                                                {"block_time_seconds", 120}, {"next_block_reward_din", "100.00000000"},
                                                                {"current_supply_din", "12120900"}});
        QMap<QString, InfoRow*> rows;
        for (auto* r : info->findChildren<InfoRow*>()) if (!r->isHidden() || true) rows[r->nameText()] = r;
        QVERIFY2(rows.contains("Height"), "Height row");
        QCOMPARE(rows["Height"]->valueText(), QString("121,208"));
        QVERIFY2(rows.contains("Next halving"), "Next halving row");
        QCOMPARE(rows["Next halving"]->valueText(), QString("block 1,314,001 · ~4.5 years"));
        // The Mempool card already shows mempool size; no duplicate row here.
        for (auto* r : info->findChildren<InfoRow*>())
            QVERIFY2(r->nameText() != "Mempool" || r->isHidden(), "duplicate Mempool row is visible");
    }
    void mempoolColumnsAreNotCramped() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        window.resize(1440, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* table = window.findChild<QTableWidget*>("overviewMempoolTable");
        QVERIFY(table);
        QCoreApplication::processEvents();
        for (int c = 1; c <= 3; ++c)
            QVERIFY2(table->columnWidth(c) >= 64,
                     qPrintable(QString("%1 column is %2 px").arg(table->horizontalHeaderItem(c)->text())
                                    .arg(table->columnWidth(c))));
        QVERIFY(table->columnWidth(0) > table->columnWidth(1));
    }
    void mainNavigationRowIsCentered() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTcpServer endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroOverviewColumnsTest");
        QCoreApplication::setApplicationName("IsolatedOverviewNavigation");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("updates/check_enabled", false);
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        if (auto* rpc = window.findChild<RpcClient*>()) {
            rpc->setDatadir(dir.path());
            rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));
        }
        auto* tabs = window.findChild<QTabWidget*>("mainNavigationTabs");
        QVERIFY(tabs);
        window.resize(1920, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        QTabBar* bar = tabs->tabBar();
        const int leftSpace = bar->x();
        const int rightSpace = tabs->width() - (bar->x() + bar->width());
        QVERIFY2(leftSpace + rightSpace >= 80,
                 qPrintable(QString("main tabs have no spare width: %1 px").arg(tabs->width())));
        QVERIFY2(qAbs(leftSpace - rightSpace) <= 6,
                 qPrintable(QString("main navigation is off-center: left %1 px, right %2 px")
                                .arg(leftSpace).arg(rightSpace)));
    }
    void windowShrinksPastTheTabNames() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        QTabWidget* tabs = nullptr;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i)
                if (t->tabToolTip(i) == "Overview" || t->tabText(i) == "Overview") tabs = t;
        QVERIFY(tabs);
        window.resize(1440, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        QVERIFY(!tabs->tabText(0).isEmpty());
        // With the names showing, the user drags the window narrower.
        window.resize(1000, 900);
        QCoreApplication::processEvents();
        QVERIFY2(window.width() <= 1000,
                 qPrintable(QString("window refused to shrink: stuck at %1 px (minimum %2 px)")
                                .arg(window.width()).arg(window.minimumSizeHint().width())));
        QVERIFY2(tabs->tabText(0).isEmpty(), "tabs did not switch to icons when the window shrank");
    }
    void subTabNamesAreNotCutOff() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        window.resize(1440, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTabWidget* main = nullptr;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i)
                if (t->tabToolTip(i) == "Overview" || t->tabText(i) == "Overview") main = t;
        QVERIFY(main);
        for (auto* t : window.findChildren<QTabWidget*>()) {
            if (t == main) continue;
            QWidget* page = t;
            while (page && page->parentWidget() != nullptr) {
                if (main->indexOf(page) >= 0) { main->setCurrentWidget(page); break; }
                page = page->parentWidget();
            }
            QCoreApplication::processEvents();
            if (!t->isVisible()) continue;
            QTabBar* bar = t->tabBar();
            // macOS sizes tabs without the stylesheet padding, leaving no slack; a label
            // that is allowed to elide then shows as "Colle…". Labels must never be cut.
            QCOMPARE(bar->elideMode(), Qt::ElideNone);
            for (int i = 0; i < t->count(); ++i)
                QVERIFY2(bar->tabRect(i).width() >= bar->fontMetrics().horizontalAdvance(t->tabText(i)),
                         qPrintable(QString("sub-tab '%1' narrower than its name").arg(t->tabText(i))));
        }
    }
    void tabNamesFitAtCommonWidths() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        QTabWidget* tabs = nullptr;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i)
                if (t->tabToolTip(i) == "Overview" || t->tabText(i) == "Overview") tabs = t;
        QVERIFY(tabs);
        // A common laptop/desktop window: every tab keeps its name.
        window.resize(1440, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        if (const QString shots = qEnvironmentVariable("OVERVIEW_SNAPSHOT_DIR"); !shots.isEmpty()) {
            tabs->tabBar()->grab().save(shots + "/tab-bar-1440.png");
        }
        for (int i = 0; i < tabs->count(); ++i)
            QVERIFY2(!tabs->tabText(i).isEmpty(),
                     qPrintable(QString("1440 px: %1 lost its name (bar needs %2 px)")
                                    .arg(tabs->tabToolTip(i)).arg(tabs->tabBar()->sizeHint().width())));
    }
    void narrowWindowShowsIconOnlyTabs() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        QTabWidget* tabs = nullptr;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i)
                if (t->tabToolTip(i) == "Overview" || t->tabText(i) == "Overview") tabs = t;
        QVERIFY(tabs);

        window.resize(800, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        for (int i = 0; i < tabs->count(); ++i) {
            QVERIFY2(tabs->tabText(i).isEmpty(), qPrintable("narrow: tab still has text " + tabs->tabText(i)));
            QVERIFY2(!tabs->tabToolTip(i).isEmpty(), "narrow: tab has no name on hover");
            QVERIFY(!tabs->tabIcon(i).isNull());
        }
        // Navigation by tab name still works in icon-only mode.
        QComboBox* modes = nullptr;
        for (auto* combo : window.findChildren<QComboBox*>())
            if (combo->findData("public_transfer") >= 0 && combo->findData("public_contract") >= 0) modes = combo;
        QVERIFY(modes);
        modes->setCurrentIndex(modes->findData("public_contract"));
        QCOMPARE(tabs->tabToolTip(tabs->currentIndex()), QString("Covenants"));

        window.resize(1920, 1000);
        QCoreApplication::processEvents();
        QCOMPARE(tabs->tabText(0), QString("Overview"));
        for (int i = 0; i < tabs->count(); ++i) QVERIFY2(!tabs->tabText(i).isEmpty(), "wide: names restored");
    }
    void sendAndReceiveIconsUseTrays() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto icon = [&](const QString& name) {
            for (auto* t : window.findChildren<QTabWidget*>())
                for (int i = 0; i < t->count(); ++i)
                    if (t->tabToolTip(i) == name || t->tabText(i) == name)
                        return t->tabIcon(i).pixmap(QSize(40, 40)).toImage()
                            .scaled(40, 40, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            return QImage();
        };
        for (const QString& name : {QString("Send"), QString("Receive")}) {
            const QImage img = icon(name);
            QVERIFY2(!img.isNull(), qPrintable(name + " tab"));
            auto ink = [&](int x, int y) { return qAlpha(img.pixel(x, y)) > 60; };
            // An open tray at the bottom, and a vertical arrow above it.
            QVERIFY2(ink(7, 28) && ink(33, 28) && ink(20, 34), qPrintable(name + " tray"));
            QVERIFY2(ink(20, 15), qPrintable(name + " arrow shaft"));
            QVERIFY2(!ink(5, 20) && !ink(35, 20), qPrintable(name + " still a sideways arrow"));
            if (name == "Send") QVERIFY2(ink(14, 12) && ink(26, 12), "Send arrow points up");
            else QVERIFY2(ink(14, 18) && ink(26, 18), "Receive arrow points down");
        }
    }
    void hoveringATabShowsItsName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        // Names show on hover even while another app (e.g. Terminal) is in front.
        QVERIFY(window.testAttribute(Qt::WA_AlwaysShowToolTips));
        window.resize(900, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        QTabWidget* tabs = nullptr;
        for (auto* t : window.findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); ++i) if (t->tabToolTip(i) == "Receive") tabs = t;
        QVERIFY(tabs);
        int receive = -1;
        for (int i = 0; i < tabs->count(); ++i) if (tabs->tabToolTip(i) == "Receive") receive = i;
        QTabBar* bar = tabs->tabBar();
        const QPoint at = bar->tabRect(receive).center();
        QHelpEvent help(QEvent::ToolTip, at, bar->mapToGlobal(at));
        QCoreApplication::sendEvent(bar, &help);
        QTRY_COMPARE_WITH_TIMEOUT(QToolTip::text(), QString("Receive"), 2000);
    }
    void openExplorerRestoresAMinimizedExplorer() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        window.resize(1440, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QPushButton* open = nullptr;
        for (auto* b : window.findChildren<QPushButton*>()) if (b->text() == "Open Explorer") open = b;
        QVERIFY(open);
        QWidget* explorer = nullptr;
        for (auto* w : window.findChildren<QWidget*>()) if (w->windowTitle() == "Dinero Chain Explorer") explorer = w;
        QVERIFY(explorer);
        QTest::mouseClick(open, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(explorer->isVisible(), 2000);
        // The user minimizes the explorer to the Dock; Open Explorer must bring it back.
        explorer->setWindowState(explorer->windowState() | Qt::WindowMinimized);
        QTRY_VERIFY_WITH_TIMEOUT(explorer->isMinimized(), 2000);
        QTest::mouseClick(open, Qt::LeftButton);
        QTRY_VERIFY2_WITH_TIMEOUT(!explorer->isMinimized(), "Open Explorer left the explorer minimized", 2000);
        QVERIFY(explorer->isVisible());
    }
};
int main(int argc, char** argv) {
    // Keep every Overview test out of the user's real daemon discovery,
    // cookie and settings paths, even when run on a workstation with a node.
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    qputenv("HOME", home.path().toUtf8());
    qputenv("USERPROFILE", home.path().toUtf8());
    qputenv("APPDATA", home.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QApplication app(argc, argv);
    OverviewColumnsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_overview_columns.moc"
