#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QListView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTimer>
#include "gui-desktop/utils/rpc_client.h"

class TxListModel;
class AnimatedBalanceWidget;
class WebSocketClient;
class WalletController;

/**
 * WalletTab - Wallet balance and transaction management
 * 
 * Features:
 * - Balance display (getbalance + wallet_tx events)
 * - Address generation (getnewaddress with labels)
 * - Transaction history (listtransactions with pagination)
 * - UTXO management (listunspent)
 * - Address validation (validateaddress)
 * - Copy/QR code functionality
 * - Real-time balance updates
 */
class WalletTab : public QWidget {
    Q_OBJECT

public:
    explicit WalletTab(QWidget *parent = nullptr);
    explicit WalletTab(RpcClient* rpc, QWidget *parent = nullptr);
    
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
    // Wallet operations
    void onClickNewAddress();
    void onClickValidateAddress();
    void onClickSend();
    
    // Security operations
    void onClickEncrypt();
    void onClickUnlock();
    void onClickLock();
    
    // Maintenance operations
    void onClickBackup();
    void onClickRestore();
    void onClickRescan();
    
    // Import operations
    void onClickImportKey();
    void onClickImportVault();
    
    void updateSendButtonState();

private:
    void setupUI();
    void setupConnections();
    void refreshWalletInfo();
    void refreshTxList();
    void refreshReceiveAddress(bool forceNew = false);
    void refreshMiningRewards();
    void showNotification(const QString& message);
    void setupWebSocketConnections();

    // UI Components
    // Balance section
    AnimatedBalanceWidget *m_balanceWidget;
    QLabel *m_lockState;
    
    // Mining rewards section
    QLabel *m_miningEarned;
    QLabel *m_blocksFound;
    QLabel *m_lastReward;
    
    // Security buttons
    QPushButton *m_encryptBtn;
    QPushButton *m_unlockBtn;
    QPushButton *m_lockBtn;
    
    // Receive section
    QLineEdit *m_receiveAddress;
    QPushButton *m_newAddressBtn;
    
    // Send section
    QLineEdit *m_sendAddress;
    QPushButton *m_validateBtn;
    QLabel *m_addressValid;
    QDoubleSpinBox *m_sendAmount;
    QSpinBox *m_targetBlocks;
    QPushButton *m_sendBtn;
    
    // Transaction history
    QListView *m_txListView;
    TxListModel *m_txModel;
    
    // Maintenance
    QPushButton *m_backupBtn;
    QPushButton *m_restoreBtn;
    QPushButton *m_rescanBtn;
    
    // Import
    QPushButton *m_importKeyBtn;
    QPushButton *m_importVaultBtn;
    
    // Refresh timer
    QTimer *m_refreshTimer;
    
    // RPC client
    RpcClient *m_rpcClient;
    
    // WebSocket client for real-time updates
    WebSocketClient *m_webSocketClient;
    bool m_realTimeEnabled = true;
    
    // HD Wallet controller
    WalletController *m_walletController;
};

/**
 * BalanceWidget - Custom widget for displaying DIN balances
 */
class BalanceWidget : public QWidget {
    Q_OBJECT
    
public:
    explicit BalanceWidget(QWidget *parent = nullptr);
    
    void setBalance(qint64 una, const QString &label);
    void setConfirmedBalance(qint64 una);
    void setUnconfirmedBalance(qint64 una);
    void setHighlight(bool highlight);
    
protected:
    void paintEvent(QPaintEvent *event) override;
    
private:
    qint64 m_balance;
    qint64 m_confirmed;
    qint64 m_unconfirmed;
    QString m_label;
    bool m_highlighted;
    
    QString formatDIN(qint64 una) const;
};
