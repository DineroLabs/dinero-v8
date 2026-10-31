#include <QtTest/QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include "connection_manager.h"

class ConnectionManagerTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
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
