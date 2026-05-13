#pragma once
#include <QtCore>
#include <QtNetwork>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

struct RpcReply {
    QJsonValue result;
    int code = 0;              // JSON-RPC error code (0 means OK)
    QString message;
    bool ok = false;
};

class RpcClientLite {
public:
    RpcClientLite(const QUrl& url, const QByteArray& basicAuth);
    RpcReply call(const QString& method, const QJsonValue& params);

private:
    QUrl url_;
    QByteArray auth_;
};

// Helper to read cookie file and convert to Basic Auth
QByteArray readCookieBasicAuth(const QString& cookiePath);

struct Api {
    RpcClientLite& rpc;
    bool v2 = false;

    void detect();

    // ---- Wallet ----
    RpcReply walletInfo();
    RpcReply walletCreate(const QString& name);
    RpcReply walletLoad(const QString& name);
    RpcReply walletUnload(const QString& name);
    RpcReply walletGetNewAddress();
    RpcReply walletListAddresses();

    // ---- Mining ----
    RpcReply miningInfo();
    RpcReply miningStart(int threads);
    RpcReply miningStop();
    RpcReply miningSetPayout(const QString& addr);

    // ---- Chain (keep legacy names unless you namespaced them) ----
    RpcReply getBlockchainInfo();
    RpcReply getBlockCount();
    RpcReply getBestBlockHash();
    RpcReply getBlock(const QString& hash, int verbosity = 2);
};
