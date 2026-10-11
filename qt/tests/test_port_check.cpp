#include <QtTest/QtTest>
#include <QTcpServer>
#include "portcheck.h"

class PortCheckTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void listeningPortAccepts() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QVERIFY(tcpPortAccepts("127.0.0.1", server.serverPort(), 500));
    }
    void closedPortDoesNotAccept() {
        quint16 port = 0;
        {
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
            port = server.serverPort();
        }  // closed again: nothing listens on this port now
        // On macOS 27 with Qt 6.9 waitForConnected() reports a refused
        // connection as connected, which made startup claim port 20998 was
        // in use when no node was running.
        for (int i = 0; i < 3; ++i) {
            QVERIFY2(!tcpPortAccepts("127.0.0.1", port, 500), "a closed port was reported as accepting");
        }
    }
};
QTEST_MAIN(PortCheckTest)
#include "test_port_check.moc"
