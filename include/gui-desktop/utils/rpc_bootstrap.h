#pragma once
#include <optional>
#include <QString>
#include <QByteArray>
#include <QUrl>
#include <variant>

struct NodeInfo {
    QUrl rpcUrl;
    QString cookiePath;
    QString network;
    QString schema;
    QString nodeinfoPath; // where we loaded/wrote it
};

class RpcBootstrap {
public:
    // Try full bootstrap. If success, returns NodeInfo and cookie contents.
    // On failure, returns error text you can show in UI.
    static std::variant<std::pair<NodeInfo, QByteArray>, QString>
    bootstrap(const QStringList& argv);

private:
    static std::optional<QString> argValue(const QStringList& argv, const QString& flag);
    static std::optional<QString> findNodeInfoPath(const QStringList& argv);
    static std::optional<NodeInfo> loadNodeInfo(const QString& path, QString& err);
    static bool fileExists(const QString& path);
    static std::optional<QByteArray> readCookie(const QString& path, QString& err);
    static std::optional<QByteArray> rpcPing(const QUrl& url, const QByteArray& cookie, QString& err);

    // daemon auto-launch (best-effort)
    static std::optional<QString> launchDaemonReturnNodeInfoPath(QString& err);
    static bool waitForFile(const QString& path, int timeoutMs);
    static bool writeNodeInfoJson(const QString& path, const NodeInfo& ni, QString& err);

    static QString defaultNodeInfoCandidate(); // per-OS default
    static QString appSupportDir();            // per-OS base dir
    
    // Token provisioning
    static std::optional<QString> createLongLivedToken(const QUrl& url, const QByteArray& cookie, QString& err);
};
