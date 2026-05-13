#include "peer.h"
#include <QDataStream>
#include <QDateTime>
#include <QDebug>

#ifndef DINERO_CLI_GIT_SHA
#define DINERO_CLI_GIT_SHA "unknown"
#endif

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

    // Protocol version (4 bytes) - 70016 = Utreexo support
    ds << qint32(70016);

    // Services (8 bytes) - NODE_NETWORK | NODE_UTREEXO | NODE_UTREEXO_BRIDGE
    // NODE_NETWORK = 1, NODE_UTREEXO = 1<<24, NODE_UTREEXO_BRIDGE = 1<<25
    constexpr quint64 NODE_NETWORK = 1;
    constexpr quint64 NODE_UTREEXO = 1ULL << 24;
    constexpr quint64 NODE_UTREEXO_BRIDGE = 1ULL << 25;
    quint64 services = NODE_NETWORK | NODE_UTREEXO | NODE_UTREEXO_BRIDGE;
    ds << services;

    // Timestamp (8 bytes)
    ds << qint64(QDateTime::currentSecsSinceEpoch());

    // addrRecv (26 bytes): services(8) + IPv6-mapped address(16) + port(2)
    ds << services;                          // 8 bytes: receiver services
    ds.writeRawData("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\x00\x00\x00\x00", 16);  // IPv4 mapped to IPv6
    ds << quint16(qToBigEndian(quint16(20999)));  // Port in network byte order

    // addrFrom (26 bytes): our address
    ds << services;                          // 8 bytes: our services
    ds.writeRawData("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\x00\x00\x00\x00", 16);
    ds << quint16(qToBigEndian(quint16(20999)));

    // Nonce (8 bytes) - random value to detect self-connections
    quint64 nonce = QRandomGenerator::global()->generate64();
    ds << nonce;

    // User agent (var_str): length prefix + string bytes
    QByteArray userAgent = QByteArray("/dinerod:") + DINERO_CLI_GIT_SHA + "/";
    quint8 uaLen = static_cast<quint8>(userAgent.size());
    ds << uaLen;
    ds.writeRawData(userAgent.constData(), userAgent.size());

    // Start height (4 bytes) - TODO: get real height from blockchain
    ds << qint32(0);

    // Relay (1 byte) - accept tx relay
    ds << quint8(1);

    send("version", payload);
    versionSent_ = true;
    services_ = services;

    qDebug() << "[P2P]" << id_ << "sent version (protocol=70016, services=" << services << ")";
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
        
        // Parse version message (Bitcoin wire format)
        // Layout: version(4) + services(8) + timestamp(8) + addrRecv(26) + addrFrom(26) + nonce(8) + userAgent(var_str) + startHeight(4) + relay(1)
        if (payload.size() < 85) {  // Minimum size
            addDosScore(5, "version too short");
            return;
        }

        const char* p = payload.constData();
        int offset = 0;

        // Version (4 bytes)
        qint32 version;
        memcpy(&version, p + offset, 4);
        offset += 4;

        // Services (8 bytes)
        quint64 services;
        memcpy(&services, p + offset, 8);
        offset += 8;
        peerServices_ = services;

        // Timestamp (8 bytes)
        qint64 timestamp;
        memcpy(&timestamp, p + offset, 8);
        offset += 8;

        // Skip addrRecv (26 bytes) and addrFrom (26 bytes)
        offset += 52;

        // Nonce (8 bytes)
        quint64 nonce;
        memcpy(&nonce, p + offset, 8);
        offset += 8;

        // User agent (var_str): length prefix + string
        QString userAgent;
        qint32 startHeight = 0;
        if (offset < payload.size()) {
            quint8 uaLen = static_cast<quint8>(payload[offset]);
            offset += 1;
            if (offset + uaLen <= payload.size()) {
                userAgent = QString::fromUtf8(payload.mid(offset, uaLen));
                offset += uaLen;
            }
        }

        // Start height (4 bytes)
        if (offset + 4 <= payload.size()) {
            memcpy(&startHeight, p + offset, 4);
            offset += 4;
        }
        peerHeight_ = startHeight;

        // Check for Utreexo support
        constexpr quint64 NODE_UTREEXO = 1ULL << 24;
        bool supportsUtreexo = (services & NODE_UTREEXO) != 0;

        qDebug() << "[P2P]" << id_ << "version:" << version << "services:" << Qt::hex << services
                 << "utreexo:" << supportsUtreexo << "agent:" << userAgent << "height:" << startHeight;
        
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
        updateActivity();  // Update last activity timestamp to prevent timeout
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
