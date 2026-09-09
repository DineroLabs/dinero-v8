#include <QtTest/QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QComboBox>
#include <QTabWidget>
#include "mainwindow.h"
#include "rpcclient.h"
#include "shieldedwidget.h"

// Navigation must never start or clean up daemon processes.
void killStaleDinerodByPort() { qFatal("Unexpected daemon cleanup in navigation test"); }

class WalletNavigationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void sharedCovenantComposerMovesWithoutDuplicatingState() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTcpServer endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        qputenv("DINERO_RPC_URL", QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()).toUtf8());
        QCoreApplication::setOrganizationName("DineroNavigationTest");
        QCoreApplication::setApplicationName("IsolatedWalletNavigation");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        MainWindow window(dinero::qt::DaemonBootstrapOwner::ApplicationMain);
        auto* rpc = window.findChild<RpcClient*>();
        QVERIFY(rpc);
        rpc->setDatadir(dir.path());
        rpc->setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));
        QComboBox* modes = nullptr;
        for (auto* combo : window.findChildren<QComboBox*>())
            if (combo->findData("public_transfer") >= 0 && combo->findData("public_contract") >= 0) modes = combo;
        QVERIFY(modes);
        QVERIFY(modes->findData("private_composer") >= 0);
        QTabWidget* tabs = nullptr;
        int send = -1, covenants = -1;
        for (auto* candidate : window.findChildren<QTabWidget*>()) {
            for (int i = 0; i < candidate->count(); ++i) {
                if (candidate->tabText(i) == "Covenants") { tabs = candidate; covenants = i; }
            }
        }
        QVERIFY(tabs);
        for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == "Send") send = i;
        QVERIFY(send >= 0);
        tabs->setCurrentIndex(send);
        QVERIFY(tabs->widget(send)->isAncestorOf(modes));
        auto* recipient = window.findChild<QLineEdit*>("sendRecipient");
        QVERIFY(recipient);
        recipient->setText("payment draft");
        modes->setCurrentIndex(modes->findData("public_contract"));
        QCOMPARE(tabs->currentIndex(), covenants);
        QVERIFY(recipient->text().isEmpty());
        recipient->setText("covenant draft");
        QVERIFY(tabs->widget(covenants)->isAncestorOf(modes));
        tabs->setCurrentIndex(send);
        QCOMPARE(modes->currentData().toString(), QString("public_transfer"));
        QCOMPARE(recipient->text(), QString("payment draft"));
        QVERIFY(tabs->widget(send)->isAncestorOf(modes));
        tabs->setCurrentIndex(covenants);
        QCOMPARE(modes->currentData().toString(), QString("public_contract"));
        QCOMPARE(recipient->text(), QString("covenant draft"));
        const QString screenshot = qEnvironmentVariable("DINERO_QT_NAV_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            window.show();
            QApplication::processEvents();
            QVERIFY(window.grab().save(screenshot));
        }
    }
};
QTEST_MAIN(WalletNavigationTest)
#include "test_wallet_navigation.moc"
