# Qt6 Cookie Authentication for Dinero

This document shows how to implement automatic cookie-based authentication in your Qt6 wallet and miner applications.

## Overview

**Local GUI ↔ Local Daemon**: Use cookie auth (no passwords to manage, per-user, per-machine, rotates on restart)
**Remote Connections**: Use user-provided credentials (store securely in OS keychain)

## Qt6 Implementation

Drop this code into your RPC client to automatically read and use cookie authentication:

```cpp
// Cookie-based Basic Auth for local Dinero daemon (Qt6)
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFileSystemWatcher>

class DineroRpcClient : public QObject {
    Q_OBJECT

private:
    QNetworkAccessManager* m_nam;
    QFileSystemWatcher* m_cookieWatcher;
    QString m_rpcUrl;
    QString m_cookiePath;
    QByteArray m_authHeader;
    bool m_testnet;

public:
    DineroRpcClient(bool testnet = true, QObject* parent = nullptr) 
        : QObject(parent), m_testnet(testnet) {
        m_nam = new QNetworkAccessManager(this);
        m_cookieWatcher = new QFileSystemWatcher(this);
        
        // Set up paths
        QString dataDir = dineroDataDir(testnet);
        m_cookiePath = dataDir + "/.cookie";
        m_rpcUrl = "http://127.0.0.1:20998/";  // Default testnet RPC port
        
        // Watch for cookie changes
        connect(m_cookieWatcher, &QFileSystemWatcher::fileChanged,
                this, &DineroRpcClient::onCookieChanged);
        
        // Initial cookie read
        updateCookieAuth();
    }

private:
    static QString dineroDataDir(bool testnet) {
#ifdef Q_OS_MAC
        QString base = QDir::homePath() + "/.dinero";
#elif defined(Q_OS_WIN)
        QString base = qEnvironmentVariable("APPDATA") + "/Dinero";
#else
        QString base = QDir::homePath() + "/.dinero";
#endif
        return testnet ? (base + "/testnet") : base;
    }

    void updateCookieAuth() {
        QFile f(m_cookiePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_authHeader.clear();
            return;
        }
        
        QByteArray cookie = f.readAll().trimmed(); // "__cookie__:abcdef..."
        m_authHeader = "Basic " + cookie.toBase64(); // HTTP Basic header value
        
        // Watch the cookie file for changes
        if (!m_cookieWatcher->files().contains(m_cookiePath)) {
            m_cookieWatcher->addPath(m_cookiePath);
        }
    }

private slots:
    void onCookieChanged() {
        // Cookie rotated on daemon restart - update auth
        updateCookieAuth();
    }

public:
    QNetworkReply* makeRpcCall(const QString& method, const QJsonArray& params = QJsonArray()) {
        if (m_authHeader.isEmpty()) {
            updateCookieAuth(); // Retry if cookie wasn't ready initially
        }
        
        QNetworkRequest req(QUrl(m_rpcUrl));
        req.setRawHeader("Authorization", m_authHeader);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        
        QJsonObject rpcCall;
        rpcCall["jsonrpc"] = "1.0";
        rpcCall["id"] = "qt";
        rpcCall["method"] = method;
        rpcCall["params"] = params;
        
        QJsonDocument doc(rpcCall);
        QByteArray body = doc.toJson(QJsonDocument::Compact);
        
        return m_nam->post(req, body);
    }
};
```

## Usage Example

```cpp
// In your wallet/miner Qt app
auto* rpcClient = new DineroRpcClient(true); // testnet=true

// Make RPC calls
auto* reply = rpcClient->makeRpcCall("getblockchaininfo");
connect(reply, &QNetworkReply::finished, [reply]() {
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        qDebug() << "Blockchain info:" << doc.object();
    } else {
        qDebug() << "RPC error:" << reply->errorString();
    }
    reply->deleteLater();
});

// Mining example
QJsonArray params;
params.append(1); // number of blocks
params.append("tdin1qhfvq3s8vzvq33vyfjmgjzs5tyyzjl4jxkkcgfr"); // address
auto* mineReply = rpcClient->makeRpcCall("generatetoaddress", params);
```

## Key Features

1. **Automatic Cookie Reading**: Reads from the correct platform-specific path
2. **Cookie Rotation Handling**: Uses `QFileSystemWatcher` to detect when daemon restarts and cookie changes
3. **Network Detection**: Supports both testnet and mainnet cookie paths
4. **Error Resilience**: Retries cookie reading if initially unavailable

## Local Development Workflow

1. Start daemon with cookie auth:
   ```bash
   chmod +x scripts/dev/start-local.sh
   scripts/dev/start-local.sh
   ```

2. Your Qt apps automatically connect using the cookie

3. Stop daemon when done:
   ```bash
   scripts/dev/stop-local.sh
   ```

## Security Benefits

- **No hardcoded passwords** in source code or config files
- **Per-user, per-machine** authentication tokens
- **Automatic rotation** on daemon restart
- **OS-level file permissions** protect the cookie file
- **No network transmission** of permanent credentials

## Future Enhancements

For even better security, consider:
- **Unix domain sockets** (macOS/Linux) or **named pipes** (Windows) for local RPC
- **TLS + certificate pinning** for remote connections
- **OS keychain integration** for remote node credentials
