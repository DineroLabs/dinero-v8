#include "rpc_api.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QDir>

RpcClientLite::RpcClientLite(const QUrl& url, const QByteArray& basicAuth)
    : url_(url), auth_(basicAuth) {}

RpcReply RpcClientLite::call(const QString& method, const QJsonValue& params) {
    QNetworkRequest req(url_);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Basic " + auth_);

    QJsonObject body{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", method},
        {"params", params.isUndefined() ? QJsonArray{} : params}
    };
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkAccessManager nam;
    QEventLoop loop;
    QNetworkReply* r = nam.post(req, payload);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    RpcReply out;
    if (r->error() != QNetworkReply::NoError) {
        out.code = -32000;
        out.message = r->errorString();
        r->deleteLater();
        return out;
    }

    const auto doc = QJsonDocument::fromJson(r->readAll());
    r->deleteLater();
    if (!doc.isObject()) {
        out.code = -32700;
        out.message = "Malformed JSON";
        return out;
    }
    const auto obj = doc.object();
    if (obj.contains("error") && obj["error"].isObject()) {
        const auto e = obj["error"].toObject();
        out.code = e.contains("code") ? e["code"].toInt() : -32000;
        out.message = e.contains("message") ? e["message"].toString() : "Unknown error";
        return out;
    }
    out.result = obj.contains("result") ? obj["result"] : QJsonValue();
    out.ok = true;
    return out;
}

QByteArray readCookieBasicAuth(const QString& cookiePath) {
    QFile f(cookiePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    auto line = f.readAll().trimmed(); // "user:pass"
    return line.toBase64();
}

void Api::detect() {
    // 1) Try explicit v2 probe
    if (auto r = rpc.call("rpc.version", QJsonObject{}); r.ok) {
        const auto maj = r.result.toObject().contains("major") ? r.result.toObject()["major"].toInt() : 0;
        v2 = (maj >= 2);
        if (v2) return;
    }
    // 2) Try a v2-unique name; if "Method not found" -> legacy
    if (auto r = rpc.call("wallet.info", QJsonObject{}); r.code == -32601) {
        v2 = false; 
        return;
    }
    // If it didn't 32601, assume v2 works
    v2 = true;
}

// ---- Wallet ----
RpcReply Api::walletInfo() {
    return v2 ? rpc.call("wallet.info", QJsonObject{})
              : rpc.call("getwalletinfo", QJsonArray{});
}

RpcReply Api::walletCreate(const QString& name) {
    return v2 ? rpc.call("wallet.create", QJsonObject{{"name", name}})
              : rpc.call("createwallet", QJsonArray{name});
}

RpcReply Api::walletLoad(const QString& name) {
    return v2 ? rpc.call("wallet.load", QJsonObject{{"name", name}})
              : rpc.call("loadwallet", QJsonArray{name});
}

RpcReply Api::walletUnload(const QString& name) {
    return v2 ? rpc.call("wallet.unload", QJsonObject{{"name", name}})
              : rpc.call("unloadwallet", QJsonArray{name});
}

RpcReply Api::walletGetNewAddress() {
    return v2 ? rpc.call("wallet.getnewaddress", QJsonObject{})
              : rpc.call("getnewaddress", QJsonArray{});
}

RpcReply Api::walletListAddresses() {
    return v2 ? rpc.call("wallet.listaddresses", QJsonObject{})
              : rpc.call("listaddresses", QJsonArray{});
}

// ---- Mining ----
RpcReply Api::miningInfo() {
    return v2 ? rpc.call("mining.info", QJsonObject{})
              : rpc.call("getmininginfo", QJsonArray{});
}

RpcReply Api::miningStart(int threads) {
    return v2 ? rpc.call("mining.start", QJsonObject{{"threads", threads}})
              : rpc.call("startmining", QJsonArray{threads});
}

RpcReply Api::miningStop() {
    return v2 ? rpc.call("mining.stop", QJsonObject{})
              : rpc.call("stopmining", QJsonArray{});
}

RpcReply Api::miningSetPayout(const QString& addr) {
    return v2 ? rpc.call("mining.setpayoutaddress", QJsonObject{{"address", addr}})
              : rpc.call("setminingaddress", QJsonArray{addr});
}

// ---- Chain (vNext dotted names) ----
RpcReply Api::getBlockchainInfo() {
    return rpc.call("blockchain.getinfo", QJsonArray{});
}

RpcReply Api::getBlockCount() {
    return rpc.call("blockchain.getblockcount", QJsonArray{});
}

RpcReply Api::getBestBlockHash() {
    return rpc.call("blockchain.getbestblockhash", QJsonArray{});
}

RpcReply Api::getBlock(const QString& hash, int verbosity) {
    return rpc.call("blockchain.getblock", QJsonArray{hash, verbosity});
}
