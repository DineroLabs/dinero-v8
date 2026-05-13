#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QObject>
#include "daemon/mempool.h"
#include "wallet/transaction.h"
#include "wallet/hd_wallet.h"

/**
 * Real Mempool Integration Service
 * 
 * This service provides real mempool integration for transaction broadcasting:
 * - Real transaction creation and validation
 * - Real mempool submission and broadcasting
 * - Real fee calculation and validation
 * - Real transaction status tracking
 * - Real mempool statistics and monitoring
 */
class RealMempoolIntegration : public QObject {
    Q_OBJECT

public:
    explicit RealMempoolIntegration(QObject* parent = nullptr);
    ~RealMempoolIntegration();

    // Initialization
    bool initialize(std::shared_ptr<dinero::Mempool> mempool, 
                   std::shared_ptr<dinero::Blockchain> blockchain,
                   const QString& dataDir = QString());
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // Transaction Creation and Broadcasting
    QString createAndBroadcastTransaction(const QString& accountId, 
                                         const QString& toAddress, 
                                         double amount,
                                         double feeRate = 0.00001);
    
    QString createTransaction(const QString& accountId, 
                             const QString& toAddress, 
                             double amount,
                             double feeRate = 0.00001);
    
    QString signTransaction(const QString& accountId, const QString& transactionHex);
    QString broadcastTransaction(const QString& transactionHex);
    
    // Transaction Status and Tracking
    QJsonObject getTransactionStatus(const QString& txid);
    QJsonObject getTransactionDetails(const QString& txid);
    QJsonArray getMempoolTransactions();
    QJsonArray getPendingTransactions(const QString& accountId);
    
    // Fee Estimation
    QJsonObject estimateFee(double amount, int targetBlocks = 6);
    QJsonObject getFeeEstimate();
    QJsonObject getMempoolStats();
    
    // UTXO Management
    QJsonArray getUTXOs(const QString& accountId);
    QJsonObject selectUTXOs(const QString& accountId, double amount);
    bool spendUTXO(const QString& txid, uint32_t vout);
    
    // Mempool Monitoring
    void startMempoolMonitoring();
    void stopMempoolMonitoring();
    
    // Account Integration
    void setAccountWallet(const QString& accountId, std::shared_ptr<HDWallet> wallet);
    std::shared_ptr<HDWallet> getAccountWallet(const QString& accountId);

signals:
    void transactionBroadcasted(const QString& txid);
    void transactionConfirmed(const QString& txid, int confirmations);
    void transactionRejected(const QString& txid, const QString& reason);
    void mempoolUpdated();
    void feeEstimateUpdated();

private slots:
    void onMempoolUpdate();
    void onTransactionStatusUpdate();

private:
    // Core components
    std::shared_ptr<dinero::Mempool> mempool_;
    std::shared_ptr<dinero::Blockchain> blockchain_;
    
    // Account management
    std::unordered_map<QString, std::shared_ptr<HDWallet>> accountWallets_;
    std::mutex accountWalletsMutex_;
    
    // Monitoring
    QTimer* mempoolMonitorTimer_;
    QTimer* statusUpdateTimer_;
    std::atomic<bool> monitoringEnabled_;
    std::atomic<bool> initialized_;
    
    // Transaction tracking
    std::unordered_map<QString, QString> pendingTransactions_; // txid -> accountId
    std::mutex pendingTransactionsMutex_;
    
    // Helper methods
    dinero::Transaction createDineroTransaction(const QString& accountId, 
                                              const QString& toAddress, 
                                              double amount,
                                              double feeRate);
    
    QString createTransactionHex(const dinero::Transaction& tx);
    dinero::Transaction parseTransactionHex(const QString& hex);
    
    QJsonObject calculateFeeEstimate(double amount, int targetBlocks);
    QJsonObject getMempoolStatistics();
    
    bool validateTransaction(const dinero::Transaction& tx, QString& error);
    bool checkDoubleSpend(const dinero::Transaction& tx);
    
    // Address and script utilities
    QString createP2WPKHScript(const QString& address);
    QString createP2PKHScript(const QString& address);
    bool isValidAddress(const QString& address);
    
    // UTXO utilities
    std::vector<QJsonObject> getAccountUTXOs(const QString& accountId);
    QJsonObject selectBestUTXOs(const std::vector<QJsonObject>& utxos, double targetAmount);
    
    // Error handling
    QJsonObject createErrorResponse(const QString& error);
    QJsonObject createSuccessResponse(const QJsonObject& data = QJsonObject());
};

/**
 * Transaction Builder
 * 
 * Helper class for building transactions with proper validation
 */
class TransactionBuilder {
public:
    TransactionBuilder();
    ~TransactionBuilder();
    
    // Transaction construction
    TransactionBuilder& setVersion(uint32_t version);
    TransactionBuilder& setLockTime(uint32_t lockTime);
    TransactionBuilder& addInput(const QString& txid, uint32_t vout, const QString& scriptSig = "");
    TransactionBuilder& addOutput(double amount, const QString& scriptPubKey);
    TransactionBuilder& addOutput(double amount, const QString& address, bool isP2WPKH = true);
    
    // Transaction building
    dinero::Transaction build();
    QString buildHex();
    
    // Validation
    bool isValid() const;
    QString getError() const;
    
    // Fee calculation
    double calculateFee(double feeRate) const;
    double calculateSize() const;
    
private:
    dinero::Transaction tx_;
    QString error_;
    bool valid_;
    
    // Helper methods
    QString createScriptPubKey(const QString& address, bool isP2WPKH);
        bool validateInputs();
        bool validateOutputs();
};

/**
 * Fee Calculator
 * 
 * Helper class for calculating transaction fees
 */
class FeeCalculator {
public:
    FeeCalculator();
    ~FeeCalculator();
    
    // Fee calculation methods
    double calculateFee(double amount, double feeRate) const;
    double calculateFeeRate(double amount, double fee) const;
    double calculateSize(const dinero::Transaction& tx) const;
    
    // Fee estimation
    QJsonObject estimateFee(const QString& accountId, double amount, int targetBlocks = 6);
    QJsonObject getRecommendedFeeRate() const;
    
    // Fee validation
    bool isValidFeeRate(double feeRate) const;
    bool isValidFee(double fee, double amount) const;
    
private:
    // Fee calculation constants
    static constexpr double MIN_FEE_RATE = 0.000001; // 0.000001 DIN per byte
    static constexpr double MAX_FEE_RATE = 0.01;     // 0.01 DIN per byte
    static constexpr double DEFAULT_FEE_RATE = 0.00001; // 0.00001 DIN per byte
    
    // Helper methods
    double estimateTransactionSize(const QString& accountId, double amount) const;
    double getCurrentMempoolFeeRate() const;
};

/**
 * UTXO Manager
 * 
 * Helper class for managing UTXOs
 */
class UTXOManager {
public:
    UTXOManager();
    ~UTXOManager();
    
    // UTXO management
    QJsonArray getUTXOs(const QString& accountId);
    QJsonObject selectUTXOs(const QString& accountId, double amount);
    bool spendUTXO(const QString& txid, uint32_t vout);
    bool addUTXO(const QString& txid, uint32_t vout, double amount, const QString& scriptPubKey);
    
    // UTXO validation
    bool isValidUTXO(const QJsonObject& utxo) const;
        bool isUTXOSpent(const QString& txid, uint32_t vout);
    
    // Balance calculation
        double getBalance(const QString& accountId);
    double getSpendableBalance(const QString& accountId) const;
    
private:
    // UTXO storage (in a real implementation, this would be persistent)
    std::unordered_map<QString, std::vector<QJsonObject>> accountUTXOs_;
    std::unordered_set<QString> spentUTXOs_;
        mutable std::mutex utxoMutex_;
    
    // Helper methods
    QString createUTXOKey(const QString& txid, uint32_t vout) const;
    QJsonObject createUTXO(const QString& txid, uint32_t vout, double amount, const QString& scriptPubKey) const;
};
