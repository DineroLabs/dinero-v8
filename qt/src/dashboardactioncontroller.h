// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>

class RpcClient;

namespace dinero::qt::dashboard {

// Owns MyNodeDashboard's explicit peer actions. Sections emit user intent;
// this controller performs confirmations, clipboard writes, and RPC dispatch.
class DashboardActionController : public QObject {
    Q_OBJECT
public:
    using ConfirmCallback = std::function<bool(QWidget*, const QString&, const QString&)>;

    explicit DashboardActionController(RpcClient* rpc,
                                       QWidget* parent_widget,
                                       QObject* parent = nullptr);

    static bool isRelayEndpoint(const QString& endpoint);
    static QString hostForBan(const QString& endpoint);
    static bool isBannableEndpoint(const QString& endpoint);
    static bool isFleetEndpoint(const QString& endpoint);
    static QJsonObject peerDetailsJson(const PeerRow& peer);

    void setConfirmCallbackForTest(ConfirmCallback callback);

public Q_SLOTS:
    void copyEndpoint(const QString& endpoint);
    void copyPeerDetails(const PeerRow& peer);
    void disconnectPeer(const QString& peer_addr);
    void banPeer(const QString& endpoint, int seconds);
    void tryDirectReconnect(const QString& endpoint);
    void dialRelayHint(const HintRow& hint);

Q_SIGNALS:
    void actionStatusChanged(const QString& message);
    void rpcDispatched(const QString& method, const QJsonValue& params, bool named);

private Q_SLOTS:
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

private:
    RpcClient* rpc_{nullptr};
    QWidget* parent_widget_{nullptr};
    ConfirmCallback confirm_callback_;

    bool confirm(const QString& title, const QString& body) const;
    void dispatchArray(const QString& method, const QJsonArray& params);
    void dispatchObject(const QString& method, const QJsonObject& params);
};

}  // namespace dinero::qt::dashboard
