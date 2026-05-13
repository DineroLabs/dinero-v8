#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QNetworkAccessManager>
#include <memory>
#include <mutex>
#include <cstdint>

// Forward declarations
namespace dinero {
    class Blockchain;
    class Mempool;
}

class RealBlockchainSync;

class RealNetworkIntegration : public QObject {
    Q_OBJECT

public:
    explicit RealNetworkIntegration(QObject* parent = nullptr);
    ~RealNetworkIntegration();

    bool initialize(std::shared_ptr<dinero::Blockchain> blockchain,
                   std::shared_ptr<dinero::Mempool> mempool,
                   std::shared_ptr<RealBlockchainSync> blockchainSync,
                   const QString& dataDir);
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // Network Operations
    bool startNetwork();
    void stopNetwork();
    bool isNetworkActive() const;
    QString getNetworkStatus() const;

    // Peer Management
    bool connectToPeer(const QString& address, uint16_t port);
    void disconnectPeer(const QString& address);
    QJsonArray getConnectedPeers() const;
    QJsonObject getPeerInfo(const QString& address) const;

    // Blockchain Integration
    QJsonObject getBlockchainInfo() const;
    QJsonObject getBlockInfo(const QString& blockHash) const;
    QJsonArray getRecentBlocks(int count = 10) const;

    // Mempool Integration
    QJsonObject getMempoolInfo() const;
    QJsonArray getMempoolTransactions() const;
    QJsonObject getMempoolStats() const;

    // UTXO Management
    QJsonArray getUTXOsForAddress(const QString& address) const;
    QJsonObject getUTXOInfo(const QString& txid, int vout) const;
    uint32_t getUTXOCount() const;

    // Fee Estimation
    QJsonObject getFeeEstimates() const;
    double getFeeRate(int blocks) const;
    QJsonObject getSmartFee(int blocks) const;

    // Real-time Monitoring
    void startRealTimeMonitoring();
    void stopRealTimeMonitoring();
    bool isRealTimeMonitoringEnabled() const;

    // Network Stats
    QJsonObject getNetworkStats() const;
    QJsonObject getNetworkHealth() const;

    // Peer Connections
    QJsonArray getPeerConnections() const;
    QJsonObject getPeerConnectionInfo(const QString& address) const;

    // Message Handling
    bool handleNetworkMessage(const QJsonObject& message);

    // Broadcasting
    bool broadcastBlock(const QJsonObject& block);
    bool broadcastTransaction(const QJsonObject& transaction);

    // Network Health
    bool isNetworkHealthy() const;

signals:
    void networkStarted();
    void networkStopped();
    void peerConnected(const QString& address, uint16_t port);
    void peerDisconnected(const QString& address);
    void blockReceived(const QJsonObject& block);
    void transactionReceived(const QJsonObject& transaction);
    void mempoolUpdated(const QJsonObject& mempoolStats);
    void networkStatsUpdated(const QJsonObject& networkStats);
    void networkHealthChanged(const QJsonObject& networkHealth);
    void feeEstimatesUpdated(const QJsonObject& feeEstimates);

private slots:
    void onNetworkTimer();

private:
    // Core components
    std::shared_ptr<dinero::Blockchain> blockchain_;
    std::shared_ptr<dinero::Mempool> mempool_;
    std::shared_ptr<RealBlockchainSync> blockchainSync_;
    
    // Network components
    QTimer* networkTimer_;
    QNetworkAccessManager* networkAccessManager_;
    
    // State management
    bool initialized_;
    bool networkActive_;
    bool realTimeMonitoringEnabled_;
    QString networkStatus_;
    QString dataDir_;
    
    // Network metrics
    uint32_t connectedPeers_;
    uint32_t bestHeight_;
    QString bestBlockHash_;
    
    // Thread safety
    mutable std::mutex mutex_;

    // Helper methods
    void updateNetworkState();
    void emitNetworkStarted();
    void emitNetworkStopped();
    void emitPeerConnected(const QString& address, uint16_t port);
    void emitPeerDisconnected(const QString& address);
    void emitBlockReceived(const QJsonObject& block);
    void emitTransactionReceived(const QJsonObject& transaction);
    void emitMempoolUpdated(const QJsonObject& mempoolStats);
    void emitNetworkStatsUpdated(const QJsonObject& networkStats);
    void emitNetworkHealthChanged(const QJsonObject& networkHealth);
    void emitFeeEstimatesUpdated(const QJsonObject& feeEstimates);
};
