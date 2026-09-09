#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QMessageBox>
#include <QLabel>
#include <QJsonObject>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include "privatecovenantwidget.h"
#include "rpcclient.h"
class PrivateCovenantWidgetTest:public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        QCoreApplication::setOrganizationName("DineroPrivateCovenantTest");
        QCoreApplication::setApplicationName("IsolatedLifecycle");
    }
    void capabilitiesFailClosedAndWalletScopeClearsDrafts() {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QTcpServer endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        // Keep transport pending: this case explicitly supplies RPC events.
        RpcClient rpc; rpc.setDatadir(dir.path());
        rpc.setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort())));
        PrivateCovenantWidget widget(&rpc); widget.setWalletScope("alpha");
        auto* fund=widget.findChild<QPushButton*>("privateCovenantFund"); QVERIFY(fund); QVERIFY(!fund->isEnabled());
        Q_EMIT rpc.rpcResult("wallet.shieldedbalance",QJsonObject{{"private_covenants_enabled",true}});
        QVERIFY(fund->isEnabled());
        Q_EMIT rpc.rpcResult("wallet.shieldedbalance",QJsonObject{}); QVERIFY(!fund->isEnabled());
        Q_EMIT rpc.rpcResult("wallet.shieldedbalance",QJsonObject{{"private_covenants_enabled",true}});
        auto* owner=widget.findChild<QLineEdit*>("privateCovenantOwner"); owner->setText("draft");
        widget.setWalletScope("beta"); QVERIFY(owner->text().isEmpty()); QVERIFY(!fund->isEnabled());
        Q_EMIT rpc.rpcResult("wallet.shieldedbalance",QJsonObject{{"private_covenants_enabled",true}});
        Q_EMIT rpc.connectionFailed("disconnected"); QVERIFY(!fund->isEnabled());
        Q_EMIT rpc.rpcResult("wallet.shieldedbalance",QJsonObject{{"private_covenants_enabled",true}});
        owner->setText("rdins1fixture");
        auto* outputs=widget.findChild<QTableWidget*>("privateCovenantOutputs");
        outputs->item(0,0)->setText("rdins1payee"); outputs->item(0,1)->setText("0.2");
        const auto accept=[] { for(auto* top:QApplication::topLevelWidgets()) if(auto* box=qobject_cast<QMessageBox*>(top)) box->button(QMessageBox::Ok)->click(); };
        QTimer::singleShot(1,accept); fund->click(); QVERIFY(!fund->isEnabled());
        Q_EMIT rpc.rpcError("wallet.covenant.privatefund",-32603,"wallet_locked");
        QVERIFY(fund->isEnabled());
        QTimer::singleShot(1,accept); fund->click(); QVERIFY(!fund->isEnabled());
        Q_EMIT rpc.rpcError("wallet.covenant.privatefund",-1,"connection lost");
        QVERIFY(!fund->isEnabled());
        widget.setWalletScope({}); widget.setWalletScope("beta");
        Q_EMIT rpc.rpcResult("wallet.shieldedbalance",QJsonObject{{"private_covenants_enabled",true}});
        QVERIFY(!fund->isEnabled()); // durable uncertain outcome, never automatic retry
        auto* resolve=widget.findChild<QPushButton*>("privateCovenantResolve"); QVERIFY(resolve->isEnabled());
        QTimer::singleShot(1,accept); resolve->click(); QVERIFY(fund->isEnabled());
        QVERIFY(outputs->item(0,0)->text().isEmpty()); // no replay of the old form

    }
    void fundMovingErrorsNeverFailOverOrReplay() {
        QTcpServer primary,backup;
        QVERIFY(primary.listen(QHostAddress::LocalHost)); QVERIFY(backup.listen(QHostAddress::LocalHost));
        int backupRequests=0;
        const auto reject=[](QTcpSocket* socket) {
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket]{
                socket->readAll();
                socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        };
        connect(&primary,&QTcpServer::newConnection,this,[&]{reject(primary.nextPendingConnection());});
        connect(&backup,&QTcpServer::newConnection,this,[&]{++backupRequests;reject(backup.nextPendingConnection());});
        QTemporaryDir dir;
        RpcClient rpc; rpc.setDatadir(dir.path());
        rpc.setEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(primary.serverPort())));
        rpc.addEndpoint(QUrl(QString("http://127.0.0.1:%1/").arg(backup.serverPort())));
        QSignalSpy errors(&rpc,&RpcClient::rpcError);
        for(int i=0;i<6;++i) {
            rpc.callNamed(i<3 ? "wallet.covenant.privatefund" : "wallet.transfer",{});
            QTRY_COMPARE_WITH_TIMEOUT(errors.size(),i+1,5000);
        }
        QCOMPARE(backupRequests,0);
    }
    void realDaemonFundingAndPayment() {
        if(qEnvironmentVariable("DINERO_PRIVATE_GUI_LIFECYCLE")!="1") QSKIP("Requires isolated regtest lifecycle fixture");
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDir.path());
        RpcClient rpc; rpc.setDatadir(qEnvironmentVariable("DINERO_PRIVATE_GUI_DATADIR"));
        rpc.setEndpoint(QUrl(qEnvironmentVariable("DINERO_RPC_URL"))); QVERIFY(rpc.loadCookie());
        PrivateCovenantWidget widget(&rpc); widget.setWalletScope("default"); widget.show(); widget.refresh();
        auto* fund=widget.findChild<QPushButton*>("privateCovenantFund");
        QTRY_VERIFY_WITH_TIMEOUT(fund->isEnabled(),20000);
        widget.findChild<QLineEdit*>("privateCovenantOwner")->setText(qEnvironmentVariable("DINERO_PRIVATE_GUI_OWNER"));
        auto* outputs=widget.findChild<QTableWidget*>("privateCovenantOutputs");
        outputs->item(0,0)->setText(qEnvironmentVariable("DINERO_PRIVATE_GUI_RECIPIENT")); outputs->item(0,1)->setText("0.20000000");
        outputs->item(1,0)->setText(qEnvironmentVariable("DINERO_PRIVATE_GUI_RECIPIENT")); outputs->item(1,1)->setText("0.10000000");
        const auto acceptReview=[] {
            for(auto* top:QApplication::topLevelWidgets()) if(auto* box=qobject_cast<QMessageBox*>(top)) box->button(QMessageBox::Ok)->click();
        };
        QSignalSpy results(&rpc,&RpcClient::rpcResult);
        QTimer::singleShot(100,acceptReview); fund->click();
        if(fund->isEnabled()) for(auto* label:widget.findChildren<QLabel*>()) qWarning()<<label->text();
        QVERIFY(!fund->isEnabled());
        const auto resultFor=[&](const QString& method) {
            for(const auto& args:results) if(args[0].toString()==method) return qvariant_cast<QJsonValue>(args[1]).toObject();
            return QJsonObject{};
        };
        QTRY_VERIFY_WITH_TIMEOUT(!resultFor("wallet.covenant.privatefund").isEmpty(),600000);
        auto funded=resultFor("wallet.covenant.privatefund"); QVERIFY2(funded.value("txid").toString().size()==64,qPrintable(funded.value("error").toString()));
        results.clear(); rpc.call("generatetoaddress",QJsonArray{1,qEnvironmentVariable("DINERO_PRIVATE_GUI_MINER")});
        QTRY_VERIFY_WITH_TIMEOUT(resultFor("generatetoaddress").contains("blocks"),60000);
        widget.refresh();
        auto* inventory=widget.findChild<QTableWidget*>("privateCovenantInventory");
        QTRY_COMPARE_WITH_TIMEOUT(inventory->rowCount(),1,20000);
        auto* spend=qobject_cast<QPushButton*>(inventory->cellWidget(0,3)); QVERIFY(spend); QVERIFY(spend->isEnabled());
        results.clear(); QTimer::singleShot(100,acceptReview); spend->click();
        QTRY_VERIFY_WITH_TIMEOUT(!resultFor("wallet.covenant.privatespend").isEmpty(),600000);
        auto paid=resultFor("wallet.covenant.privatespend"); QVERIFY2(paid.value("txid").toString().size()==64,qPrintable(paid.value("error").toString()));
        results.clear(); rpc.call("generatetoaddress",QJsonArray{1,qEnvironmentVariable("DINERO_PRIVATE_GUI_MINER")});
        QTRY_VERIFY_WITH_TIMEOUT(resultFor("generatetoaddress").contains("blocks"),60000);
        results.clear(); rpc.call("wallet.listshielded");
        QTRY_VERIFY_WITH_TIMEOUT(resultFor("wallet.listshielded").contains("notes"),20000);
        bool recipient=false,secondRecipient=false;
        for(const auto& value:resultFor("wallet.listshielded").value("notes").toArray()) {
            const auto note=value.toObject();
            if(note.value("confirmed").toBool() && !note.value("private_covenant").toBool()) {
                if(note.value("value_una").toInteger()==20000000) recipient=true;
                if(note.value("value_una").toInteger()==10000000) secondRecipient=true;
            }
        }
        QVERIFY(recipient); QVERIFY(secondRecipient);
        const auto screenshot=qEnvironmentVariable("DINERO_PRIVATE_GUI_SCREENSHOT");
        if(!screenshot.isEmpty()) QVERIFY(widget.grab().save(screenshot));
    }
};
QTEST_MAIN(PrivateCovenantWidgetTest)
#include "test_private_covenant_widget.moc"
