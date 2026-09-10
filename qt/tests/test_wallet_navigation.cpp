#include <QtTest/QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QComboBox>
#include <QTabWidget>
#include <QPushButton>
#include <QLabel>
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
        modes->setCurrentIndex(modes->findData("private_contract"));
        QCOMPARE(tabs->currentIndex(), covenants);
        QVERIFY(recipient->isHidden());
        QVERIFY(window.findChild<QLineEdit*>("privateCovenantOwner"));
        tabs->setCurrentIndex(send);
        QCOMPARE(modes->currentData().toString(),QString("public_transfer"));
        QVERIFY(!recipient->isHidden());
        modes->setCurrentIndex(modes->findData("private_contract"));
        // A startup wallet reply must survive clearing the previous drafts.
        // Before the fix, clearing after binding silently disabled address lookup.
        QVERIFY(QMetaObject::invokeMethod(&window,"checkRescanStatus",Qt::DirectConnection));
        Q_EMIT rpc->rpcResult("wallet.getinfo",QJsonObject{{"wallet_name","alpha"},{"hd_enabled",true},{"locked",true}});
        auto* ownAddress=window.findChild<QPushButton*>("privateCovenantOwnAddress"); QVERIFY(ownAddress);
        auto* owner=window.findChild<QLineEdit*>("privateCovenantOwner"); QVERIFY(owner);
        auto* addressStatus=window.findChild<QLabel*>("privateCovenantOwnerStatus"); QVERIFY(addressStatus);
        ownAddress->click();
        Q_EMIT rpc->rpcResult("wallet.getshieldedaddress",QJsonObject{{"address","dins1cachedaddress"}});
        QCOMPARE(owner->text(),QString("dins1cachedaddress"));
        QVERIFY(QMetaObject::invokeMethod(&window,"checkRescanStatus",Qt::DirectConnection));
        Q_EMIT rpc->rpcResult("wallet.getinfo",QJsonObject{{"wallet_name","beta"},{"hd_enabled",true},{"locked",true}});
        QVERIFY(owner->text().isEmpty());
        ownAddress->click();
        Q_EMIT rpc->rpcResult("wallet.getshieldedaddress",QJsonObject{{"error","wallet_locked_receive_cache_miss"},{"error_message","Unlock once to derive your address"}});
        QVERIFY(owner->text().isEmpty());
        QVERIFY(addressStatus->text().contains("Unlock once"));
        ownAddress->click();
        Q_EMIT rpc->rpcError("wallet.getshieldedaddress",-1,"Connection failed");
        QVERIFY(addressStatus->text().contains("Connection failed"));
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
