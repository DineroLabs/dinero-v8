#include "blockchain/enhanced_block_explorer.h"
#include "daemon/mempool.h"
#include "daemon/main.h"
#include "wallet/hd_wallet.h"
#include "wallet/wallet_balance_service.h"
#include "common/sha256d.h"
#include "crypto/ripemd160.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QDateTime>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <random>

// Optional WebSocket support
#ifdef QT_WEBSOCKETS_LIB
#include <QWebSocket>
#endif

EnhancedBlockExplorer::EnhancedBlockExplorer(QObject* parent)
    : QObject(parent)
    , blockchain_(nullptr)
    , mempool_(nullptr)
    , realTimeTimer_(nullptr)
#ifdef QT_WEBSOCKETS_LIB
    , webSocket_(nullptr)
#endif
    , networkAccessManager_(nullptr)
    , initialized_(false)
    , realTimeEnabled_(false)
    , lastBlockHeight_(0)
{
    // Initialize real-time components
    realTimeTimer_ = new QTimer(this);
    realTimeTimer_->setInterval(5000); // 5 second updates
    
#ifdef QT_WEBSOCKETS_LIB
    webSocket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
#endif
    networkAccessManager_ = new QNetworkAccessManager(this);
    
    // Connect signals
    connect(realTimeTimer_, &QTimer::timeout, this, &EnhancedBlockExplorer::onRealTimeTimer);
#ifdef QT_WEBSOCKETS_LIB
    connect(webSocket_, &QWebSocket::connected, this, &EnhancedBlockExplorer::onWebSocketConnected);
    connect(webSocket_, &QWebSocket::disconnected, this, &EnhancedBlockExplorer::onWebSocketDisconnected);
    connect(webSocket_, &QWebSocket::textMessageReceived, this, &EnhancedBlockExplorer::onWebSocketMessage);
#endif
    
    qDebug() << "Enhanced Block Explorer initialized";
}

EnhancedBlockExplorer::~EnhancedBlockExplorer() {
    shutdown();
}

bool EnhancedBlockExplorer::initialize(std::shared_ptr<dinero::Blockchain> blockchain,
                                     std::shared_ptr<dinero::Mempool> mempool,
                                     const QString& dataDir) {
    if (initialized_) {
        return true;
    }
    
    try {
        blockchain_ = blockchain;
        mempool_ = mempool;
        dataDir_ = dataDir;
        
        // Initialize cache
        clearCache();
        
        // Get initial blockchain state
        if (blockchain_) {
            lastBlockHeight_ = static_cast<int>(blockchain_->getBlockHeight());
            lastBlockHash_ = QString::fromStdString(blockchain_->getBestBlockHash());
        }
        
        initialized_ = true;
        qDebug() << "Enhanced Block Explorer initialized successfully";
        return true;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to initialize Enhanced Block Explorer:" << e.what();
        return false;
    }
}

void EnhancedBlockExplorer::shutdown() {
    if (!initialized_) {
        return;
    }
    
    stopRealTimeUpdates();
    
    blockchain_.reset();
    mempool_.reset();
    
    clearCache();
    clearWatchedAddresses();
    
    initialized_ = false;
    qDebug() << "Enhanced Block Explorer shutdown complete";
}

QJsonObject EnhancedBlockExplorer::getBlockInfo(const QString& blockHash) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    // Check cache first
    QString cacheKey = QString("block_%1").arg(blockHash);
    QJsonObject cached = getCachedData(cacheKey);
    if (!cached.isEmpty()) {
        return cached;
    }
    
    try {
        QJsonObject blockInfo = formatBlockInfo(blockHash, -1);
        setCachedData(cacheKey, blockInfo, 3600); // Cache for 1 hour
        return blockInfo;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get block info: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getBlockInfo(int height) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    // Check cache first
    QString cacheKey = QString("block_height_%1").arg(height);
    QJsonObject cached = getCachedData(cacheKey);
    if (!cached.isEmpty()) {
        return cached;
    }
    
    try {
        // Get block data from height
        QString blockData = QString::fromStdString(blockchain_->getBlock(static_cast<uint32_t>(height)));
        if (blockData.isEmpty()) {
            return createErrorResponse("Block not found");
        }
        
        QJsonObject blockInfo = formatBlockInfo(blockData, height);
        setCachedData(cacheKey, blockInfo, 3600); // Cache for 1 hour
        return blockInfo;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get block info: %1").arg(e.what()));
    }
}

QJsonArray EnhancedBlockExplorer::getRecentBlocks(int count) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    try {
        QJsonArray blocks;
        int currentHeight = lastBlockHeight_;
        if (blockchain_) {
            currentHeight = static_cast<int>(blockchain_->getBlockHeight());
        }
        
        for (int i = 0; i < count && (currentHeight - i) >= 0; ++i) {
            int height = currentHeight - i;
            
            QJsonObject block;
            block["height"] = height;
            block["hash"] = QString("block_%1").arg(height); // Simplified hash
            block["timestamp"] = QDateTime::currentSecsSinceEpoch() - (i * 600); // Simulated timestamps
            block["tx_count"] = 1; // Simplified
            block["size"] = 1000; // Simplified
            block["confirmations"] = i;
            
            blocks.append(block);
        }
        
        return blocks;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get recent blocks:" << e.what();
        return QJsonArray();
    }
}

QJsonArray EnhancedBlockExplorer::getBlocks(int fromHeight, int limit) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    try {
        QJsonArray blocks;
        int currentHeight = static_cast<int>(blockchain_->getBlockHeight());
        
        for (int height = fromHeight; height <= currentHeight && blocks.size() < limit; ++height) {
            QString blockData = QString::fromStdString(blockchain_->getBlock(static_cast<uint32_t>(height)));
            
            QJsonObject block;
            block["height"] = height;
            block["hash"] = QString("block_%1").arg(height); // Simplified hash
            block["timestamp"] = QDateTime::currentSecsSinceEpoch() - ((currentHeight - height) * 600);
            block["tx_count"] = 1; // Simplified
            block["size"] = 1000; // Simplified
            block["confirmations"] = currentHeight - height;
            
            blocks.append(block);
        }
        
        return blocks;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get blocks:" << e.what();
        return QJsonArray();
    }
}

QJsonObject EnhancedBlockExplorer::getBlockchainInfo() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        QJsonObject info;
        info["chain"] = "dinero";
        
        if (blockchain_) {
            // Use actual blockchain data
            info["blocks"] = static_cast<int>(blockchain_->getBlockHeight());
            info["headers"] = static_cast<int>(blockchain_->getBlockHeight());
            info["bestblockhash"] = QString::fromStdString(blockchain_->getBestBlockHash());
        } else {
            // Use mock data when blockchain is not available
            info["blocks"] = static_cast<int>(lastBlockHeight_);
            info["headers"] = static_cast<int>(lastBlockHeight_);
            info["bestblockhash"] = lastBlockHash_;
        }
        
        info["difficulty"] = 1.0; // Simplified
        info["mediantime"] = QDateTime::currentSecsSinceEpoch();
        info["verificationprogress"] = 1.0;
        info["initialblockdownload"] = false;
        info["chainwork"] = QString("0000000000000000000000000000000000000000000000000000000000000000");
        info["size_on_disk"] = 0; // Simplified
        info["pruned"] = false;
        info["softforks"] = QJsonObject();
        info["bip9_softforks"] = QJsonObject();
        
        return info;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get blockchain info: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getTransactionInfo(const QString& txid) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    // Check cache first
    QString cacheKey = QString("tx_%1").arg(txid);
    QJsonObject cached = getCachedData(cacheKey);
    if (!cached.isEmpty()) {
        return cached;
    }
    
    try {
        QJsonObject txInfo = formatTransactionInfo(txid);
        setCachedData(cacheKey, txInfo, 1800); // Cache for 30 minutes
        return txInfo;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get transaction info: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getTransactionHex(const QString& txid) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        // Simplified implementation - in real implementation, get from blockchain
        QJsonObject result;
        result["txid"] = txid;
        result["hex"] = QString("0100000001000000000000000000000000000000000000000000000000000000000000000000ffffffff00ffffffff0100f2052a010000001976a914abcdef1234567890abcdef1234567890abcdef1288ac00000000");
        return result;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get transaction hex: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getTransactionProof(const QString& txid, const QString& blockHash) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        QJsonObject proof;
        proof["txid"] = txid;
        proof["blockhash"] = blockHash;
        proof["merkle_proof"] = QJsonArray(); // Simplified
        proof["confirmations"] = 1; // Simplified
        return proof;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get transaction proof: %1").arg(e.what()));
    }
}

QJsonArray EnhancedBlockExplorer::searchTransactions(const QString& query, int limit) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    try {
        QJsonArray results;
        
        // Simple search implementation
        if (isTransactionId(query)) {
            QJsonObject txInfo = getTransactionInfo(query);
            if (!txInfo.contains("error")) {
                results.append(txInfo);
            }
        } else if (isAddress(query)) {
            QJsonArray addressTxs = getAddressTransactions(query, limit);
            for (const QJsonValue& tx : addressTxs) {
                results.append(tx);
            }
        }
        
        return results;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to search transactions:" << e.what();
        return QJsonArray();
    }
}

QJsonArray EnhancedBlockExplorer::getTransactionHistory(const QString& address, int limit) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    try {
        QJsonArray history;
        
        // Simplified implementation - in real implementation, query blockchain
        for (int i = 0; i < limit; ++i) {
            QJsonObject tx;
            tx["txid"] = QString("tx_%1_%2").arg(address).arg(i);
            tx["height"] = static_cast<int>(blockchain_->getBlockHeight()) - i;
            tx["confirmations"] = i;
            tx["timestamp"] = QDateTime::currentSecsSinceEpoch() - (i * 600);
            tx["delta"] = (i % 2 == 0) ? 100000000 : -50000000; // Simulated delta
            history.append(tx);
        }
        
        return history;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get transaction history:" << e.what();
        return QJsonArray();
    }
}

QJsonObject EnhancedBlockExplorer::getAddressInfo(const QString& address) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    if (!isValidAddress(address)) {
        return createErrorResponse("Invalid address format");
    }
    
    // Check cache first
    QString cacheKey = QString("address_%1").arg(address);
    QJsonObject cached = getCachedData(cacheKey);
    if (!cached.isEmpty()) {
        return cached;
    }
    
    try {
        QJsonObject addressInfo = formatAddressInfo(address);
        setCachedData(cacheKey, addressInfo, 300); // Cache for 5 minutes
        return addressInfo;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get address info: %1").arg(e.what()));
    }
}

QJsonArray EnhancedBlockExplorer::getAddressTransactions(const QString& address, int limit) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    if (!isValidAddress(address)) {
        return QJsonArray();
    }
    
    try {
        return getTransactionHistory(address, limit);
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get address transactions:" << e.what();
        return QJsonArray();
    }
}

QJsonArray EnhancedBlockExplorer::getAddressUTXOs(const QString& address, int minConfirmations) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    if (!isValidAddress(address)) {
        return QJsonArray();
    }
    
    try {
        QJsonArray utxos;
        
        // Simplified implementation - in real implementation, query UTXO set
        for (int i = 0; i < 5; ++i) {
            QJsonObject utxo;
            utxo["txid"] = QString("utxo_%1_%2").arg(address).arg(i);
            utxo["vout"] = i;
            utxo["value"] = 100000000; // 1 DIN in una
            utxo["height"] = static_cast<int>(blockchain_->getBlockHeight()) - i;
            utxo["confirmations"] = i;
            utxo["scriptPubKey"] = QString("0014abcdef1234567890abcdef1234567890abcdef12");
            utxos.append(utxo);
        }
        
        return utxos;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get address UTXOs:" << e.what();
        return QJsonArray();
    }
}

QJsonObject EnhancedBlockExplorer::getAddressBalance(const QString& address) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    if (!isValidAddress(address)) {
        return createErrorResponse("Invalid address format");
    }
    
    try {
        QJsonObject balance;
        balance["address"] = address;
        
        // Get real balance from blockchain's UTXO set via WalletManager
        if (blockchain_) {
            std::string std_address = address.toStdString();
            
            // Query blockchain for UTXOs
            auto utxos = blockchain_->getUTXOsForAddress(std_address);
            
            uint64_t total_balance = 0;
            int tx_count = 0;
            
            // Calculate balance from UTXOs
            for (const auto& utxo : utxos) {
                total_balance += utxo.second; // second is the value
                tx_count++;
            }
            
            balance["balance"] = static_cast<qint64>(total_balance);
            balance["received"] = static_cast<qint64>(total_balance); // TODO: Track actual received
            balance["sent"] = 0; // TODO: Track actual sent
            balance["tx_count"] = tx_count;
            balance["unconfirmed_balance"] = 0; // TODO: Query mempool for pending
            balance["unconfirmed_tx_count"] = 0;
        } else {
            // Fallback if blockchain not available
            balance["balance"] = 0;
            balance["received"] = 0;
            balance["sent"] = 0;
            balance["tx_count"] = 0;
            balance["unconfirmed_balance"] = 0;
            balance["unconfirmed_tx_count"] = 0;
        }
        
        return balance;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get address balance: %1").arg(e.what()));
    }
}

QJsonArray EnhancedBlockExplorer::getMempoolTransactions(int limit) {
    if (!initialized_) {
        return QJsonArray();
    }
    
    try {
        QJsonArray mempoolTxs;
        
        // Simplified implementation - in real implementation, query mempool
        for (int i = 0; i < limit; ++i) {
            QJsonObject tx;
            tx["txid"] = QString("mempool_tx_%1").arg(i);
            tx["size"] = 250;
            tx["vsize"] = 250;
            tx["fee"] = 1000; // 1000 una
            tx["fee_rate"] = 4.0; // sat/vB
            tx["first_seen"] = QDateTime::currentSecsSinceEpoch() - (i * 60);
            mempoolTxs.append(tx);
        }
        
        return mempoolTxs;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get mempool transactions:" << e.what();
        return QJsonArray();
    }
}

QJsonObject EnhancedBlockExplorer::getMempoolInfo() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        return formatMempoolInfo();
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get mempool info: %1").arg(e.what()));
    }
}

QJsonArray EnhancedBlockExplorer::getMempoolTxIds() {
    if (!initialized_) {
        return QJsonArray();
    }
    
    try {
        QJsonArray txIds;
        
        // Simplified implementation
        for (int i = 0; i < 10; ++i) {
            txIds.append(QString("mempool_txid_%1").arg(i));
        }
        
        return txIds;
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to get mempool txids:" << e.what();
        return QJsonArray();
    }
}

QJsonObject EnhancedBlockExplorer::getMempoolStats() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        QJsonObject stats;
        stats["size"] = 10;
        stats["bytes"] = 2500;
        stats["usage"] = 2500;
        stats["maxmempool"] = 300000000;
        stats["mempoolminfee"] = 0.000001;
        stats["minrelaytxfee"] = 0.000001;
        return stats;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get mempool stats: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getNetworkStats() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        return formatNetworkStats();
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get network stats: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getMiningStats() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        return formatMiningStats();
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get mining stats: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getDifficultyStats() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        QJsonObject difficulty;
        difficulty["current_difficulty"] = 1.0;
        difficulty["difficulty_change"] = 0.0;
        difficulty["difficulty_change_24h"] = 0.0;
        difficulty["hash_rate"] = 1000000; // 1 MH/s
        difficulty["hash_rate_24h"] = 1000000;
        return difficulty;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get difficulty stats: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getBlocks24hStats() {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        QJsonObject stats;
        stats["blocks_24h"] = 144; // 144 blocks per day (10 minute blocks)
        stats["blocks_1h"] = 6; // 6 blocks per hour
        stats["blocks_1w"] = 1008; // 1008 blocks per week
        stats["blocks_1m"] = 4320; // 4320 blocks per month
        return stats;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Failed to get 24h blocks stats: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::search(const QString& query) {
    if (!initialized_) {
        return createErrorResponse("Block explorer not initialized");
    }
    
    try {
        QJsonObject result;
        
        if (isBlockHash(query)) {
            result["type"] = "block";
            result["data"] = getBlockInfo(query);
        } else if (isTransactionId(query)) {
            result["type"] = "transaction";
            result["data"] = getTransactionInfo(query);
        } else if (isAddress(query)) {
            result["type"] = "address";
            result["data"] = getAddressInfo(query);
        } else if (isBlockHeight(query)) {
            bool ok;
            int height = query.toInt(&ok);
            if (ok) {
                result["type"] = "block";
                result["data"] = getBlockInfo(height);
            }
        } else {
            result["type"] = "unknown";
            result["error"] = "Invalid search query";
        }
        
        return result;
        
    } catch (const std::exception& e) {
        return createErrorResponse(QString("Search failed: %1").arg(e.what()));
    }
}

QJsonObject EnhancedBlockExplorer::getHealthStatus() {
    QJsonObject health;
    health["status"] = initialized_ ? "healthy" : "unhealthy";
    health["initialized"] = initialized_;
    health["real_time_enabled"] = realTimeEnabled_;
    health["last_block_height"] = lastBlockHeight_;
    health["last_block_hash"] = lastBlockHash_;
    health["watched_addresses"] = static_cast<int>(watchedAddresses_.size());
    health["cache_size"] = static_cast<int>(cache_.size());
    health["timestamp"] = QDateTime::currentSecsSinceEpoch();
    return health;
}

QJsonObject EnhancedBlockExplorer::getExplorerStatus() {
    QJsonObject status;
    status["version"] = "1.0.0";
    status["name"] = "Dinero Block Explorer";
    status["description"] = "Enhanced block explorer for Dinero cryptocurrency";
    status["features"] = QJsonArray::fromStringList({
        "Real-time updates",
        "Address monitoring", 
        "Transaction tracking",
        "Mempool monitoring",
        "Network statistics",
        "Search functionality"
    });
    status["endpoints"] = QJsonArray::fromStringList({
        "/blocks",
        "/transactions", 
        "/addresses",
        "/mempool",
        "/network",
        "/search"
    });
    return status;
}

void EnhancedBlockExplorer::startRealTimeUpdates() {
    if (!initialized_ || realTimeEnabled_) {
        return;
    }
    
    realTimeEnabled_ = true;
    realTimeTimer_->start();
    
    // Setup WebSocket if available
    setupWebSocket();
    
    qDebug() << "Real-time updates started";
}

void EnhancedBlockExplorer::stopRealTimeUpdates() {
    if (!realTimeEnabled_) {
        return;
    }
    
    realTimeEnabled_ = false;
    realTimeTimer_->stop();
    
#ifdef QT_WEBSOCKETS_LIB
    if (webSocket_ && webSocket_->state() == QAbstractSocket::ConnectedState) {
        webSocket_->close();
    }
#endif
    
    qDebug() << "Real-time updates stopped";
}

bool EnhancedBlockExplorer::isRealTimeEnabled() const {
    return realTimeEnabled_;
}

void EnhancedBlockExplorer::addAddressToWatch(const QString& address) {
    if (!isValidAddress(address)) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(watchedAddressesMutex_);
    watchedAddresses_.insert(address);
    
    qDebug() << "Added address to watch:" << address;
}

void EnhancedBlockExplorer::removeAddressFromWatch(const QString& address) {
    std::lock_guard<std::mutex> lock(watchedAddressesMutex_);
    watchedAddresses_.erase(address);
    
    qDebug() << "Removed address from watch:" << address;
}

QJsonArray EnhancedBlockExplorer::getWatchedAddresses() {
    std::lock_guard<std::mutex> lock(watchedAddressesMutex_);
    
    QJsonArray addresses;
    for (const QString& address : watchedAddresses_) {
        addresses.append(address);
    }
    
    return addresses;
}

void EnhancedBlockExplorer::clearWatchedAddresses() {
    std::lock_guard<std::mutex> lock(watchedAddressesMutex_);
    watchedAddresses_.clear();
    
    qDebug() << "Cleared all watched addresses";
}

// Private slot implementations
void EnhancedBlockExplorer::onBlockchainUpdate() {
    if (!initialized_) {
        return;
    }
    
    try {
        processBlockchainUpdate();
    } catch (const std::exception& e) {
        handleError("Blockchain Update", e.what());
    }
}

void EnhancedBlockExplorer::onMempoolUpdate() {
    if (!initialized_) {
        return;
    }
    
    try {
        processMempoolUpdate();
    } catch (const std::exception& e) {
        handleError("Mempool Update", e.what());
    }
}

void EnhancedBlockExplorer::onNetworkUpdate() {
    if (!initialized_) {
        return;
    }
    
    try {
        QJsonObject networkStats = getNetworkStats();
        emit networkStatsUpdated(networkStats);
        
        QJsonObject mempoolInfo = getMempoolInfo();
        emit mempoolUpdated(mempoolInfo);
        
    } catch (const std::exception& e) {
        handleError("Network Update", e.what());
    }
}

void EnhancedBlockExplorer::onRealTimeTimer() {
    if (!initialized_) {
        return;
    }
    
    try {
        onBlockchainUpdate();
        onMempoolUpdate();
        onNetworkUpdate();
        processAddressActivity();
        cleanupExpiredCache();
        
    } catch (const std::exception& e) {
        handleError("Real-time Timer", e.what());
    }
}

#ifdef QT_WEBSOCKETS_LIB
void EnhancedBlockExplorer::onWebSocketConnected() {
    qDebug() << "WebSocket connected for real-time updates";
    emit connectionRestored();
}

void EnhancedBlockExplorer::onWebSocketDisconnected() {
    qDebug() << "WebSocket disconnected";
    emit connectionLost();
}

void EnhancedBlockExplorer::onWebSocketMessage(const QString& message) {
    try {
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        QJsonObject data = doc.object();
        
        QString type = data["type"].toString();
        
        if (type == "new_block") {
            int height = data["height"].toInt();
            QString hash = data["hash"].toString();
            uint64_t timestamp = data["timestamp"].toVariant().toULongLong();
            emit newBlockFound(height, hash, timestamp);
        } else if (type == "new_transaction") {
            QString txid = data["txid"].toString();
            QString blockHash = data["block_hash"].toString();
            emit newTransactionFound(txid, blockHash);
        }
        
    } catch (const std::exception& e) {
        qDebug() << "Failed to process WebSocket message:" << e.what();
    }
}
#endif

// Helper method implementations
QJsonObject EnhancedBlockExplorer::formatBlockInfo(const QString& blockHash, int height) {
    QJsonObject block;
    block["hash"] = blockHash;
    
    int currentHeight = lastBlockHeight_;
    if (blockchain_) {
        currentHeight = static_cast<int>(blockchain_->getBlockHeight());
    }
    
    block["height"] = height >= 0 ? height : currentHeight;
    block["timestamp"] = QDateTime::currentSecsSinceEpoch();
    block["size"] = 1000; // Simplified
    block["weight"] = 4000; // Simplified
    block["version"] = 1;
    block["versionHex"] = "00000001";
    block["merkleroot"] = QString("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    block["tx"] = QJsonArray::fromStringList({QString("tx_%1").arg(blockHash)});
    block["tx_count"] = 1;
    block["confirmations"] = currentHeight - block["height"].toInt();
    block["previousblockhash"] = QString("0000000000000000000000000000000000000000000000000000000000000000");
    block["nextblockhash"] = QString("0000000000000000000000000000000000000000000000000000000000000000");
    block["nonce"] = 0;
    block["bits"] = QString("1d00ffff");
    block["difficulty"] = 1.0;
    block["chainwork"] = QString("0000000000000000000000000000000000000000000000000000000000000000");
    return block;
}

QJsonObject EnhancedBlockExplorer::formatTransactionInfo(const QString& txid) {
    QJsonObject tx;
    tx["txid"] = txid;
    tx["version"] = 1;
    tx["locktime"] = 0;
    tx["size"] = 250;
    tx["weight"] = 1000;
    tx["vsize"] = 250;
    tx["fee"] = 1000; // 1000 una
    tx["confirmations"] = 1;
    tx["blockhash"] = lastBlockHash_;
    tx["blockheight"] = lastBlockHeight_;
    tx["blocktime"] = QDateTime::currentSecsSinceEpoch();
    tx["time"] = QDateTime::currentSecsSinceEpoch();
    
    // Simplified inputs/outputs
    QJsonArray inputs;
    QJsonObject input;
    input["txid"] = QString("0000000000000000000000000000000000000000000000000000000000000000");
    input["vout"] = 0;
    input["scriptSig"] = QJsonObject();
    input["sequence"] = static_cast<qint64>(4294967295);
    inputs.append(input);
    tx["vin"] = inputs;
    
    QJsonArray outputs;
    QJsonObject output;
    output["value"] = 100000000; // 1 DIN
    output["n"] = 0;
    output["scriptPubKey"] = QJsonObject();
    outputs.append(output);
    tx["vout"] = outputs;
    
    return tx;
}

QJsonObject EnhancedBlockExplorer::formatAddressInfo(const QString& address) {
    QJsonObject info;
    info["address"] = address;
    info["scripthash"] = addressToScriptHash(address);
    info["balance"] = 500000000; // 5 DIN in una
    info["received"] = 1000000000; // 10 DIN
    info["sent"] = 500000000; // 5 DIN
    info["tx_count"] = 10;
    info["unconfirmed_balance"] = 0;
    info["unconfirmed_tx_count"] = 0;
    return info;
}

QJsonObject EnhancedBlockExplorer::formatMempoolInfo() {
    QJsonObject info;
    
    if (mempool_) {
        // Use actual mempool data
        auto stats = mempool_->getStats();
        info["size"] = static_cast<int>(stats.tx_count);
        info["bytes"] = static_cast<int>(stats.total_size);
        info["usage"] = static_cast<int>(stats.total_size);
        info["maxmempool"] = 300000000; // 300MB default
        info["mempoolminfee"] = static_cast<double>(stats.min_fee_rate);
        info["minrelaytxfee"] = static_cast<double>(stats.min_fee_rate);
    } else {
        // Mock data when mempool is not available
        info["size"] = 10;
        info["bytes"] = 2500;
        info["usage"] = 2500;
        info["maxmempool"] = 300000000;
        info["mempoolminfee"] = 0.000001;
        info["minrelaytxfee"] = 0.000001;
    }
    
    return info;
}

QJsonObject EnhancedBlockExplorer::formatNetworkStats() {
    QJsonObject stats;

    stats["version"] = 1000000;
    stats["subversion"] = "/Dinero:1.0.0/";
    stats["protocolversion"] = 70015;
    stats["localservices"] = QString("0000000000000000");
    stats["timeoffset"] = 0;
    stats["connections"] = 0;
    stats["networkactive"] = true;
    stats["networks"] = QJsonArray();
    stats["relayfee"] = 0.000001;
    stats["incrementalfee"] = 0.000001;
    stats["localaddresses"] = QJsonArray();
    stats["warnings"] = dataDir_.isEmpty() ? "Explorer running without daemon RPC context." : "";

    return stats;
}

QJsonObject EnhancedBlockExplorer::formatMiningStats() {
    QJsonObject stats;
    stats["blocks"] = static_cast<int>(blockchain_->getBlockHeight());
    stats["currentblocksize"] = 1000;
    stats["currentblocktx"] = 1;
    stats["difficulty"] = 1.0;
    stats["networkhashps"] = 1000000;
    stats["pooledtx"] = 10;
    stats["chain"] = "dinero";
    stats["warnings"] = "";
    return stats;
}

QString EnhancedBlockExplorer::addressToScriptHash(const QString& address) {
    // Simplified implementation - in real implementation, decode bech32 and hash
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(address.toUtf8());
    return QString::fromUtf8(hash.result().toHex());
}

QString EnhancedBlockExplorer::scriptHashToAddress(const QString& scriptHash) {
    // Simplified implementation
    return QString("dnr1q%1").arg(scriptHash.left(32));
}

bool EnhancedBlockExplorer::isValidAddress(const QString& address) {
    return address.startsWith("dnr1") && address.length() >= 42;
}

bool EnhancedBlockExplorer::isBlockHash(const QString& query) {
    return query.length() == 64 && QRegularExpression("^[0-9a-fA-F]+$").match(query).hasMatch();
}

bool EnhancedBlockExplorer::isTransactionId(const QString& query) {
    return query.length() == 64 && QRegularExpression("^[0-9a-fA-F]+$").match(query).hasMatch();
}

bool EnhancedBlockExplorer::isAddress(const QString& query) {
    return isValidAddress(query);
}

bool EnhancedBlockExplorer::isBlockHeight(const QString& query) {
    bool ok;
    int height = query.toInt(&ok);
    return ok && height >= 0;
}

void EnhancedBlockExplorer::setupWebSocket() {
#ifdef QT_WEBSOCKETS_LIB
    // Simplified WebSocket setup - in real implementation, connect to actual WebSocket server
    if (webSocket_ && webSocket_->state() != QAbstractSocket::ConnectedState) {
        webSocket_->open(QUrl("ws://localhost:8080/ws"));
    }
#else
    qDebug() << "WebSocket support not available - using polling mode";
#endif
}

void EnhancedBlockExplorer::processBlockchainUpdate() {
    if (!blockchain_) {
        return;
    }
    
    int currentHeight = static_cast<int>(blockchain_->getBlockHeight());
    QString currentHash = QString::fromStdString(blockchain_->getBestBlockHash());
    
    if (currentHeight > lastBlockHeight_ || currentHash != lastBlockHash_) {
        lastBlockHeight_ = currentHeight;
        lastBlockHash_ = currentHash;
        
        emit newBlockFound(currentHeight, currentHash, QDateTime::currentSecsSinceEpoch());
        emit blockUpdated(currentHeight, currentHash);
        
        // Clear block-related cache
        std::lock_guard<std::mutex> lock(cacheMutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->first.startsWith("block_") || it->first.startsWith("tx_")) {
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void EnhancedBlockExplorer::processMempoolUpdate() {
    if (!mempool_) {
        return;
    }
    
    // Simplified mempool processing
    QJsonObject mempoolInfo = getMempoolInfo();
    emit mempoolUpdated(mempoolInfo);
}

void EnhancedBlockExplorer::processAddressActivity() {
    std::lock_guard<std::mutex> lock(watchedAddressesMutex_);
    
    for (const QString& address : watchedAddresses_) {
        // Simplified address activity processing
        // In real implementation, check for new transactions affecting watched addresses
        QJsonObject addressInfo = getAddressInfo(address);
        if (!addressInfo.contains("error")) {
            double balance = addressInfo["balance"].toDouble();
            emit addressBalanceChanged(address, balance);
        }
    }
}

void EnhancedBlockExplorer::broadcastUpdate(const QString& type, const QJsonObject& data) {
#ifdef QT_WEBSOCKETS_LIB
    if (webSocket_ && webSocket_->state() == QAbstractSocket::ConnectedState) {
        QJsonObject message;
        message["type"] = type;
        message["data"] = data;
        message["timestamp"] = QDateTime::currentSecsSinceEpoch();
        
        QJsonDocument doc(message);
        webSocket_->sendTextMessage(doc.toJson());
    }
#endif
}

void EnhancedBlockExplorer::handleError(const QString& operation, const QString& error) {
    qDebug() << QString("Enhanced Block Explorer Error in %1: %2").arg(operation).arg(error);
    emit explorerError(QString("%1: %2").arg(operation).arg(error));
}

QJsonObject EnhancedBlockExplorer::createErrorResponse(const QString& error, int code) {
    QJsonObject response;
    response["error"] = error;
    response["code"] = code;
    response["timestamp"] = QDateTime::currentSecsSinceEpoch();
    return response;
}

QJsonObject EnhancedBlockExplorer::getCachedData(const QString& key) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - it->second.timestamp < it->second.ttl) {
            return it->second.data;
        } else {
            cache_.erase(it);
        }
    }
    
    return QJsonObject();
}

void EnhancedBlockExplorer::setCachedData(const QString& key, const QJsonObject& data, int ttl) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    CacheEntry entry;
    entry.data = data;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();
    entry.ttl = ttl;
    
    cache_[key] = entry;
}

void EnhancedBlockExplorer::clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.clear();
}

void EnhancedBlockExplorer::cleanupExpiredCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (now - it->second.timestamp >= it->second.ttl) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}
