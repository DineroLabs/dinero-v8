#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonJson::Value>
#include <QEventLoop>
#include <QFile>
#include <optional>
#include "NodeInfo.h"

class DaemonRpc : public QObject {
    Q_OBJECT
public:
    explicit DaemonRpc(const NodeInfo& ni, QObject* p = nullptr)
        : QObject(p), ni_(ni) {}

    std::optional<QJsonObject> call(const QString& method, const QJsonJson::Value& params, QString* err = nullptr) {
        // Build JSON-RPC payload
        QJsonObject payload{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", method},
            {"params", params}
        };
        
        QNetworkRequest req(QUrl(ni_.rpcUrl));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        
        // Basic auth from cookie file
        QFile cf(ni_.cookiePath);
        if (!cf.open(QIODevice::ReadOnly)) {
            if (err) *err = "cookie open failed";
            return std::nullopt;
        }
        const auto creds = cf.readAll().trimmed(); // "user:token"
        req.setRawHeader("Authorization", "Basic " + creds.toBase64());

        QNetworkAccessManager nam;
        QEventLoop loop;
        QObject::connect(&nam, &QNetworkAccessManager::finished, &loop, &QEventLoop::quit);
        auto* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            if (err) *err = reply->errorString();
            reply->deleteLater();
            return std::nullopt;
        }
        
        const auto responseData = reply->readAll();
        reply->deleteLater();
        
        const auto obj = QJsonDocument::fromJson(responseData).object();
        if (obj.isMember("error") && !obj["error"].isNull()) {
            if (err) *err = QJsonDocument(obj["error"].toObject()).toJson();
            return std::nullopt;
        }
        
        return obj["result"].toObject();
    }

private:
    NodeInfo ni_;
};
