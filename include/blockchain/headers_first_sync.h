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

class HeadersFirstSync : public QObject {
    Q_OBJECT

public:
    explicit HeadersFirstSync(QObject* parent = nullptr);
    ~HeadersFirstSync();

    bool initialize(std::shared_ptr<dinero::Blockchain> blockchain,
                   std::shared_ptr<dinero::Mempool> mempool,
                   const QString& dataDir);
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // Headers-First Sync Operations
    bool startHeadersFirstSync(const QStringList& peerAddresses);
    void stopSync();
    bool isSyncing() const;
    QString getSyncStatus() const;
    QJsonObject getSyncProgress() const;
    QJsonObject getSyncMetrics() const;

    // Headers Download
    bool downloadHeaders(const QStringList& peerAddresses);
    uint32_t getHeadersCount() const;
    uint32_t getBestHeight() const;

    // Headers Validation
    bool validateHeader(const QJsonObject& header) const;

    // Headers Storage
    bool storeHeaders(const QJsonArray& headers);

    // Headers Retrieval
    QJsonArray getHeaders(int startHeight, int count) const;
    QJsonArray getHeadersByHash(const QString& startHash, const QString& endHash) const;
    QJsonObject getHeaderByHeight(int height) const;
    QJsonObject getHeaderByHash(const QString& hash) const;

    // Headers Chain
    QJsonObject getChainTip() const;
    QString getChainWork() const;
    uint32_t getChainLength() const;

    // Headers Work
    QString calculateWork(const QJsonObject& header) const;
    QString getTotalWork() const;

    // Headers Progress
    QJsonObject getHeadersMetrics() const;
    QJsonObject getPerformanceMetrics() const;

    // Headers Errors
    bool recoverFromCorruption();
    bool recoverFromNetworkInterruption();

    // Headers Performance
    bool optimizePerformance();

    // Real-time Monitoring
    void startRealTimeMonitoring();
    void stopRealTimeMonitoring();
    bool isRealTimeMonitoringEnabled() const;

    // Peer Management
    bool connectToPeer(const QString& address, uint16_t port);

signals:
    void headersProgressUpdated(const QJsonObject& progress);
    void headersSyncCompleted();
    void headersSyncError(const QString& error);
    void newHeaderReceived(int height, const QString& headerHash);
    void peerConnected(const QString& address, uint16_t port);
    void peerDisconnected(const QString& address);
    void chainTipUpdated(const QJsonObject& chainTip);

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
    
    // Headers metrics
    uint32_t headersReceived_;
    uint32_t bestHeight_;
    QString bestBlockHash_;
    uint32_t totalHeaders_;
    uint32_t connectedPeers_;
    uint64_t syncStartTime_;
    uint64_t lastActivity_;
    
    // Thread safety
    mutable std::mutex mutex_;

    // Helper methods
    void updateHeadersProgress();
    void emitHeadersProgress();
    void emitHeadersSyncCompleted();
    void emitHeadersSyncError(const QString& error);
    void emitNewHeaderReceived(int height, const QString& headerHash);
    void emitPeerConnected(const QString& address, uint16_t port);
    void emitPeerDisconnected(const QString& address);
    void emitChainTipUpdated(const QJsonObject& chainTip);
};
