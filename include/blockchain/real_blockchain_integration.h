#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QObject>
#include "daemon/mempool.h"
#include "wallet/utxo_index.h"
#include "common/blockchain_db.h"

/**
 * Real Blockchain Integration Service
 * 
 * This service replaces simulated data with real blockchain integration:
 * - Real transaction broadcasting via mempool
 * - Real UTXO management with blockchain sync
 * - Real fee estimation from mempool data
 * - Real transaction history from blockchain
 * - Real balance calculation from UTXO set
 * - Block explorer integration for transaction tracking
 */
class RealBlockchainIntegration : public QObject {
    Q_OBJECT

public:
    explicit RealBlockchainIntegration(QObject* parent = nullptr);
    ~RealBlockchainIntegration();

    // Initialization
    bool initialize(const std::string& datadir);
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // Account Management Integration
    QString createAccount(const QString& name, const QString& description);
    QJsonArray listAccounts();
    QJsonObject getAccountInfo(const QString& accountId);
    bool switchToAccount(const QString& accountId);
    bool deleteAccount(const QString& accountId);

    // Address Management Integration
    QString generateNewAddress(const QString& accountId);
    QJsonObject getAccountBalance(const QString& accountId);
    QJsonArray getAccountUTXOs(const QString& accountId);

    // Transaction Management Integration
    QString sendTransaction(const QString& accountId, const QString& toAddress, double amount);
    QJsonObject createTransaction(const QString& accountId, const QString& toAddress, double amount);
    QString signTransaction(const QString& accountId, const QString& transaction);
    QString broadcastTransaction(const QString& accountId, const QString& transaction);

    // Transaction History Integration
    QJsonArray getTransactionHistory(const QString& accountId, int limit = 50);
    QJsonObject getTransactionDetails(const QString& accountId, const QString& txid);
    QJsonObject getTransactionStatus(const QString& accountId, const QString& txid);

    // Fee Estimation Integration
    QJsonObject estimateFee(const QString& accountId, double amount);

    // Blockchain State Integration
    QJsonObject getBlockchainInfo();
    QJsonObject getMempoolInfo();
    QJsonObject getNetworkInfo();

    // Real-time Updates
    void startRealTimeUpdates();
    void stopRealTimeUpdates();

signals:
    void transactionReceived(const QString& accountId, const QString& txid, double amount);
    void balanceUpdated(const QString& accountId, double confirmed, double unconfirmed);
    void transactionConfirmed(const QString& accountId, const QString& txid, int confirmations);
    void mempoolUpdated();
    void blockchainUpdated(int height, const QString& blockHash);

private slots:
    void onMempoolUpdate();
    void onBlockchainUpdate();
    void onTransactionBroadcasted(const QString& txid);

private:
    // Core blockchain components
    std::shared_ptr<dinero::Blockchain> blockchain_;
    std::shared_ptr<dinero::Mempool> mempool_;
    std::unique_ptr<Dinero::Common::BlockchainDB> blockchain_db_;
    std::unique_ptr<dinero::UTXOIndex> utxo_index_;

    // Account management
    struct AccountData {
        QString accountId;
        QString name;
        QString description;
        QString masterSeed;
        uint32_t nextAddressIndex;
        std::chrono::system_clock::time_point createdAt;
        std::chrono::system_clock::time_point lastUsed;
    };
    
    std::unordered_map<QString, AccountData> accounts_;
    QString currentAccountId_;
    std::mutex accountsMutex_;

    // Real-time update management
    QTimer* updateTimer_;
    std::atomic<bool> realTimeUpdatesEnabled_;
    std::atomic<bool> initialized_;

    // Helper methods
    QString generateAccountId();
    AccountData* getAccount(const QString& accountId);
    bool saveAccount(const AccountData& account);
    bool loadAccounts();
    void updateAccountLastUsed(const QString& accountId);

    // Blockchain integration helpers
    QJsonObject getRealBalance(const QString& accountId);
    QJsonArray getRealUTXOs(const QString& accountId);
    QJsonArray getRealTransactionHistory(const QString& accountId, int limit);
    QJsonObject getRealTransactionDetails(const QString& txid);
    QJsonObject getRealFeeEstimation(double amount);

    // Transaction processing
    QString broadcastRealTransaction(const QString& transaction);
    bool validateRealTransaction(const QString& transaction);
    QJsonObject createRealTransaction(const QString& accountId, const QString& toAddress, double amount);

    // UTXO management
    bool spendUTXO(const QString& txid, uint32_t vout, const QString& spendingTxid);
    bool addUTXO(const QString& txid, uint32_t vout, uint64_t amount, const QString& scriptPubKey, uint32_t height);
    std::vector<dinero::UTXO> selectUTXOs(const QString& accountId, uint64_t targetAmount);

    // Address derivation
    QString deriveAddress(const QString& accountId, uint32_t index);
    QString getAccountMasterKey(const QString& accountId);

    // Error handling
    QJsonObject createErrorResponse(const QString& error);
    QJsonObject createSuccessResponse(const QJsonObject& data = QJsonObject());
};

/**
 * Blockchain State Monitor
 * 
 * Monitors blockchain state changes and provides real-time updates
 */
class BlockchainStateMonitor : public QObject {
    Q_OBJECT

public:
    explicit BlockchainStateMonitor(std::shared_ptr<dinero::Blockchain> blockchain, QObject* parent = nullptr);
    ~BlockchainStateMonitor();

    void startMonitoring();
    void stopMonitoring();

signals:
    void blockAdded(int height, const QString& blockHash);
    void transactionAdded(const QString& txid);
    void transactionRemoved(const QString& txid);
    void mempoolChanged();

private slots:
    void checkBlockchainState();
    void checkMempoolState();

private:
    std::shared_ptr<dinero::Blockchain> blockchain_;
    QTimer* monitorTimer_;
    
    // State tracking
    uint32_t lastBlockHeight_;
    QString lastBlockHash_;
    std::unordered_set<QString> lastMempoolTxids_;
    
    std::atomic<bool> monitoring_;
};

/**
 * Real Fee Estimator
 * 
 * Provides real fee estimation based on mempool data
 */
class RealFeeEstimator : public QObject {
    Q_OBJECT

public:
    explicit RealFeeEstimator(std::shared_ptr<dinero::Mempool> mempool, QObject* parent = nullptr);
    ~RealFeeEstimator();

    struct FeeEstimate {
        double feeRate;           // sat/byte
        double fee;              // total fee in DIN
        int estimatedBlocks;     // estimated confirmation time
        double confidence;       // confidence level (0-1)
    };

    FeeEstimate estimateFee(double amount, int targetBlocks = 6);
    QJsonObject getFeeEstimateJson(double amount, int targetBlocks = 6);

signals:
    void feeEstimateUpdated();

private:
    std::shared_ptr<dinero::Mempool> mempool_;
    
    // Fee estimation parameters
    static constexpr int FEE_ESTIMATION_BLOCKS[] = {1, 3, 6, 12, 24};
    static constexpr int FEE_ESTIMATION_SAMPLES = 100;
    
    // Helper methods
    double calculateFeeRate(int targetBlocks);
    double calculateConfidence(const std::vector<double>& feeRates);
};

/**
 * Block Explorer Integration
 * 
 * Provides block explorer functionality for transaction tracking
 */
class BlockExplorerIntegration : public QObject {
    Q_OBJECT

public:
    explicit BlockExplorerIntegration(std::shared_ptr<dinero::Blockchain> blockchain, QObject* parent = nullptr);
    ~BlockExplorerIntegration();

    // Transaction tracking
    QJsonObject getTransactionInfo(const QString& txid);
    QJsonObject getBlockInfo(const QString& blockHash);
    QJsonObject getBlockInfo(int height);
    QJsonArray getRecentBlocks(int count = 10);
    QJsonArray searchTransactions(const QString& query);

    // Address tracking
    QJsonObject getAddressInfo(const QString& address);
    QJsonArray getAddressTransactions(const QString& address, int limit = 50);

    // Network statistics
    QJsonObject getNetworkStats();
    QJsonObject getMiningStats();

signals:
    void transactionFound(const QString& txid);
    void blockFound(const QString& blockHash);

private:
    std::shared_ptr<dinero::Blockchain> blockchain_;
    
    // Helper methods
    QJsonObject formatTransactionInfo(const QString& txid);
    QJsonObject formatBlockInfo(const QString& blockHash);
    QJsonObject formatAddressInfo(const QString& address);
};
