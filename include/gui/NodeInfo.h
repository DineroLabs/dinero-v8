#pragma once
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>

struct NodeInfo {
    QString rpcUrl;
    QString cookiePath;
    QString wsUrl;
    int pid;
};

inline std::optional<NodeInfo> loadNodeInfo(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const auto obj = QJsonDocument::fromJson(f.readAll()).object();
    NodeInfo ni;
    ni.rpcUrl     = obj["rpc"].toObject()["url"].toString();      // e.g. "http://127.0.0.1:57344"
    ni.cookiePath = obj["cookie"].toString();                     // e.g. ".../data/mainnet/.cookie"
    ni.wsUrl      = obj["ws"].toObject()["url"].toString();       // e.g. "ws://127.0.0.1:57345/ws"
    ni.pid        = obj["pid"].toInt();
    if (ni.rpcUrl.isEmpty() || ni.cookiePath.isEmpty()) return std::nullopt;
    return ni;
}
