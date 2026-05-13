#include "peer_manager.h"
/* daemon-only: QDebug disabled */
/* daemon-only: QHostAddress disabled */
/* daemon-only: QThread disabled */

PeerManager::PeerManager(const Options& opts, QObject* parent)
    : QObject(parent)
    , options_(opts)
    , server_(nullptr)
    , retryTimer_(nullptr)
    , boundPort_(0)
{
    // Qt objects will be created in startP2P() after moveToThread()
}

void PeerManager::startP2P() {
    Q_ASSERT(QThread::currentThread() == thread());
    
    if (!options_.enabled) {
        qDebug() << "[P2P] P2P networking disabled";
        return;
    }
    
    // Create Qt objects in the target thread
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &PeerManager::onNewConnection);
    
    retryTimer_ = new QTimer(this);
    retryTimer_->setInterval(30000);  // 30 seconds
    retryTimer_->setSingleShot(false);
    connect(retryTimer_, &QTimer::timeout, this, &PeerManager::onRetryTimer);
    
    // Start listening server
    if (!server_->listen(QHostAddress::AnyIPv4, options_.listenPort)) {
        if (options_.listenPort != 0) {
            qCritical() << "[P2P] Failed to bind to port" << options_.listenPort << ":" << server_->errorString();
            return;
        }
        
        // Try auto port
        if (!server_->listen(QHostAddress::AnyIPv4, 0)) {
            qCritical() << "[P2P] Failed to bind to any port:" << server_->errorString();
            return;
        }
    }
    
    boundPort_ = server_->serverPort();
    qInfo() << "[P2P] Listening on port" << boundPort_;
    
    // Emit signal that P2P is ready
    emit p2pStarted(boundPort_);
    
    // Start outbound connections
    const QStringList& targets = options_.connectOnly.isEmpty() ? options_.addNodes : options_.connectOnly;
    
    for (const QString& target : targets) {
        QStringList parts = target.split(':');
        QString host = parts[0];
        quint16 port = parts.size() > 1 ? parts[1].toUShort() : boundPort_;
        
        if (port == 0) {
            port = 20999;  // Default P2P port
        }
        
        dialPeer(host, port);
    }
    
    // Start retry timer if we have targets
    if (!targets.isEmpty()) {
        retryTimer_->start();
    }
}

bool PeerManager::start() {
    // This method is called from the main thread before moveToThread()
    // The actual P2P setup happens in startP2P() after thread migration
    return true;
}

void PeerManager::stopP2P() {
    Q_ASSERT(QThread::currentThread() == thread());
    
    qDebug() << "[P2P] Stopping peer manager";
    
    // Stop and delete Qt objects in the correct thread
    if (retryTimer_) {
        retryTimer_->stop();
        retryTimer_->deleteLater();
        retryTimer_ = nullptr;
    }
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    
    // Close all peers
    for (Peer* peer : peers_) {
        peer->close("Shutting down");
        peer->deleteLater();
    }
    
    peers_.clear();
    pendingConnections_.clear();
}

void PeerManager::stop() {
    // This method is called from the main thread
    // The actual cleanup happens in stopP2P() in the target thread
}

void PeerManager::dialPeer(const QString& host, quint16 port) {
    if (peers_.size() >= options_.maxPeers) {
        qDebug() << "[P2P] Max peers reached, not connecting to" << host << ":" << port;
        return;
    }
    
    // Check if we're already connected to this peer
    QString targetId = QString("%1:%2").arg(host).arg(port);
    for (Peer* peer : peers_) {
        if (peer->id() == targetId) {
            qDebug() << "[P2P] Already connected to" << targetId;
            return;
        }
    }
    
    qDebug() << "[P2P] Dialing" << targetId;
    
    Peer* peer = new Peer(QHostAddress(host), port, this);
    connect(peer, &Peer::handshakeDone, this, &PeerManager::onPeerHandshake);
    connect(peer, &Peer::closed, this, &PeerManager::onPeerClosed);
    connect(peer, &Peer::gotMessage, this, &PeerManager::onPeerMessage);
    
    peers_.append(peer);
    peer->start();
}

void PeerManager::onNewConnection() {
    while (QTcpSocket* socket = server_->nextPendingConnection()) {
        if (peers_.size() >= options_.maxPeers) {
            qDebug() << "[P2P] Max peers reached, rejecting inbound connection";
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        
        qDebug() << "[P2P] Accepting inbound connection from" 
                 << socket->peerAddress().toString() << ":" << socket->peerPort();
        
        Peer* peer = new Peer(socket, this);
        connect(peer, &Peer::handshakeDone, this, &PeerManager::onPeerHandshake);
        connect(peer, &Peer::closed, this, &PeerManager::onPeerClosed);
        connect(peer, &Peer::gotMessage, this, &PeerManager::onPeerMessage);
        
        peers_.append(peer);
        peer->start();
    }
}

void PeerManager::onPeerHandshake(Peer* peer) {
    qInfo() << "[P2P] Peer" << peer->id() << "handshake complete";
    emit peerReady(peer);
    
    // Start headers sync for this peer
    requestHeaders(peer);
}

void PeerManager::onPeerClosed(Peer* peer, QString reason) {
    qDebug() << "[P2P] Peer" << peer->id() << "closed:" << reason;
    
    peers_.removeAll(peer);
    
    // If this was an outbound connection that failed, add to retry list
    if (!peer->isInbound() && peer->state() != Peer::Ready) {
        QString target = peer->id();
        if (!pendingConnections_.contains(target)) {
            pendingConnections_.append(target);
            qDebug() << "[P2P] Added" << target << "to retry list";
        }
    }
    
    peer->deleteLater();
}

void PeerManager::onPeerMessage(Peer* peer, QByteArray cmd, QByteArray payload) {
    emit messageFromPeer(peer, cmd, payload);
}

void PeerManager::onRetryTimer() {
    // Retry failed connections
    QStringList toRetry = pendingConnections_;
    pendingConnections_.clear();
    
    for (const QString& target : toRetry) {
        QStringList parts = target.split(':');
        if (parts.size() == 2) {
            QString host = parts[0];
            quint16 port = parts[1].toUShort();
            dialPeer(host, port);
        }
    }
}

Peer* PeerManager::findBestPeer() const {
    for (Peer* peer : peers_) {
        if (peer->state() == Peer::Ready) {
            return peer;
        }
    }
    return nullptr;
}

void PeerManager::requestHeaders(Peer* peer) {
    if (peer->state() != Peer::Ready) {
        return;
    }
    
    // Build getheaders message
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    
    // TODO: Build proper block locator from blockchain
    // For now, send empty locator (request from genesis)
    ds << qint32(0);  // Locator count
    
    // Stop hash (all zeros = get as many as possible)
    QByteArray stopHash(32, '\0');
    ds.writeRawData(stopHash.constData(), 32);
    
    peer->send("getheaders", payload);
    qDebug() << "[P2P] Requested headers from" << peer->id();
}

// ============================================================================
// Phase W.2.6 Enhancement #4: Peer Quality Metrics
// ============================================================================

PeerManager::PeerQualityStats PeerManager::GetQualityStats() const {
    PeerQualityStats stats;

#ifdef QT_CORE_LIB
    double ping_sum = 0.0;
    int ping_count = 0;

    for (const Peer* peer : peers_) {
        if (!peer || peer->state() != Peer::Ready) {
            continue;
        }

        stats.total_peers++;

        // Collect ping metrics
        qint64 ping_ms = peer->lastPingMs();
        if (ping_ms > 0) {
            ping_sum += static_cast<double>(ping_ms);
            ping_count++;

            // Categorize peer quality (good < 300ms, bad >= 300ms)
            if (ping_ms < 300) {
                stats.good_peers++;
            } else {
                stats.bad_peers++;
            }
        }
    }

    // Calculate averages
    if (ping_count > 0) {
        stats.avg_ping_ms = ping_sum / ping_count;
    }

    // avg_download_kbps is reserved for future use (Phase N.x network metrics)
    stats.avg_download_kbps = 0.0;
#else
    // Non-Qt stub: returns empty stats (no peers tracked)
    // This will cause CheckLowPeerQuality() to return nullopt
#endif

    return stats;
}
