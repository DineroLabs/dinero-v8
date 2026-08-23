#include "connection_manager.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QtMath>

namespace {
QString defaultCookiePath() {
#if defined(Q_OS_WIN)
    const QString appData = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
    return QDir(appData.isEmpty() ? QDir::home().filePath("Dinero") : QDir(appData).filePath("Dinero"))
        .filePath(".cookie");
#elif defined(Q_OS_MAC)
    return QDir(QDir::home().filePath("Library/Application Support/Dinero")).filePath(".cookie");
#else
    return QDir(QDir::home().filePath(".dinero")).filePath(".cookie");
#endif
}
}

ConnectionManager::ConnectionManager(QObject* parent)
    : QObject(parent)
    , state_(Disconnected)
    , daemonUrl_("http://127.0.0.1:20998")
    , cookiePath_(defaultCookiePath())
    , nam_(new QNetworkAccessManager(this))
    , healthCheckTimer_(new QTimer(this))
    , reconnectTimer_(new QTimer(this))
    , queueProcessTimer_(new QTimer(this))
    , processingQueue_(false)
    , healthCheckInterval_(5000)  // 5 seconds
    , maxRetries_(40)
    , currentRetryAttempt_(0)
    , consecutiveFailures_(0)
{
    // Windows slow-start: the embedded daemon can take ~150s to serve RPC on a
    // node with many wallets at high height. With the old cap of 10 the loop
    // reached Failed and stopped retrying before the daemon was ready, so the
    // GUI stayed disconnected even once dinerod came up. Keep retrying well
    // past the init window (backoff caps at 30s). Override via env if needed.
    if (int envRetries = qEnvironmentVariableIntValue("DINERO_QT_MAX_RECONNECT"); envRetries > 0) {
        maxRetries_ = envRetries;
    }

    healthCheckTimer_->setInterval(healthCheckInterval_);
    QObject::connect(healthCheckTimer_, &QTimer::timeout, this, &ConnectionManager::onHealthCheckTimer);
    
    queueProcessTimer_->setInterval(1000);  // Process queue every second
    QObject::connect(queueProcessTimer_, &QTimer::timeout, this, &ConnectionManager::processRpcQueue);
    
    reconnectTimer_->setSingleShot(true);
    QObject::connect(reconnectTimer_, &QTimer::timeout, this, &ConnectionManager::reconnect);
}

ConnectionManager::~ConnectionManager() {
    disconnectFromDaemon();
}

QString ConnectionManager::stateString() const {
    switch (state_) {
        case Disconnected: return "Disconnected";
        case Connecting: return "Connecting...";
        case Connected: return "Connected";
        case Reconnecting: return QString("Reconnecting (%1/%2)").arg(currentRetryAttempt_).arg(maxRetries_);
        case Failed: return "Connection Failed";
        default: return "Unknown";
    }
}

void ConnectionManager::setDaemonUrl(const QString& url) {
    daemonUrl_ = url;
}

void ConnectionManager::setCookiePath(const QString& path) {
    cookiePath_ = path;
}

void ConnectionManager::connectToDaemon() {
    if (state_ == Connected || state_ == Connecting) {
        return;
    }
    
    setState(Connecting);
    currentRetryAttempt_ = 0;
    consecutiveFailures_ = 0;
    
    Q_EMIT statusMessage("Connecting to daemon...", "info");
    
    // Perform initial connection check
    performHealthCheck();
}

void ConnectionManager::disconnectFromDaemon() {
    healthCheckTimer_->stop();
    queueProcessTimer_->stop();
    reconnectTimer_->stop();
    
    if (activeHealthCheck_) {
        // abort() synchronously re-enters the reply's finished handler,
        // which clears activeHealthCheck_ — re-reading the member after
        // abort() dereferences null (SIGSEGV at 0x8 on every app close,
        // 2026-08-22 crash reports). Detach to a local and clear the
        // member BEFORE aborting; a second deleteLater() on the same
        // live object is safe (Qt coalesces deferred deletes).
        QNetworkReply* reply = activeHealthCheck_;
        activeHealthCheck_ = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    
    setState(Disconnected);
    Q_EMIT statusMessage("Disconnected from daemon", "info");
}

void ConnectionManager::reconnect() {
    if (state_ == Connected) {
        return;
    }
    
    if (currentRetryAttempt_ >= maxRetries_) {
        setState(Failed);
        Q_EMIT statusMessage("Failed to connect after " + QString::number(maxRetries_) + " attempts", "error");
        return;
    }
    
    currentRetryAttempt_++;
    setState(Reconnecting);
    Q_EMIT reconnecting(currentRetryAttempt_, maxRetries_);
    
    int backoff = calculateBackoff(currentRetryAttempt_);
    Q_EMIT statusMessage(QString("Reconnecting in %1 seconds...").arg(backoff / 1000), "warning");
    
    reconnectTimer_->setInterval(backoff);
    reconnectTimer_->start();
    
    // Try to connect
    performHealthCheck();
}

void ConnectionManager::call(const QString& method, const QJsonObject& params,
                            std::function<void(const QJsonObject&)> onSuccess,
                            std::function<void(const QString&)> onError)
{
    RpcCall call;
    call.method = method;
    call.params = params;
    call.onSuccess = onSuccess;
    call.onError = onError;
    call.attempts = 0;
    call.namedParams = false;
    
    if (state_ == Connected) {
        // Execute immediately if connected
        executeRpcCall(call);
    } else {
        // Queue for later
        rpcQueue_.append(call);
        
        if (!queueProcessTimer_->isActive()) {
            queueProcessTimer_->start();
        }
        
        Q_EMIT statusMessage(QString("RPC call '%1' queued (daemon not connected)").arg(method), "warning");
    }
}

void ConnectionManager::callNamed(const QString& method, const QJsonObject& params,
                                  std::function<void(const QJsonObject&)> onSuccess,
                                  std::function<void(const QString&)> onError)
{
    RpcCall call;
    call.method = method;
    call.params = params;
    call.onSuccess = onSuccess;
    call.onError = onError;
    call.attempts = 0;
    call.namedParams = true;
    
    if (state_ == Connected) {
        // Execute immediately if connected
        executeRpcCall(call);
    } else {
        // Queue for later
        rpcQueue_.append(call);
        
        if (!queueProcessTimer_->isActive()) {
            queueProcessTimer_->start();
        }
        
        Q_EMIT statusMessage(QString("RPC call '%1' queued (daemon not connected)").arg(method), "warning");
    }
}

void ConnectionManager::setState(ConnectionState newState) {
    if (state_ == newState) {
        return;
    }
    
    ConnectionState oldState = state_;
    state_ = newState;
    
    Q_EMIT stateChanged(newState, oldState);
    
    // Emit specific signals
    if (newState == Connected) {
        consecutiveFailures_ = 0;
        currentRetryAttempt_ = 0;
        healthCheckTimer_->start();
        queueProcessTimer_->start();
        Q_EMIT connected();
        Q_EMIT statusMessage("✅ Connected to daemon", "info");
    } else if (newState == Disconnected) {
        healthCheckTimer_->stop();
        Q_EMIT disconnected();
    }
}

void ConnectionManager::scheduleHealthCheck() {
    if (state_ == Connected) {
        healthCheckTimer_->start();
    }
}

void ConnectionManager::onHealthCheckTimer() {
    if (state_ == Connected) {
        performHealthCheck();
    }
}

void ConnectionManager::performHealthCheck() {
    if (activeHealthCheck_ && activeHealthCheck_->isRunning()) {
        // Previous health check still running, skip this one
        return;
    }
    
    // Read cookie
    QString cookie = readCookie();
    if (cookie.isEmpty()) {
        if (state_ == Connected) {
            Q_EMIT statusMessage("Lost connection to daemon (cookie missing)", "error");
            setState(Reconnecting);
            reconnect();
        } else {
            // The RPC cookie isn't there YET. This is not a permanent failure:
            // the daemon is still starting and hasn't bound RPC / written its
            // .cookie. On a fully-synced node a cold start can take 1-2 minutes
            // to open the DB + build the block index before RPC comes up. The
            // old setState(Failed) here made the GUI give up ~5s in and never
            // reconnect, even though the daemon came up healthy moments later —
            // the "daemon didn't start even after 3 minutes" bug. Keep retrying
            // within the retry budget (reconnect() still Fails after maxRetries_
            // if the cookie genuinely never appears).
            Q_EMIT statusMessage("Waiting for the daemon to finish starting…", "info");
            // Re-entry guard: reconnect() ends with a synchronous
            // performHealthCheck() probe, so calling it unconditionally
            // here recurses (probe -> miss -> reconnect -> probe -> ...)
            // and burns the whole retry budget in milliseconds. Only
            // start the retry machinery when no retry is scheduled yet;
            // afterwards the single-shot backoff timer re-invokes
            // reconnect() on its own schedule.
            if (state_ != Reconnecting) {
                reconnect();
            }
        }
        return;
    }
    
    // Create health check request
    QUrl url{daemonUrl_};
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Basic " + cookie.toUtf8().toBase64()));
    
    QJsonObject rpcRequest;
    rpcRequest["jsonrpc"] = "1.0";
    rpcRequest["id"] = "health_check";
    rpcRequest["method"] = "getblockcount";
    rpcRequest["params"] = QJsonArray();
    
    QJsonDocument doc(rpcRequest);
    QByteArray data = doc.toJson();
    
    QNetworkReply* reply = nam_->post(request, data);
    activeHealthCheck_ = reply;
    
    // Use QPointer to safely track reply lifetime
    QPointer<QNetworkReply> replyPtr(reply);
    
    QObject::connect(reply, &QNetworkReply::finished, [this, replyPtr]() {
        if (!replyPtr || !activeHealthCheck_ || replyPtr != activeHealthCheck_) {
            if (replyPtr) replyPtr->deleteLater();
            return;
        }
        
        if (replyPtr->error() == QNetworkReply::NoError) {
            // Success
            if (state_ != Connected) {
                setState(Connected);
            }
            consecutiveFailures_ = 0;
            
            // Parse response to get block height
            QByteArray response = replyPtr->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(response);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                if (obj.contains("result")) {
                    int blocks = obj["result"].toInt();
                    Q_EMIT blockchainSynced(blocks, blocks);
                }
            }
        } else {
            // Failure
            consecutiveFailures_++;
            
            if (state_ == Connected && consecutiveFailures_ >= 3) {
                Q_EMIT statusMessage("Lost connection to daemon", "error");
                setState(Reconnecting);
                reconnect();
            } else if (state_ == Connecting || state_ == Reconnecting) {
                reconnect();
            }
        }
        
        replyPtr->deleteLater();
        activeHealthCheck_ = nullptr;
    });
    
    // Timeout after 3 seconds - use QPointer to safely check if reply still exists
    QTimer::singleShot(3000, [replyPtr]() {
        if (replyPtr && replyPtr->isRunning()) {
            replyPtr->abort();
        }
    });
}

void ConnectionManager::processRpcQueue() {
    if (state_ != Connected || processingQueue_ || rpcQueue_.isEmpty()) {
        return;
    }
    
    processingQueue_ = true;
    
    // Process up to 5 calls per tick
    int processed = 0;
    while (!rpcQueue_.isEmpty() && processed < 5) {
        RpcCall call = rpcQueue_.takeFirst();
        executeRpcCall(call);
        processed++;
    }
    
    processingQueue_ = false;
    
    if (rpcQueue_.isEmpty()) {
        queueProcessTimer_->stop();
        Q_EMIT statusMessage("All queued RPC calls processed", "info");
    }
}

void ConnectionManager::executeRpcCall(const RpcCall& call) {
    QString cookie = readCookie();
    if (cookie.isEmpty()) {
        if (call.onError) {
            call.onError("Cookie authentication failed");
        }
        Q_EMIT rpcError(call.method, "Cookie authentication failed");
        return;
    }
    
    QUrl url{daemonUrl_};
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Basic " + cookie.toUtf8().toBase64()));
    
    QJsonObject rpcRequest;
    rpcRequest["jsonrpc"] = "1.0";
    rpcRequest["id"] = call.method;
    rpcRequest["method"] = call.method;
    
    if (!call.namedParams) {
        QJsonArray paramsArray;
        if (!call.params.isEmpty()) {
            paramsArray.append(call.params);
        }
        rpcRequest["params"] = paramsArray;
    } else if (call.params.isEmpty()) {
        rpcRequest["params"] = QJsonArray();
    } else {
        rpcRequest["params"] = call.params;
    }
    
    QJsonDocument doc(rpcRequest);
    QByteArray data = doc.toJson();
    
    QNetworkReply* reply = nam_->post(request, data);
    
    // Use QPointer to safely track reply lifetime
    QPointer<QNetworkReply> replyPtr(reply);
    
    QObject::connect(reply, &QNetworkReply::finished, [this, replyPtr, call]() {
        if (!replyPtr) {
            // Reply was deleted before lambda executed
            if (call.onError) {
                call.onError("Request was cancelled");
            }
            return;
        }
        
        if (replyPtr->error() == QNetworkReply::NoError) {
            QByteArray response = replyPtr->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(response);
            
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                
                if (obj.contains("error") && !obj["error"].isNull()) {
                    QString error = obj["error"].toObject()["message"].toString();
                    if (call.onError) {
                        call.onError(error);
                    }
                    Q_EMIT rpcError(call.method, error);
                } else if (obj.contains("result")) {
                    QJsonObject result = obj["result"].toObject();
                    if (call.onSuccess) {
                        call.onSuccess(result);
                    }
                    Q_EMIT rpcSuccess(call.method, result);
                }
            }
        } else {
            QString error = replyPtr->errorString();
            if (call.onError) {
                call.onError(error);
            }
            Q_EMIT rpcError(call.method, error);
        }
        
        replyPtr->deleteLater();
    });
}

QString ConnectionManager::readCookie() {
    QFile file(cookiePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QString cookie = file.readAll().trimmed();
    file.close();
    return cookie;
}

int ConnectionManager::calculateBackoff(int attempt) const {
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s, max 30s
    int backoff = qMin(1000 * qPow(2, attempt - 1), 30000.0);
    return backoff;
}
