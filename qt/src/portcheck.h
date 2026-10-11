#pragma once
#include <QString>

// True only when something is actually accepting TCP connections on
// host:port within timeoutMs.
bool tcpPortAccepts(const QString& host, quint16 port, int timeoutMs);
