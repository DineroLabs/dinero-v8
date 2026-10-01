#include <QtTest/QtTest>
#include <QGroupBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include "mainwindow.h"
#include "rpcclient.h"

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
        QVERIFY(consensus && mempool && resources && info && blocks && node);

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
            // Left column stays aligned too.
            QVERIFY2(qAbs(blocks->width() - info->width()) <= 1, qPrintable("blocks width" + at));
            QVERIFY2(qAbs(node->width() - info->width()) <= 1, qPrintable("node width" + at));
            // Optional visual check: OVERVIEW_SNAPSHOT_DIR=/path saves one PNG per size.
            const QString snapshotDir = qEnvironmentVariable("OVERVIEW_SNAPSHOT_DIR");
            if (!snapshotDir.isEmpty())
                window.grab().save(QString("%1/overview-%2.png").arg(snapshotDir).arg(size.width()));
        }
    }
};
QTEST_MAIN(OverviewColumnsTest)
#include "test_overview_columns.moc"
