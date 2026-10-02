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

// The Overview tab must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in overview layout test"); }

class OverviewColumnsTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
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
        auto* maturing = window.findChild<QLabel*>("miningRewardsMaturing");
        QVERIFY(headline && maturing);

        const double now = double(QDateTime::currentSecsSinceEpoch());
        const QJsonArray rewards{
            QJsonObject{{"type", "mined"}, {"category", "immature"}, {"amount", 100.0}, {"confirmations", 5}, {"time", now - 600}},
            QJsonObject{{"type", "mined"}, {"category", "immature"}, {"amount", 100.0}, {"confirmations", 97}, {"time", now - 3600}},
        };
        Q_EMIT rpc->rpcResult("overview.miningrewards", rewards);
        QCOMPARE(headline->text(), QString("2 blocks · 200 DIN"));
        QVERIFY2(maturing->text().startsWith("Maturing: 200 DIN · next unlock in 3 blocks"), qPrintable(maturing->text()));

        // The Transactions tab must not be touched by this reply.
        Q_EMIT rpc->rpcError("overview.miningrewards", -1, "No wallet loaded");
        QCOMPARE(headline->text(), QString("Mining rewards unavailable"));
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
    void miningThreadsStartAtFour() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* threads = window.findChild<QLineEdit*>("miningThreads");
        QVERIFY(threads);
        QCOMPARE(threads->text(), QString("4"));
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
QTEST_MAIN(OverviewColumnsTest)
#include "test_overview_columns.moc"
