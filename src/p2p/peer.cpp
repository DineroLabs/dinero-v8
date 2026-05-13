#include "peer.h"
#include <QDataStream>
#include <QDateTime>
#include <QDebug>

Peer::Peer(const QHostAddress& host, quint16 port, QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
    , lastPingNonce_(0)
    , state_(Connecting)
    , dosScore_(0)
    , isInbound_(false)
    , peerHeight_(0)
    , lastPingMs_(0)
    , connectedTime_(QDateTime::currentMSecsSinceEpoch())
    , services_(0)
    , versionSent_(false)
    , versionReceived_(false)
    , verackReceived_(false)
{
    connect(socket_, &QTcpSocket::readyRead, this, &Peer::onReadyRead);
    connect(socket_, &QTcpSocket::connected, this, &Peer::onConnected);
    connect(socket_, &QTcpSocket::errorOccurred, this, &Peer::onError);
    
    socket_->connectToHost(host, port);
    id_ = QString("%1:%2").arg(host.toString()).arg(port);
    
    qDebug() << "[P2P] Connecting to" << id_;
}

Peer::Peer(QTcpSocket* accepted, QObject* parent)
    : QObject(parent)
    , socket_(accepted)
    , lastPingNonce_(0)
    , state_(Handshake)
    , dosScore_(0)
    , isInbound_(true)
    , peerHeight_(0)
    , lastPingMs_(0)
    , connectedTime_(QDateTime::currentMSecsSinceEpoch())
    , services_(0)
    , versionSent_(false)
    , versionReceived_(false)
    , verackReceived_(false)
{
    socket_->setParent(this);
    connect(socket_, &QTcpSocket::readyRead, this, &Peer::onReadyRead);
    connect(socket_, &QTcpSocket::errorOccurred, this, &Peer::onError);
    
    id_ = QString("%1:%2").arg(socket_->peerAddress().toString()).arg(socket_->peerPort());
    
    qDebug() << "[P2P] Accepted inbound connection from" << id_;
    
    // For inbound connections, wait for their version message first
}

void Peer::start() {
    if (socket_->state() == QAbstractSocket::ConnectedState) {
        onConnected();
    }
}

void Peer::onConnected() {
    qDebug() << "[P2P]" << id_ << "connected";
    state_ = Handshake;
    
    // For outbound connections, send version immediately
    if (!isInbound_) {
        sendVersion();
    }
}

void Peer::send(const QByteArray& cmd, const QByteArray& payload) {
    if (state_ == Closed) {
        return;
    }
    
    QByteArray message = p2p::makeMessage(cmd, payload);
    qint64 written = socket_->write(message);
    
    if (written != message.size()) {
        qWarning() << "[P2P]" << id_ << "partial write:" << written << "of" << message.size();
    }
}

void Peer::sendVersion() {
    if (versionSent_) {
        return;
    }
    
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    
    ds << qint32(1);  // Protocol version
    ds << qint64(1);  // Services (NODE_NETWORK)
    ds << qint64(QDateTime::currentSecsSinceEpoch());  // Timestamp
    
    // User agent
    QString userAgent = "Dinero/2.0.0-beta.2";
    QByteArray userAgentBytes = userAgent.toUtf8();
    ds << userAgentBytes;
    
    ds << qint32(0);  // Start height (TODO: get from blockchain)
    
    send("version", payload);
    versionSent_ = true;
    
    qDebug() << "[P2P]" << id_ << "sent version";
}

void Peer::onReadyRead() {
    inBuffer_ += socket_->readAll();
    
    QByteArray cmd, payload;
    while (p2p::parseOne(inBuffer_, cmd, payload)) {
        handleMessage(cmd, payload);
    }
}

void Peer::onError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error)
    close(QString("Socket error: %1").arg(socket_->errorString()));
}

void Peer::close(const QString& reason) {
    if (state_ == Closed) {
        return;
    }
    
    qDebug() << "[P2P]" << id_ << "closing:" << reason;
    
    state_ = Closed;
    pingTimer_.stop();
    socket_->disconnectFromHost();
    
    emit closed(this, reason);
}

void Peer::onPingTick() {
    lastPingNonce_ = QRandomGenerator::global()->generate64();
    
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << lastPingNonce_;
    
    send("ping", payload);
}

void Peer::handleMessage(const QByteArray& cmd, const QByteArray& payload) {
    // Basic DoS protection
    if (payload.size() > 2 * 1024 * 1024) {  // 2MB limit
        addDosScore(10, "oversized message");
        return;
    }
    
    if (cmd == "version") {
        if (versionReceived_) {
            addDosScore(5, "duplicate version");
            return;
        }
        
        versionReceived_ = true;
        
        // Parse version message
        QDataStream ds(payload);
        ds.setByteOrder(QDataStream::LittleEndian);
        
        qint32 version;
        qint64 services, timestamp;
        QByteArray userAgent;
        qint32 startHeight;
        
        ds >> version >> services >> timestamp >> userAgent >> startHeight;
        
        qDebug() << "[P2P]" << id_ << "version:" << version << "services:" << services 
                 << "agent:" << userAgent << "height:" << startHeight;
        
        // Send verack
        send("verack", QByteArray());
        
        // If we haven't sent our version yet (inbound), send it now
        if (!versionSent_) {
            sendVersion();
        }
        
        return;
    }
    
    if (cmd == "verack") {
        if (!versionSent_ || verackReceived_) {
            addDosScore(5, "unexpected verack");
            return;
        }
        
        verackReceived_ = true;
        
        // Handshake complete if we've received both version and verack
        if (versionReceived_ && verackReceived_) {
            state_ = Ready;
            
            // Start ping timer
            pingTimer_.setInterval(25000);  // 25 seconds
            connect(&pingTimer_, &QTimer::timeout, this, &Peer::onPingTick);
            pingTimer_.start();
            
            qDebug() << "[P2P]" << id_ << "handshake complete";
            emit handshakeDone(this);
        }
        
        return;
    }
    
    if (cmd == "ping") {
        QDataStream ds(payload);
        ds.setByteOrder(QDataStream::LittleEndian);
        quint64 nonce;
        ds >> nonce;
        
        QByteArray pongPayload;
        QDataStream pongDs(&pongPayload, QIODevice::WriteOnly);
        pongDs.setByteOrder(QDataStream::LittleEndian);
        pongDs << nonce;
        
        send("pong", pongPayload);
        return;
    }
    
    if (cmd == "pong") {
        // TODO: Verify nonce matches our ping
        return;
    }
    
    // For other messages, forward to peer manager
    if (state_ == Ready) {
        emit gotMessage(this, cmd, payload);
    } else {
        addDosScore(2, "message before handshake complete");
    }
}

void Peer::addDosScore(int points, const QString& reason) {
    dosScore_ += points;
    
    if (!reason.isEmpty()) {
        qWarning() << "[P2P]" << id_ << "DoS +" << points << ":" << reason << "(total:" << dosScore_ << ")";
    }
    
    if (dosScore_ >= 100) {
        close("DoS score exceeded");
    }
}

QString Peer::address() const {
    if (socket_) {
        return QString("%1:%2").arg(socket_->peerAddress().toString()).arg(socket_->peerPort());
    }
    return id_;
}
