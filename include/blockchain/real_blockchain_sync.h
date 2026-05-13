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

class RealBlockchainSync : public QObject {
    Q_OBJECT

public:
    explicit RealBlockchainSync(QObject* parent = nullptr);
    ~RealBlockchainSync();

    bool initialize(std::shared_ptr<dinero::Blockchain> blockchain,
                   std::shared_ptr<dinero::Mempool> mempool,
                   const QString& dataDir);
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // Sync Operations
    bool startSync(const QStringList& peerAddresses);
    void stopSync();
    bool isSyncing() const;
    QString getSyncStatus() const;
    QJsonObject getSyncProgress() const;
    QJsonObject getSyncMetrics() const;

    // Blockchain Info
    QJsonObject getBlockchainInfo() const;
    QJsonObject getBlockInfo(const QString& blockHash) const;
    QJsonObject getBlockInfo(int height) const;
    QJsonArray getRecentBlocks(int count = 10) const;

    // Peer Management
    bool connectToPeer(const QString& address, uint16_t port);
    void disconnectPeer(const QString& address);
    QJsonArray getConnectedPeers() const;

    // Network Info
    QJsonObject getNetworkInfo() const;

    // Validation
    bool validateBlock(const QJsonObject& block) const;
    bool validateTransaction(const QJsonObject& transaction) const;
    QJsonObject getValidationRules() const;

    // Consensus
    QJsonObject getConsensusState() const;

    // Real-time Monitoring
    void startRealTimeMonitoring();
    void stopRealTimeMonitoring();
    bool isRealTimeMonitoringEnabled() const;

signals:
    void syncProgressUpdated(const QJsonObject& progress);
    void syncCompleted();
    void syncError(const QString& error);
    void newBlockFound(int height, const QString& blockHash);
    void peerConnected(const QString& address, uint16_t port);
    void peerDisconnected(const QString& address);
    void consensusStateChanged(const QJsonObject& state);

private slots:
    void onSyncTimer();

private:
    // Core components
    std::shared_ptr<dinero::Blockchain> blockchain_;
    std::shared_ptr<dinero::Mempool> mempool_;
    
    // Sync components
    QTimer* syncTimer_;
    QNetworkAccessManager* networkAccessManager_;
    
    // State management
    bool initialized_;
    bool syncing_;
    bool realTimeMonitoringEnabled_;
    QString syncStatus_;
    QString dataDir_;
    
    // Sync metrics
    uint32_t headersReceived_;
    uint32_t blocksDownloaded_;
    uint32_t bestHeight_;
    QString bestBlockHash_;
    uint32_t totalHeaders_;
    uint32_t totalBlocks_;
    uint32_t connectedPeers_;
    uint64_t syncStartTime_;
    uint64_t lastActivity_;
    
    // Thread safety
    mutable std::mutex mutex_;

    // Helper methods
    void updateSyncProgress();
    void emitSyncProgress();
    void emitSyncCompleted();
    void emitSyncError(const QString& error);
    void emitNewBlockFound(int height, const QString& blockHash);
    void emitPeerConnected(const QString& address, uint16_t port);
    void emitPeerDisconnected(const QString& address);
    void emitConsensusStateChanged(const QJsonObject& state);
};
