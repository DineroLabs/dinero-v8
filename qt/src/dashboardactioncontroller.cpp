// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dashboardactioncontroller.h"

#include "rpcclient.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace dinero::qt::dashboard {

namespace {

QString endpointHost(const QString& endpoint) {
    const QString trimmed = endpoint.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("relay:"))) {
        return {};
    }

    if (trimmed.startsWith('[')) {
        const int close = trimmed.indexOf(']');
        if (close <= 1) return {};
        return trimmed.mid(1, close - 1);
    }

    const int colon_count = trimmed.count(':');
    if (colon_count == 1) {
        return trimmed.left(trimmed.lastIndexOf(':'));
    }
    if (colon_count > 1) {
        // Bare IPv6 without a port is acceptable for setban. Bracketed
        // host:port is handled above.
        return trimmed;
    }
    return trimmed;
}

QString compact(const QString& s) {
    return s.size() <= 18 ? s : s.left(9) + QStringLiteral("…") + s.right(8);
}

QString seederOptInKey() {
    return QStringLiteral("dashboard/seeder_opt_in_v1");
}

}  // namespace

DashboardActionController::DashboardActionController(
        RpcClient* rpc, QWidget* parent_widget, QObject* parent)
    : QObject(parent)
    , rpc_(rpc)
    , parent_widget_(parent_widget) {
    confirm_callback_ = [](QWidget* parent, const QString& title, const QString& body) {
        return QMessageBox::question(parent, title, body,
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No) == QMessageBox::Yes;
    };
    seeder_opted_in_ = QSettings().value(seederOptInKey(), false).toBool();
    seeder_status_ = seeder_opted_in_ ? tr("Ready") : tr("Off");
    if (rpc_) {
        connect(rpc_, &RpcClient::rpcResult,
                this, &DashboardActionController::onRpcResult);
        connect(rpc_, &RpcClient::rpcError,
                this, &DashboardActionController::onRpcError);
        seeder_status_timer_ = new QTimer(this);
        seeder_status_timer_->setInterval(5000);
        connect(seeder_status_timer_, &QTimer::timeout,
                this, &DashboardActionController::refreshSeederStatus);
        seeder_status_timer_->start();
        QTimer::singleShot(0, this, &DashboardActionController::refreshSeederStatus);
    }
    QTimer::singleShot(0, this, [this]() {
        emitSeederState();
    });
}

bool DashboardActionController::isRelayEndpoint(const QString& endpoint) {
    return endpoint.trimmed().startsWith(QStringLiteral("relay:"));
}

QString DashboardActionController::hostForBan(const QString& endpoint) {
    const QString host = endpointHost(endpoint);
    QHostAddress addr;
    if (!addr.setAddress(host)) return {};
    return host;
}

bool DashboardActionController::isBannableEndpoint(const QString& endpoint) {
    return !hostForBan(endpoint).isEmpty() && !isRelayEndpoint(endpoint);
}

bool DashboardActionController::isFleetEndpoint(const QString& endpoint) {
    const QString host = endpointHost(endpoint);
    return host == QStringLiteral("172.93.160.131") ||
           host == QStringLiteral("173.249.195.59") ||
           host == QStringLiteral("72.18.214.120") ||
           host == QStringLiteral("96.9.226.98");
}

QJsonObject DashboardActionController::peerDetailsJson(const PeerRow& peer) {
    return QJsonObject{
        {QStringLiteral("addr"), peer.addr},
        {QStringLiteral("via_relay"), peer.via_relay},
        {QStringLiteral("relay_via_addr"), peer.relay_via_addr},
        {QStringLiteral("inbound"), peer.is_inbound},
        {QStringLiteral("fleet_name"), peer.fleet_name},
        {QStringLiteral("height"), peer.height},
        {QStringLiteral("ping_ms"), peer.ping_ms},
        {QStringLiteral("quality_score"), peer.quality_score},
        {QStringLiteral("handshake_complete"), peer.handshake_complete},
        {QStringLiteral("services"), QStringLiteral("0x%1").arg(peer.services, 0, 16)},
        {QStringLiteral("subversion"), peer.subversion},
        {QStringLiteral("bytes_sent"), peer.bytes_sent},
        {QStringLiteral("bytes_recv"), peer.bytes_recv},
        {QStringLiteral("connected_for_seconds"), qint64(peer.connected_for.count())},
        {QStringLiteral("last_message_ago_seconds"), qint64(peer.last_message_ago.count())},
    };
}

void DashboardActionController::setConfirmCallbackForTest(ConfirmCallback callback) {
    confirm_callback_ = std::move(callback);
}

void DashboardActionController::copyEndpoint(const QString& endpoint) {
    if (endpoint.trimmed().isEmpty()) {
        Q_EMIT actionStatusChanged(tr("No endpoint to copy"));
        return;
    }
    if (auto* clipboard = QApplication::clipboard()) {
        clipboard->setText(endpoint);
    }
    Q_EMIT actionStatusChanged(tr("Copied %1").arg(compact(endpoint)));
}

void DashboardActionController::copyPeerDetails(const PeerRow& peer) {
    const auto json = QJsonDocument(peerDetailsJson(peer))
        .toJson(QJsonDocument::Indented);
    if (auto* clipboard = QApplication::clipboard()) {
        clipboard->setText(QString::fromUtf8(json));
    }
    Q_EMIT actionStatusChanged(tr("Copied peer details"));
}

void DashboardActionController::disconnectPeer(const QString& peer_addr) {
    if (peer_addr.trimmed().isEmpty()) {
        Q_EMIT actionStatusChanged(tr("No peer selected"));
        return;
    }
    if (!confirm(tr("Disconnect peer?"),
                 tr("Disconnect %1 from this node? The daemon may reconnect later.")
                     .arg(peer_addr))) {
        Q_EMIT actionStatusChanged(tr("Disconnect cancelled"));
        return;
    }
    dispatchArray(QStringLiteral("disconnectnode"), QJsonArray{peer_addr});
    Q_EMIT actionStatusChanged(tr("Disconnect requested for %1").arg(compact(peer_addr)));
}

void DashboardActionController::banPeer(const QString& endpoint, int seconds) {
    const QString host = hostForBan(endpoint);
    if (host.isEmpty()) {
        Q_EMIT actionStatusChanged(tr("Ban unavailable for %1").arg(endpoint));
        return;
    }

    QString body = tr("Ban %1 for %2 seconds?").arg(host).arg(seconds);
    if (isFleetEndpoint(endpoint)) {
        body += tr("\n\nThis is one of your configured bootstrap peers. Ban only if you are debugging.");
    }
    if (!confirm(tr("Ban peer?"), body)) {
        Q_EMIT actionStatusChanged(tr("Ban cancelled"));
        return;
    }

    dispatchArray(QStringLiteral("setban"),
                  QJsonArray{host, QStringLiteral("add"), seconds, false});
    Q_EMIT actionStatusChanged(tr("Ban requested for %1").arg(host));
}

void DashboardActionController::tryDirectReconnect(const QString& endpoint) {
    if (endpoint.trimmed().isEmpty() || isRelayEndpoint(endpoint)) {
        Q_EMIT actionStatusChanged(tr("Direct reconnect unavailable"));
        return;
    }
    dispatchArray(QStringLiteral("addnode"),
                  QJsonArray{endpoint, QStringLiteral("onetry")});
    Q_EMIT actionStatusChanged(tr("Reconnect requested for %1").arg(compact(endpoint)));
}

void DashboardActionController::dialRelayHint(const HintRow& hint) {
    if (hint.target_node_id_hex.trimmed().isEmpty()) {
        Q_EMIT actionStatusChanged(tr("Relay hint has no target"));
        return;
    }
    QJsonObject params{
        {QStringLiteral("target_node_id_hex"), hint.target_node_id_hex},
        {QStringLiteral("dry_run"), false},
    };
    if (!hint.endpoint.trimmed().isEmpty() &&
        hint.endpoint != QStringLiteral("(no addr)")) {
        params.insert(QStringLiteral("relay_endpoint"), hint.endpoint);
    }
    dispatchObject(QStringLiteral("relayhints.dial"), params);
    Q_EMIT actionStatusChanged(tr("Relay dial submitted for %1")
        .arg(compact(hint.target_node_id_hex)));
}

void DashboardActionController::setSeederOptIn(bool enabled) {
    seeder_opted_in_ = enabled;
    QSettings().setValue(seederOptInKey(), enabled);
    if (!enabled && seeder_running_) {
        stopSeeder();
        seeder_status_ = tr("Stopping");
    } else {
        seeder_status_ = enabled ? (seeder_running_ ? tr("Running") : tr("Ready"))
                                 : tr("Off");
    }
    emitSeederState();
}

void DashboardActionController::startSeeder() {
    if (!seeder_opted_in_) {
        seeder_status_ = tr("Switch to Yes first");
        emitSeederState();
        return;
    }
    const QString binary = defaultSeederPath();
    if (binary.isEmpty()) {
        seeder_status_ = tr("dinero-seeder not found");
        emitSeederState();
        return;
    }
    dispatchObject(QStringLiteral("seeder.start"), QJsonObject{
        {QStringLiteral("binary"), binary},
    });
    seeder_status_ = tr("Starting");
    emitSeederState();
}

void DashboardActionController::stopSeeder() {
    dispatchObject(QStringLiteral("seeder.stop"), QJsonObject{});
    seeder_status_ = tr("Stopping");
    emitSeederState();
}

void DashboardActionController::refreshSeederStatus() {
    dispatchObject(QStringLiteral("seeder.status"), QJsonObject{});
}

bool DashboardActionController::confirm(const QString& title,
                                        const QString& body) const {
    return confirm_callback_ ? confirm_callback_(parent_widget_, title, body)
                             : false;
}

void DashboardActionController::dispatchArray(const QString& method,
                                              const QJsonArray& params) {
    Q_EMIT rpcDispatched(method, QJsonValue(params), false);
    if (rpc_) rpc_->call(method, params);
}

void DashboardActionController::dispatchObject(const QString& method,
                                               const QJsonObject& params) {
    Q_EMIT rpcDispatched(method, QJsonValue(params), true);
    if (rpc_) rpc_->callNamed(method, params);
}

void DashboardActionController::emitSeederState() {
    Q_EMIT seederStateChanged(seeder_opted_in_, seeder_running_, seeder_status_);
}

QString DashboardActionController::defaultSeederPath() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    auto add = [&candidates](const QString& path) {
        if (path.trimmed().isEmpty()) return;
        const QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            const QString abs = info.absoluteFilePath();
            if (!candidates.contains(abs)) candidates << abs;
        }
    };

#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("dinero-seeder.exe");
    add(QDir(app_dir).absoluteFilePath(name));
    add(QDir(app_dir).absoluteFilePath("../" + name));
#else
    const QString name = QStringLiteral("dinero-seeder");
    add(QDir(app_dir).absoluteFilePath(name));
#if defined(Q_OS_MAC)
    add(QDir(app_dir).absoluteFilePath("../Resources/" + name));
    add(QDir(app_dir).absoluteFilePath("../../../../" + name));
#else
    add(QDir(app_dir).absoluteFilePath("../" + name));
#endif
#endif
    add(QStandardPaths::findExecutable(name));
    return candidates.isEmpty() ? QString() : candidates.first();
}

void DashboardActionController::onRpcResult(const QString& method,
                                            const QJsonValue& result) {
    if (method == QStringLiteral("relayhints.dial")) {
        const auto obj = result.toObject();
        const QString status = obj.value(QStringLiteral("status")).toString();
        Q_EMIT actionStatusChanged(status.isEmpty()
            ? tr("Relay dial result received")
            : tr("Relay dial: %1").arg(status));
    } else if (method == QStringLiteral("addnode") ||
               method == QStringLiteral("disconnectnode") ||
               method == QStringLiteral("setban")) {
        Q_EMIT actionStatusChanged(tr("%1 accepted").arg(method));
    } else if (method == QStringLiteral("seeder.status") ||
               method == QStringLiteral("seeder.start") ||
               method == QStringLiteral("seeder.stop")) {
        const auto obj = result.toObject();
        seeder_running_ = obj.value(QStringLiteral("running")).toBool(false);
        if (method == QStringLiteral("seeder.status")) {
            seeder_status_ = seeder_running_ ? tr("Running")
                                             : (seeder_opted_in_ ? tr("Ready") : tr("Off"));
        } else {
            seeder_status_ = seeder_running_ ? tr("Running") : tr("Stopped");
            Q_EMIT actionStatusChanged(tr("%1 accepted").arg(method));
        }
        emitSeederState();
    }
}

void DashboardActionController::onRpcError(const QString& method, int code,
                                           const QString& message) {
    if (method == QStringLiteral("relayhints.dial") ||
        method == QStringLiteral("addnode") ||
        method == QStringLiteral("disconnectnode") ||
        method == QStringLiteral("setban") ||
        method == QStringLiteral("seeder.status") ||
        method == QStringLiteral("seeder.start") ||
        method == QStringLiteral("seeder.stop")) {
        Q_EMIT actionStatusChanged(tr("%1 failed (%2): %3")
            .arg(method).arg(code).arg(message));
        if (method.startsWith(QStringLiteral("seeder."))) {
            seeder_status_ = tr("Error: %1").arg(message);
            emitSeederState();
        }
    }
}

}  // namespace dinero::qt::dashboard
