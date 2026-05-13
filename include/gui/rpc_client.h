#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonJson::Value>
#include <QUrl>
#include <QTimer>
#include <functional>

class RpcClient : public QObject {
    Q_OBJECT
public:
    explicit RpcClient(QObject* parent=nullptr);
    RpcClient(QUrl url, QByteArray basicAuth, QObject* parent=nullptr);

    // Enhanced Reply structure for better error handling
    struct Reply { 
        QJsonObject json; 
        QString error; 
        int http = 0; 
        bool isValid() const { return error.isEmpty() && http == 200; }
    };
    using Callback = std::function<void(const Reply&)>;
    
    // Legacy callback for backward compatibility
    using LegacyCallback = std::function<void(QJsonJson::Value result, QString error, int code)>;

    void setUrl(const QUrl& url);
    const QUrl& url() const { return endpoint_; }
    void setCookieFile(const QString& path);
    void setActiveWallet(const QString& name);
    QString getActiveWallet() const { return activeWallet_; }

    // Enhanced call with per-request timeout and request ID tracking
    quint64 call(const QString& method, const QJsonArray& params, Callback cb, int timeoutMs = 5000);
    quint64 call(const QString& method, const QJsonObject& params, Callback cb, int timeoutMs = 5000);
    
    // Legacy call method for backward compatibility
    void call(const QString& method, const QJsonJson::Value& params, LegacyCallback cb);

    // Optional sugar for array/object params (legacy)
    inline void callA(const QString& method, const QJsonArray& params, LegacyCallback cb) { 
        call(method, QJsonJson::Value(params), std::move(cb)); 
    }
    inline void callO(const QString& method, const QJsonObject& params, LegacyCallback cb) { 
        call(method, QJsonJson::Value(params), std::move(cb)); 
    }
    
    // Configuration and probing
    bool configure(const QString& cookiePath, int rpcPort);
    void probe();
    
    // Status checks
    bool isConfigured() const { return isConfigured_; }
    bool isConnected() const { return isConnected_; }
    
    // V2-only RPC - no legacy compatibility needed

signals:
    void cookieReloaded();
    void authFailed(int httpStatus, QString error);
    void activeWalletChanged(const QString& walletName);
    void ready(); // Emitted when RPC is ready for use

private:
    void scheduleRetry();
    
    QNetworkAccessManager nam_;
    QUrl endpoint_;
    QUrl baseUrl_;
    QString cookiePath_;
    QString activeWallet_;
    QByteArray authHeader_;
    QFileSystemWatcher watcher_;
    QTimer* requestTimer_;
    int rpcPort_ = 0;
    bool isConfigured_ = false;
    bool isConnected_ = false;
    
    // Request tracking for enhanced API
    QHash<quint64, QMetaObject::Connection> connections_;
    QHash<quint64, QTimer*> timers_;
    quint64 nextId_ = 1;

    bool loadCookie();
    void ensureWatcher();
    void doRequest(const QString& method, const QJsonJson::Value& params, LegacyCallback cb);
    void doEnhancedRequest(const QString& method, const QJsonJson::Value& params, Callback cb, int timeoutMs);
    QString namespacedMethod(const QString& method) const;
};
