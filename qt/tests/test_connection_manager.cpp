#include <QtTest/QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <memory>
#include "connection_manager.h"
#include "walletcreaterefusal.h"

class ConnectionManagerTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void automaticCreationRetryRequiresTypedNoWalletRefusal() {
        QVERIFY(WalletCreateRefusal::mayAutoRetry("snapshot_bootstrap_pending", true));
        QVERIFY(WalletCreateRefusal::mayAutoRetry("orchard_history_pending", true));
        QVERIFY(!WalletCreateRefusal::mayAutoRetry("orchard_history_pending", false));
        QVERIFY(!WalletCreateRefusal::mayAutoRetry("native_wallet_unservable", true));
        QVERIFY(!WalletCreateRefusal::mayAutoRetry("", true));
    }

    void walletCreationGetsTypedRefusalButTransportFailureDoesNot() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile cookie(directory.filePath(".cookie"));
        QVERIFY(cookie.open(QIODevice::WriteOnly));
        cookie.write("test:owned-fixture");
        cookie.close();

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int walletRequests = 0;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (auto* socket = server.nextPendingConnection()) {
                auto pending = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, pending]() {
                    pending->append(socket->readAll());
                    const int split = pending->indexOf("\r\n\r\n");
                    if (split < 0) return;
                    int length = 0;
                    for (const auto& line : pending->left(split).split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (pending->size() < split + 4 + length) return;
                    const auto request = QJsonDocument::fromJson(pending->mid(split + 4, length)).object();
                    const bool wallet = request.value("method").toString() == "wallet.createhd";
                    if (wallet) ++walletRequests;
                    const QByteArray body = wallet
                        ? walletRequests == 1
                            ? R"({"result":null,"error":{"code":-32603,"message":"Orchard history is not ready; wallet was not created. Retry after history sync.","data":{"wallet_creation":{"error_code":"orchard_history_pending","retryable":true}}}})"
                            : QByteArray("not JSON")
                        : R"({"result":142767,"error":null})";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        ConnectionManager manager;
        manager.setCookiePath(cookie.fileName());
        manager.setDaemonUrl(QString("http://127.0.0.1:%1").arg(server.serverPort()));
        manager.setHealthCheckInterval(30000);
        manager.connectToDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::Connected, 3000);

        QString refusal;
        QJsonObject data;
        manager.callNamed("wallet.createhd", {{"name", "Fresh"}}, nullptr, nullptr,
            [&](const QString& error, const QJsonObject& details) {
                refusal = error;
                data = details;
            });
        QTRY_VERIFY_WITH_TIMEOUT(!refusal.isEmpty(), 3000);
        QCOMPARE(walletRequests, 1);
        const auto typed = data.value("wallet_creation").toObject();
        QCOMPARE(typed.value("error_code").toString(), QString("orchard_history_pending"));
        QCOMPARE(typed.value("retryable").toBool(), true);

        refusal.clear();
        data = {};
        manager.callNamed("wallet.createhd", {{"name", "Fresh"}}, nullptr, nullptr,
            [&](const QString& error, const QJsonObject& details) {
                refusal = error;
                data = details;
            });
        QTRY_COMPARE_WITH_TIMEOUT(refusal, QString("Malformed RPC response"), 3000);
        QVERIFY(data.isEmpty());
        QCOMPARE(walletRequests, 2);

        QVERIFY(cookie.remove());
        refusal.clear();
        data = {};
        manager.callNamed("wallet.createhd", {{"name", "Fresh"}}, nullptr, nullptr,
            [&](const QString& error, const QJsonObject& details) {
                refusal = error;
                data = details;
            });
        QCOMPARE(refusal, QString("Cookie authentication failed"));
        QVERIFY(data.isEmpty());
        QCOMPARE(walletRequests, 2);
        manager.disconnectFromDaemon();
    }

    void lateDaemonRecoversAfterFastRetriesExpire() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray body = R"({"result":142767,"error":null,"id":"health_check"})";
        int requests = 0;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (auto* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                    socket->readAll();
                    ++requests;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        ConnectionManager manager;
        manager.setCookiePath(directory.filePath(".cookie"));
        manager.setDaemonUrl(QString("http://127.0.0.1:%1").arg(server.serverPort()));
        manager.setMaxRetries(1);
        QSignalSpy connected(&manager, &ConnectionManager::connected);
        QSignalSpy synced(&manager, &ConnectionManager::blockchainSynced);
        manager.connectToDaemon();
        manager.reconnect(); // Exhaust the fast budget while the daemon has no cookie.
        QCOMPARE(manager.state(), ConnectionManager::Failed);
        QTimer* probe = nullptr;
        for (auto* timer : manager.findChildren<QTimer*>()) {
            if (timer->isActive() && timer->interval() == 30000) probe = timer;
        }
        QVERIFY2(probe, "An exhausted startup budget must retain a slow health probe");
        // Accelerate the real installed timer; no 46-minute sleep or fake clock.
        probe->setInterval(10);
        QTest::qWait(40);
        QCOMPARE(manager.state(), ConnectionManager::Failed);
        QCOMPARE(connected.count(), 0);

        QFile cookie(directory.filePath(".cookie"));
        QVERIFY(cookie.open(QIODevice::WriteOnly));
        cookie.write("test:owned-fixture");
        cookie.close();
        // HTTP 200 alone is not readiness: initialization RPC errors and invalid
        // replies must not publish a usable wallet connection.
        const QList<QByteArray> unavailable{
            R"({"result":null,"error":{"code":-28,"message":"Starting"}})",
            "not JSON", R"({"result":-1,"error":null})",
            R"({"result":1.5,"error":null})", R"({"result":"142767","error":null})"};
        for (const auto& response : unavailable) {
            body = response;
            const auto before = requests;
            QTRY_VERIFY_WITH_TIMEOUT(requests > before, 3000);
            QCOMPARE(manager.state(), ConnectionManager::Failed);
            QCOMPARE(connected.count(), 0);
        }
        body = R"({"result":142767,"error":null,"id":"health_check"})";
        QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::Connected, 3000);
        QCOMPARE(connected.count(), 1);
        // The health response contains only getblockcount. It cannot claim
        // headers == blocks, since downloaded headers may be ahead of the tip.
        QCOMPARE(synced.count(), 0);
        QVERIFY(probe->isActive());
        QCOMPARE(probe->interval(), 5000); // Restore the normal connected cadence.
        manager.disconnectFromDaemon();
        QVERIFY(!probe->isActive());
    }

    void explicitDisconnectCancelsLateRecovery() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ConnectionManager manager;
        manager.setCookiePath(directory.filePath("missing-cookie"));
        manager.setMaxRetries(1);
        manager.connectToDaemon();
        manager.reconnect();
        QCOMPARE(manager.state(), ConnectionManager::Failed);
        manager.disconnectFromDaemon();
        for (auto* timer : manager.findChildren<QTimer*>()) QVERIFY(!timer->isActive());
        QTest::qWait(30);
        QCOMPARE(manager.state(), ConnectionManager::Disconnected);
    }
};
QTEST_GUILESS_MAIN(ConnectionManagerTest)
#include "test_connection_manager.moc"
