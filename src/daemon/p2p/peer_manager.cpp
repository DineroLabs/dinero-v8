#include "peer_manager.h"
#include "p2p/headers_first_sync.h"
#include "p2p/sha256d.h"  // P1: Double-SHA256 for block hashing
#include "storage/chain_direct.h"  // P1: Block locator generation
#include "daemon/daemon_context.h"  // Week 3: Context injection
#include "daemon/services/chainstate_service.h"  // Week 3: Chainstate access
#include <QDebug>
#include <QHostAddress>
#include <QThread>

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
    
    // If this was an outbound connection that failed or timed out, add to retry list
    if (!peer->isInbound() &&
        (peer->state() != Peer::Ready ||
         reason.contains("timeout", Qt::CaseInsensitive))) {
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
    
    // Route headers-first sync messages to HeadersFirstSync
    if (!dinero::p2p::g_headers_sync) {
        return;
    }
    
    if (cmd == "headers") {
        qDebug() << "[P2P] Received headers message from" << peer->id() 
                 << "(" << payload.size() << "bytes)";
        
        // Deserialize headers response (copied from HeadersSync::handleHeaders)
        QDataStream ds(payload);
        ds.setByteOrder(QDataStream::LittleEndian);
        
        qint32 count;
        ds >> count;
        
        // Validate header count
        if (count < 0 || count > 2000) {
            qWarning() << "[P2P] Invalid header count from" << peer->id() << ":" << count;
            return;
        }
        
        if (count == 0) {
            qDebug() << "[P2P] Peer" << peer->id() << "is up to date (no new headers)";
            // Send empty response to HeadersFirstSync
            dinero::p2p::HeadersResponse response;
            dinero::p2p::g_headers_sync->processHeaders(peer->id().toStdString(), response);
            return;
        }
        
        qDebug() << "[P2P] Parsing" << count << "headers from" << peer->id();
        
        // Parse each 128-byte header (BlockHeader v1)
        dinero::p2p::HeadersResponse response;
        response.headers.clear();

        for (int i = 0; i < count; i++) {
            // Check for truncation
            if (ds.atEnd() || payload.size() < (int)ds.device()->pos() + 128) {
                qWarning() << "[P2P] Truncated headers message from" << peer->id();
                break;
            }

            // Read 128-byte header (BlockHeader v1)
            QByteArray headerBytes(128, '\0');
            ds.readRawData(headerBytes.data(), 128);
            
            // Parse header fields (Dinero BlockHeader v1 - 128 bytes)
            QDataStream hds(headerBytes);
            hds.setByteOrder(QDataStream::LittleEndian);

            dinero::p2p::BlockHeader header;

            // Version (4 bytes) - offset 0
            quint32 version;
            hds >> version;
            header.version = version;

            // Previous block hash (32 bytes) - offset 4
            QByteArray prevHash(32, '\0');
            hds.readRawData(prevHash.data(), 32);
            header.prev_block_hash = prevHash.toHex().toStdString();

            // Merkle root (32 bytes) - offset 36
            QByteArray merkleRoot(32, '\0');
            hds.readRawData(merkleRoot.data(), 32);
            header.merkle_root = merkleRoot.toHex().toStdString();

            // Utreexo root (32 bytes) - offset 68
            QByteArray utreexoRoot(32, '\0');
            hds.readRawData(utreexoRoot.data(), 32);
            // header.utreexo_root = utreexoRoot.toHex().toStdString();  // If needed

            // Timestamp (8 bytes) - offset 100
            quint64 timestamp;
            hds >> timestamp;
            header.timestamp = timestamp;

            // Difficulty (4 bytes) - offset 108
            quint32 difficulty;
            hds >> difficulty;
            header.difficulty = difficulty;

            // Nonce (4 bytes) - offset 112
            quint32 nonce;
            hds >> nonce;
            header.nonce = nonce;

            // Reserved (12 bytes) - offset 116, skip
            QByteArray reserved(12, '\0');
            hds.readRawData(reserved.data(), 12);

            // Calculate block hash using proper double SHA256
            // Hash the full 128-byte header (BlockHeader v1)
            auto hash_array = din::crypto::sha256d(
                reinterpret_cast<const uint8_t*>(headerBytes.constData()),
                128
            );

            // Convert to hex string (little-endian for display)
            QString hashHex;
            for (int i = hash_array.size() - 1; i >= 0; --i) {  // Reverse for big-endian display
                hashHex += QString("%1").arg(hash_array[i], 2, 16, QChar('0'));
            }
            header.hash = hashHex.toStdString();

            // Height will be set by HeadersFirstSync during validation
            header.height = 0;
            
            response.headers.append(header);
        }
        
        // Set more_available flag if we got maximum headers
        response.more_available = (count == 2000);
        
        qDebug() << "[P2P] Successfully parsed" << response.headers.size() << "headers";
        
        // Pass to headers sync for processing
        dinero::p2p::g_headers_sync->processHeaders(peer->id().toStdString(), response);
    }
    else if (cmd == "block") {
        qDebug() << "[P2P] Received block message from" << peer->id() 
                 << "(" << payload.size() << "bytes)";
        
        // Validate block size
        if (payload.size() < 128) {
            qWarning() << "[P2P] Block too small from" << peer->id();
            return;
        }

        // P1: Calculate block hash from header using proper double SHA256 (128-byte BlockHeader v1)
        QByteArray headerBytes = payload.left(128);
        auto hash_array = din::crypto::sha256d(
            reinterpret_cast<const uint8_t*>(headerBytes.constData()),
            128
        );

        // Convert to hex string (reverse for big-endian display)
        QString hashHex;
        for (int i = hash_array.size() - 1; i >= 0; --i) {
            hashHex += QString("%1").arg(hash_array[i], 2, 16, QChar('0'));
        }
        std::string block_hash = hashHex.toStdString();
        
        // Convert payload to string for processBlock
        std::string block_data(payload.constData(), payload.size());
        
        qDebug() << "[P2P] Processing block, size:" << payload.size() << "bytes";
        
        // Pass block data to headers sync
        dinero::p2p::g_headers_sync->processBlock(block_hash, block_data);
    }
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

    // P1: Build proper block locator (BIP 152 algorithm)
    // Start with recent blocks, then exponential backoff
    std::vector<QByteArray> locator_hashes;

    // Week 3: Use context instead of global
    if (m_context && m_context->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(m_context->chainstate);
        if (chainstate) {
            auto chain_db = chainstate->chainDB();
            if (chain_db) {
                auto tip_result = chain_db->getTip();
                if (tip_result.ok()) {
                    int height = static_cast<int>(tip_result.value().height);
                    int step = 1;

                    // Start with recent blocks (first 10)
                    while (height > 0 && locator_hashes.size() < 10) {
                        auto hash_result = chain_db->getBlockHashByHeight(height);
                        if (hash_result.ok()) {
                            QByteArray hash_bytes(reinterpret_cast<const char*>(hash_result.value().data()), 32);
                            locator_hashes.push_back(hash_bytes);
                        }

                        height = (height > step) ? (height - step) : 0;
                    }

                    // Exponential backoff after first 10 blocks
                    if (height > 0) {
                        while (height > 0 && locator_hashes.size() < 32) {  // Max 32 locators
                            auto hash_result = chain_db->getBlockHashByHeight(height);
                            if (hash_result.ok()) {
                                QByteArray hash_bytes(reinterpret_cast<const char*>(hash_result.value().data()), 32);
                                locator_hashes.push_back(hash_bytes);
                            }

                            step *= 2;  // Exponential backoff
                            height = (height > step) ? (height - step) : 0;
                        }
                    }

                    // Always include genesis block
                    if (height > 0 || locator_hashes.empty()) {
                        auto genesis_hash = chain_db->getBlockHashByHeight(0);
                        if (genesis_hash.ok()) {
                            QByteArray genesis_bytes(reinterpret_cast<const char*>(genesis_hash.value().data()), 32);
                            locator_hashes.push_back(genesis_bytes);
                        }
                    }
                }
            }
        }
    }

    // Write locator count
    ds << qint32(locator_hashes.size());

    // Write locator hashes
    for (const auto& hash : locator_hashes) {
        ds.writeRawData(hash.constData(), 32);
    }

    // Stop hash (all zeros = get as many as possible)
    QByteArray stopHash(32, '\0');
    ds.writeRawData(stopHash.constData(), 32);

    peer->send("getheaders", payload);
    qDebug() << "[P2P] Requested headers from" << peer->id()
             << "with" << locator_hashes.size() << "locator hashes";
}
