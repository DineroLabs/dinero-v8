#include "portcheck.h"

#include <QTcpSocket>

bool tcpPortAccepts(const QString& host, quint16 port, int timeoutMs) {
    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
    // On macOS 27 with Qt 6.9, waitForConnected() can report a refused
    // connection as connected. A real connection always has a peer port.
    return socket.peerPort() != 0;
}
