#include "websocketclient.h"
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QtMath>

WebSocketClient::WebSocketClient(const QString& serverUrl, QObject* parent)
    : QObject(parent)
    , m_serverUrl(serverUrl)
    , m_pingTimer(new QTimer(this))
    , m_reconnectTimer(new QTimer(this))
{
    // Connect WebSocket signals
    connect(&m_webSocket, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
    connect(&m_webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &WebSocketClient::onError);

    // Setup ping timer
    connect(m_pingTimer, &QTimer::timeout, this, &WebSocketClient::onPingTimeout);

    // Setup reconnect timer
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &WebSocketClient::onReconnectTimeout);
}

WebSocketClient::~WebSocketClient() {
    m_autoReconnect = false;  // Disable auto-reconnect on destruction
    disconnectFromServer();
}

void WebSocketClient::connectToServer() {
    if (m_connected) {
        qDebug() << "WebSocketClient: Already connected";
        return;
    }

    m_manualDisconnect = false;
    qDebug() << "WebSocketClient: Connecting to" << m_serverUrl;

    // Set authentication header if cookie is available
    if (!m_cookieToken.isEmpty()) {
        QUrl wsUrl(m_serverUrl);
        QNetworkRequest req;
        req.setUrl(wsUrl);
        QString authHeader = "Basic " + m_cookieToken.toUtf8().toBase64();
        req.setRawHeader("Authorization", authHeader.toUtf8());
        qDebug() << "WebSocketClient: Using cookie authentication";
        m_webSocket.open(req);
    } else {
        qWarning() << "WebSocketClient: No cookie token - connection may fail if auth required";
        m_webSocket.open(QUrl(m_serverUrl));
    }
}

void WebSocketClient::disconnectFromServer() {
    if (!m_connected) {
        return;
    }

    m_manualDisconnect = true;  // Mark as intentional disconnect
    m_reconnectTimer->stop();   // Stop any pending reconnect attempts
    stopPingTimer();
    m_webSocket.close();
    m_connected = false;
}

bool WebSocketClient::isConnected() const {
    return m_connected;
}

void WebSocketClient::setServerUrl(const QString& serverUrl) {
    if (m_serverUrl == serverUrl) {
        return;  // No change needed
    }

    qDebug() << "WebSocketClient: Changing server URL from" << m_serverUrl << "to" << serverUrl;

    // Disconnect if currently connected
    bool wasConnected = m_connected;
    if (wasConnected) {
        disconnectFromServer();
    }

    // Update URL
    m_serverUrl = serverUrl;

    // Reconnect if we were previously connected
    if (wasConnected && m_autoReconnect) {
        qDebug() << "WebSocketClient: Auto-reconnecting to new URL";
        connectToServer();
    }
}

void WebSocketClient::setAutoReconnect(bool enable) {
    m_autoReconnect = enable;
    if (!enable) {
        m_reconnectTimer->stop();
    }
}

bool WebSocketClient::isAutoReconnectEnabled() const {
    return m_autoReconnect;
}

void WebSocketClient::subscribe(const QString& topic) {
    if (!m_connected) {
        qWarning() << "WebSocketClient: Cannot subscribe - not connected";
        // Store topic for later auto-resubscription
        m_subscribedTopics.insert(topic);
        return;
    }

    QJsonObject params;
    params["topic"] = topic;

    QString id = QString("sub_%1").arg(m_requestId.fetch_add(1));
    sendJsonRpc("subscribe", params, id);

    // Store topic for auto-resubscription on reconnect
    m_subscribedTopics.insert(topic);

    qDebug() << "WebSocketClient: Subscribing to topic:" << topic;
}

void WebSocketClient::unsubscribe(const QString& topic) {
    if (!m_connected) {
        qWarning() << "WebSocketClient: Cannot unsubscribe - not connected";
        // Remove from stored topics
        m_subscribedTopics.remove(topic);
        return;
    }

    QJsonObject params;
    params["topic"] = topic;

    QString id = QString("unsub_%1").arg(m_requestId.fetch_add(1));
    sendJsonRpc("unsubscribe", params, id);

    // Remove from stored topics
    m_subscribedTopics.remove(topic);

    qDebug() << "WebSocketClient: Unsubscribing from topic:" << topic;
}

void WebSocketClient::onConnected() {
    m_connected = true;
    resetReconnectState();
    qDebug() << "WebSocketClient: Connected to" << m_serverUrl;
    Q_EMIT connected();
    startPingTimer();

    // Auto-resubscribe to all previously subscribed topics
    resubscribeAll();
}

void WebSocketClient::onDisconnected() {
    m_connected = false;
    stopPingTimer();
    qDebug() << "WebSocketClient: Disconnected from server";
    Q_EMIT disconnected();

    // Attempt auto-reconnect if enabled and not a manual disconnect
    if (m_autoReconnect && !m_manualDisconnect) {
        scheduleReconnect();
    }
}

void WebSocketClient::onTextMessageReceived(const QString& message) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "WebSocketClient: JSON parse error:" << parseError.errorString();
        return;
    }

    if (!doc.isObject()) {
        qWarning() << "WebSocketClient: Received non-object JSON";
        return;
    }

    QJsonObject obj = doc.object();

    // Phase 2.3 server protocol: events are sent as {"type":"event", "topic":"...", "data":{...}}
    if (obj.contains("type") && obj["type"].toString() == "event") {
        handleEventNotification(obj);
        return;
    }

    // Legacy protocol support: {"method":"subscription", "params":...}
    if (obj.contains("method") && obj["method"].toString() == "subscription") {
        handleSubscriptionNotification(obj);
        return;
    }

    // Check if this is a response to our request
    if (obj.contains("result")) {
        // Subscription confirmation or other RPC response
        qDebug() << "WebSocketClient: Received response:" << obj;
        return;
    }

    if (obj.contains("error")) {
        QJsonObject error = obj["error"].toObject();
        qWarning() << "WebSocketClient: Received error:"
                   << "code=" << error["code"].toInt()
                   << "message=" << error["message"].toString();
        return;
    }
}

void WebSocketClient::onError(QAbstractSocket::SocketError error) {
    QString errorString = m_webSocket.errorString();
    qWarning() << "WebSocketClient: Socket error:" << error << errorString;
    Q_EMIT connectionError(errorString);
    m_connected = false;

    // Schedule reconnect on error if auto-reconnect is enabled
    if (m_autoReconnect && !m_manualDisconnect) {
        scheduleReconnect();
    }
}

void WebSocketClient::onPingTimeout() {
    if (!m_connected) {
        return;
    }

    // Send ping to keep connection alive
    QJsonObject emptyParams;
    QString id = QString("ping_%1").arg(m_requestId.fetch_add(1));
    sendJsonRpc("ping", emptyParams, id);
}

void WebSocketClient::onReconnectTimeout() {
    qDebug() << "WebSocketClient: Attempting reconnection (attempt" << (m_reconnectAttempts + 1) << ")";
    connectToServer();
}

void WebSocketClient::sendJsonRpc(const QString& method, const QJsonObject& params, const QString& id) {
    QJsonObject request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    request["params"] = params;

    if (!id.isEmpty()) {
        request["id"] = id;
    }

    QJsonDocument doc(request);
    QString message = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));

    m_webSocket.sendTextMessage(message);
}

void WebSocketClient::handleSubscriptionNotification(const QJsonObject& message) {
    // Legacy protocol: {"jsonrpc":"2.0", "method":"subscription", "params":{"result":{"topic":"...", "data":{...}}}}

    if (!message.contains("params")) {
        qWarning() << "WebSocketClient: Subscription notification missing params";
        return;
    }

    QJsonObject params = message["params"].toObject();
    if (!params.contains("result")) {
        qWarning() << "WebSocketClient: Subscription notification missing result";
        return;
    }

    QJsonObject result = params["result"].toObject();
    QString topic = result["topic"].toString();
    QJsonObject data = result.value("data").toObject();

    qDebug() << "WebSocketClient: Received subscription event for topic:" << topic;

    // Emit topic-specific signal
    if (topic == "newBlocks") {
        Q_EMIT newBlockReceived(data);
    } else if (topic == "newTransactions") {
        Q_EMIT newTransactionReceived(data);
    } else if (topic == "miningInfo") {
        Q_EMIT miningInfoReceived(data);
    } else if (topic == "networkInfo") {
        Q_EMIT networkInfoReceived(data);
    } else if (topic == "mempool") {
        Q_EMIT mempoolUpdateReceived(data);
    } else if (topic == "syncProgress") {
        Q_EMIT syncProgressReceived(data);
    }

    // Also emit generic subscription event
    Q_EMIT subscriptionEvent(topic, data);
}

void WebSocketClient::handleEventNotification(const QJsonObject& message) {
    // Phase 2.3 protocol: {"type":"event", "topic":"...", "seq":123, "ts":"...", "data":{...}}

    QString topic = message["topic"].toString();
    QJsonObject data = message.value("data").toObject();

    qDebug() << "WebSocketClient: Received event for topic:" << topic
             << "seq:" << message["seq"].toVariant().toString();

    // Emit topic-specific signal
    if (topic == "newBlocks") {
        Q_EMIT newBlockReceived(data);
    } else if (topic == "newTransactions") {
        Q_EMIT newTransactionReceived(data);
    } else if (topic == "miningInfo") {
        Q_EMIT miningInfoReceived(data);
    } else if (topic == "networkInfo") {
        Q_EMIT networkInfoReceived(data);
    } else if (topic == "mempool") {
        Q_EMIT mempoolUpdateReceived(data);
    } else if (topic == "syncProgress") {
        Q_EMIT syncProgressReceived(data);
    }

    // Also emit generic subscription event
    Q_EMIT subscriptionEvent(topic, data);
}

void WebSocketClient::startPingTimer() {
    m_pingTimer->start(PING_INTERVAL_MS);
}

void WebSocketClient::stopPingTimer() {
    m_pingTimer->stop();
}

void WebSocketClient::scheduleReconnect() {
    m_reconnectAttempts++;

    // Exponential backoff: delay = min(INITIAL_DELAY * 2^attempts, MAX_DELAY)
    int delay = qMin(
        INITIAL_RECONNECT_DELAY_MS * qPow(2, m_reconnectAttempts - 1),
        static_cast<double>(MAX_RECONNECT_DELAY_MS)
    );

    qDebug() << "WebSocketClient: Scheduling reconnect in" << delay << "ms (attempt" << m_reconnectAttempts << ")";
    Q_EMIT reconnecting(m_reconnectAttempts, delay);

    m_reconnectTimer->start(delay);
}

void WebSocketClient::resetReconnectState() {
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
}

void WebSocketClient::resubscribeAll() {
    if (m_subscribedTopics.isEmpty()) {
        return;
    }

    qDebug() << "WebSocketClient: Resubscribing to" << m_subscribedTopics.size() << "topics";

    for (const QString& topic : m_subscribedTopics) {
        QJsonObject params;
        params["topic"] = topic;
        QString id = QString("resub_%1").arg(m_requestId.fetch_add(1));
        sendJsonRpc("subscribe", params, id);
        qDebug() << "WebSocketClient: Resubscribed to topic:" << topic;
    }
}

void WebSocketClient::setDatadir(const QString& datadir) {
    m_datadir = datadir;
}

bool WebSocketClient::loadCookie() {
    if (m_datadir.isEmpty()) {
        qWarning() << "WebSocketClient: Cannot load cookie - no datadir set";
        return false;
    }

    QString cookiePath = m_datadir + "/.cookie";
    QFile cookieFile(cookiePath);

    if (!cookieFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "WebSocketClient: Failed to open cookie file:" << cookiePath;
        return false;
    }

    m_cookieToken = cookieFile.readAll().trimmed();
    cookieFile.close();

    if (m_cookieToken.isEmpty()) {
        qWarning() << "WebSocketClient: Cookie file is empty";
        return false;
    }

    qDebug() << "WebSocketClient: Cookie loaded successfully from:" << cookiePath;
    return true;
}
