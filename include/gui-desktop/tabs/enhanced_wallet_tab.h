#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QListView>
#include <QComboBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTimer>
#include <QProgressBar>
#include <QTextEdit>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include "gui-desktop/utils/rpc_client.h"

class TxListModel;
class AnimatedBalanceWidget;
class WebSocketClient;
class WalletController;

/**
 * EnhancedWalletTab - Multi-Account Wallet Management
 * 
 * Features:
 * - Multi-account support with account switching
 * - Real-time balance updates per account
 * - Transaction management with account context
 * - Fee estimation with visual recommendations
 * - Transaction history with filtering
 * - UTXO management per account
 * - Address book integration
 * - Advanced transaction workflow (create/sign/broadcast)
 */
class EnhancedWalletTab : public QWidget {
    Q_OBJECT

public:
    explicit EnhancedWalletTab(QWidget *parent = nullptr);
    explicit EnhancedWalletTab(RpcClient* rpc, QWidget *parent = nullptr);
    
    void setRpcClient(RpcClient *client);
    void refreshAll();

public slots:
    void onWalletTransactionReceived(const QString &txid, double amount);
    void onConnectionChanged(bool connected);
    
    // WebSocket real-time events
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onRealTimeBalanceUpdate(double confirmed, double unconfirmed, double immature);
    void onRealTimeMiningReward(double amount, int blockHeight);
    void onRealTimeConfirmationUpdate(const QString& txid, int confirmations);
    
    // Wallet controller events
    void onAddressReady(const QString& address);
    void onWalletError(const QString& error);
    
    // Network switching
    void updateNetwork(Network network);

private slots:
    // Account management
    void onAccountSelectionChanged();
    void onCreateNewAccount();
    void onDeleteAccount();
    void onSwitchAccount();
    void onAccountInfoChanged();
    
    // Address management
    void onGenerateNewAddress();
    void onValidateAddress();
    void onCopyAddress();
    void onShowQRCode();
    
    // Transaction management
    void onSendTransaction();
    void onCreateTransaction();
    void onSignTransaction();
    void onBroadcastTransaction();
    void onEstimateFee();
    
    // Transaction history
    void onRefreshTransactionHistory();
    void onTransactionSelectionChanged();
    void onShowTransactionDetails();
    
    // UTXO management
    void onRefreshUTXOs();
    void onSelectUTXOs();
    
    // Security operations
    void onEncryptWallet();
    void onUnlockWallet();
    void onLockWallet();
    
    // Maintenance operations
    void onBackupWallet();
    void onRestoreWallet();
    void onRescanWallet();
    
    // Import operations
    void onImportKey();
    void onImportVault();
    
    // Fee estimation
    void onFeeSliderChanged();
    void onCustomFeeChanged();
    
    // Auto-refresh
    void onAutoRefresh();

private:
    void setupUI();
    void setupConnections();
    void setupAccountManagement();
    void setupTransactionManagement();
    void setupTransactionHistory();
    void setupUTXOManagement();
    void setupFeeEstimation();
    
    // UI creation methods
    QGroupBox* createAccountGroup();
    QGroupBox* createBalanceGroup();
    QGroupBox* createTransactionGroup();
    QGroupBox* createHistoryGroup();
    QGroupBox* createFeeGroup();
    QGroupBox* createSecurityGroup();
    
    void refreshAccountList();
    void refreshCurrentAccount();
    void refreshBalance();
    void refreshTransactionHistory();
    void refreshUTXOs();
    void refreshFeeEstimation();
    
    void showNotification(const QString& message);
    void showError(const QString& error);
    void showSuccess(const QString& message);
    
    void updateSendButtonState();
    void updateAccountInfo();
    void updateTransactionDetails();
    void updateFeeEstimation();
    
    // Account management
    QString getCurrentAccountId() const;
    void setCurrentAccountId(const QString& accountId);
    
    // UI Components - Account Management
    QComboBox *m_accountCombo;
    QPushButton *m_createAccountBtn;
    QPushButton *m_deleteAccountBtn;
    QPushButton *m_switchAccountBtn;
    QLabel *m_accountInfoLabel;
    
    // UI Components - Balance Section
    EnhancedBalanceWidget *m_balanceWidget;
    QLabel *m_lockState;
    QLabel *m_accountBalance;
    QLabel *m_accountAddressCount;
    
    // UI Components - Address Management
    QLineEdit *m_receiveAddress;
    QPushButton *m_generateAddressBtn;
    QPushButton *m_copyAddressBtn;
    QPushButton *m_qrCodeBtn;
    QPushButton *m_validateAddressBtn;
    QLabel *m_addressValidLabel;
    
    // UI Components - Transaction Management
    QLineEdit *m_sendAddress;
    QDoubleSpinBox *m_sendAmount;
    QPushButton *m_sendBtn;
    QPushButton *m_createTxBtn;
    QPushButton *m_signTxBtn;
    QPushButton *m_broadcastTxBtn;
    
    // UI Components - Fee Estimation
    QSlider *m_feeSlider;
    QLabel *m_feeLabel;
    QLineEdit *m_customFeeEdit;
    QPushButton *m_estimateFeeBtn;
    QLabel *m_feeEstimationLabel;
    QProgressBar *m_feeEstimationProgress;
    
    // UI Components - Transaction History
    QTableWidget *m_transactionTable;
    QPushButton *m_refreshTxBtn;
    QPushButton *m_txDetailsBtn;
    QLineEdit *m_txFilterEdit;
    
    // UI Components - UTXO Management
    QTableWidget *m_utxoTable;
    QPushButton *m_refreshUtxoBtn;
    QPushButton *m_selectUtxoBtn;
    
    // UI Components - Security
    QPushButton *m_encryptBtn;
    QPushButton *m_unlockBtn;
    QPushButton *m_lockBtn;
    
    // UI Components - Maintenance
    QPushButton *m_backupBtn;
    QPushButton *m_restoreBtn;
    QPushButton *m_rescanBtn;
    
    // UI Components - Import
    QPushButton *m_importKeyBtn;
    QPushButton *m_importVaultBtn;
    
    // Refresh timer
    QTimer *m_refreshTimer;
    QTimer *m_autoRefreshTimer;
    
    // RPC client
    RpcClient *m_rpcClient;
    
    // WebSocket client for real-time updates
    WebSocketClient *m_webSocketClient;
    bool m_realTimeEnabled = true;
    
    // HD Wallet controller
    WalletController *m_walletController;
    
    // Current state
    QString m_currentAccountId;
    QJsonObject m_currentAccountInfo;
    QJsonObject m_currentBalance;
    QJsonArray m_currentTransactions;
    QJsonArray m_currentUTXOs;
    QJsonObject m_currentFeeEstimation;
    
    // Transaction workflow state
    QString m_pendingTransaction;
    QString m_signedTransaction;
    bool m_transactionCreated = false;
    bool m_transactionSigned = false;
};

/**
 * EnhancedBalanceWidget - Multi-Account Balance Display
 */
class EnhancedBalanceWidget : public QWidget {
    Q_OBJECT
    
public:
    explicit EnhancedBalanceWidget(QWidget *parent = nullptr);
    
    void setAccountBalance(const QJsonObject& balance);
    void setAccountInfo(const QJsonObject& accountInfo);
    void setHighlight(bool highlight);
    
protected:
    void paintEvent(QPaintEvent *event) override;
    
private:
    QJsonObject m_balance;
    QJsonObject m_accountInfo;
    bool m_highlighted;
    
    QString formatDIN(double amount) const;
    QString formatAccountInfo() const;
};

/**
 * TransactionTableWidget - Enhanced Transaction Display
 */
class TransactionTableWidget : public QTableWidget {
    Q_OBJECT
    
public:
    explicit TransactionTableWidget(QWidget *parent = nullptr);
    
    void setTransactions(const QJsonArray& transactions);
    void setAccountId(const QString& accountId);
    
signals:
    void transactionSelected(const QString& txid);
    void transactionDoubleClicked(const QString& txid);
    
private slots:
    void onSelectionChanged();
    void onDoubleClicked(const QModelIndex& index);
    
private:
    QString m_accountId;
    QJsonArray m_transactions;
    
    void setupTable();
    void populateTable();
    QString formatTransactionType(const QString& type) const;
    QString formatAmount(double amount) const;
    QString formatDate(const QString& date) const;
};

/**
 * UTXOTableWidget - UTXO Management Display
 */
class UTXOTableWidget : public QTableWidget {
    Q_OBJECT
    
public:
    explicit UTXOTableWidget(QWidget *parent = nullptr);
    
    void setUTXOs(const QJsonArray& utxos);
    void setAccountId(const QString& accountId);
    
signals:
    void utxoSelected(const QString& txid, int vout);
    void utxoDoubleClicked(const QString& txid, int vout);
    
private slots:
    void onSelectionChanged();
    void onDoubleClicked(const QModelIndex& index);
    
private:
    QString m_accountId;
    QJsonArray m_utxos;
    
    void setupTable();
    void populateTable();
    QString formatAmount(double amount) const;
    QString formatConfirmations(int confirmations) const;
};
