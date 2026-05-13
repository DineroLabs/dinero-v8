#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QTimer>
#include <QNetworkAccessManager>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "wallet/hd_wallet.h"

// Optional WebSocket support
#ifdef QT_WEBSOCKETS_LIB
#include <QWebSocket>
#endif

// Forward declarations
namespace dinero {
    class Blockchain;
    class Mempool;
}

/**
 * Enhanced Block Explorer Integration
 * 
 * Provides comprehensive block explorer functionality with real-time updates,
 * transaction tracking, address monitoring, and network statistics.
 */
class EnhancedBlockExplorer : public QObject {
    Q_OBJECT

public:
    explicit EnhancedBlockExplorer(QObject* parent = nullptr);
    ~EnhancedBlockExplorer();

    // Initialization and configuration
    bool initialize(std::shared_ptr<dinero::Blockchain> blockchain,
                   std::shared_ptr<dinero::Mempool> mempool,
                   const QString& dataDir = QString());
    void shutdown();

    // Block operations
    QJsonObject getBlockInfo(const QString& blockHash);
    QJsonObject getBlockInfo(int height);
    QJsonArray getRecentBlocks(int count = 10);
    QJsonArray getBlocks(int fromHeight, int limit = 20);
    QJsonObject getBlockchainInfo();

    // Transaction operations
    QJsonObject getTransactionInfo(const QString& txid);
    QJsonObject getTransactionHex(const QString& txid);
    QJsonObject getTransactionProof(const QString& txid, const QString& blockHash);
    QJsonArray searchTransactions(const QString& query, int limit = 50);
    QJsonArray getTransactionHistory(const QString& address, int limit = 50);

    // Address operations
    QJsonObject getAddressInfo(const QString& address);
    QJsonArray getAddressTransactions(const QString& address, int limit = 50);
    QJsonArray getAddressUTXOs(const QString& address, int minConfirmations = 0);
    QJsonObject getAddressBalance(const QString& address);

    // Mempool operations
    QJsonArray getMempoolTransactions(int limit = 100);
    QJsonObject getMempoolInfo();
    QJsonArray getMempoolTxIds();
    QJsonObject getMempoolStats();

    // Network statistics
    QJsonObject getNetworkStats();
    QJsonObject getMiningStats();
    QJsonObject getSupplyStats();
    QJsonObject getDifficultyStats();
    QJsonObject getBlocks24hStats();

    // Search and discovery
    QJsonObject search(const QString& query);
    QJsonObject getHealthStatus();
    QJsonObject getExplorerStatus();

    // Real-time monitoring
    void startRealTimeUpdates();
    void stopRealTimeUpdates();
    bool isRealTimeEnabled() const;

    // Address monitoring
    void addAddressToWatch(const QString& address);
    void removeAddressFromWatch(const QString& address);
    QJsonArray getWatchedAddresses();
    void clearWatchedAddresses();

signals:
    // Block events
    void newBlockFound(int height, const QString& blockHash, uint64_t timestamp);
    void blockUpdated(int height, const QString& blockHash);
    
    // Transaction events
    void newTransactionFound(const QString& txid, const QString& blockHash);
    void transactionConfirmed(const QString& txid, int confirmations);
    void transactionInMempool(const QString& txid, double feeRate);
    
    // Address events
    void addressActivity(const QString& address, const QString& txid, int64_t delta);
    void addressBalanceChanged(const QString& address, double newBalance);
    
    // Network events
    void networkStatsUpdated(const QJsonObject& stats);
    void mempoolUpdated(const QJsonObject& mempoolInfo);
    
    // Error events
    void explorerError(const QString& error);
    void connectionLost();
    void connectionRestored();

private slots:
    void onBlockchainUpdate();
    void onMempoolUpdate();
    void onNetworkUpdate();
    void onRealTimeTimer();
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketMessage(const QString& message);

private:
    // Core components
    std::shared_ptr<dinero::Blockchain> blockchain_;
    std::shared_ptr<dinero::Mempool> mempool_;
    
    // Real-time components
    QTimer* realTimeTimer_;
#ifdef QT_WEBSOCKETS_LIB
    QWebSocket* webSocket_;
#endif
    QNetworkAccessManager* networkAccessManager_;
    
    // State management
    bool initialized_;
    bool realTimeEnabled_;
    QString dataDir_;
    QString lastBlockHash_;
    int lastBlockHeight_;
    std::unordered_set<QString> watchedAddresses_;
    
    // Thread safety
    mutable std::mutex dataMutex_;
    mutable std::mutex watchedAddressesMutex_;
    
    // Helper methods
    QJsonObject formatBlockInfo(const QString& blockHash, int height);
    QJsonObject formatTransactionInfo(const QString& txid);
    QJsonObject formatAddressInfo(const QString& address);
    QJsonObject formatMempoolInfo();
    QJsonObject formatNetworkStats();
    QJsonObject formatMiningStats();
    
    // Address utilities
    QString addressToScriptHash(const QString& address);
    QString scriptHashToAddress(const QString& scriptHash);
    bool isValidAddress(const QString& address);
    
    // Search utilities
    bool isBlockHash(const QString& query);
    bool isTransactionId(const QString& query);
    bool isAddress(const QString& query);
    bool isBlockHeight(const QString& query);
    
    // Real-time utilities
    void setupWebSocket();
    void processBlockchainUpdate();
    void processMempoolUpdate();
    void processAddressActivity();
    void broadcastUpdate(const QString& type, const QJsonObject& data);
    
    // Error handling
    void handleError(const QString& operation, const QString& error);
    QJsonObject createErrorResponse(const QString& error, int code = -1);
    
    // Caching
    struct CacheEntry {
        QJsonObject data;
        qint64 timestamp;
        int ttl; // time to live in seconds
    };
    std::unordered_map<QString, CacheEntry> cache_;
    mutable std::mutex cacheMutex_;
    
    QJsonObject getCachedData(const QString& key);
    void setCachedData(const QString& key, const QJsonObject& data, int ttl = 300);
    void clearCache();
    void cleanupExpiredCache();
};
