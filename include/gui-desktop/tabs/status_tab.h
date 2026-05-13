#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTimer>
#include <QPushButton>
#include <QTextEdit>
#include "gui-desktop/utils/rpc_client.h"
#include "gui-desktop/models/network_state.h"

/**
 * StatusTab - Blockchain and network status overview
 * 
 * Features:
 * - Real-time blockchain info (getblockchaininfo + new_block events)
 * - Network status (getnetworkinfo)
 * - Mempool status (getmempoolinfo)
 * - Connection health monitoring
 * - Live sync progress
 * - Professional status indicators
 */
class StatusTab : public QWidget {
    Q_OBJECT

public:
    explicit StatusTab(QWidget *parent = nullptr);
    
    void setRpcClient(RpcClient *client);
    void attachRpc(RpcClient* client);  // NEW: Direct RPC attachment
    void setNetworkName(const QString& network);  // NEW: Set network from launcher
    void updateStatus();
    
    // Access to unified state model
    NetworkState* networkState() const { return m_networkState; }

public slots:
    void onNewBlockReceived(const QString &hash, int height);
    void onConnectionChanged(bool connected);

private slots:
    void refreshAll();
    void onBlockchainInfoReceived(const RpcClient::BlockchainInfo &info);
    void onNetworkInfoReceived(const RpcClient::NetworkInfo &info);
    void onMempoolInfoReceived(const RpcClient::MempoolInfo &info);
    void onNetworkStateUpdated();  // NEW: Handle unified state changes

private:
    void setupUI();
    void updateBlockchainSection(const RpcClient::BlockchainInfo &info);
    void updateNetworkSection(const RpcClient::NetworkInfo &info);
    void updateMempoolSection(const RpcClient::MempoolInfo &info);
    void setStatusColor(QLabel *label, const QString &text, bool good);

    // UI Components
    QVBoxLayout *m_mainLayout;
    
    // Blockchain section
    QGroupBox *m_blockchainGroup;
    QLabel *m_heightLabel;
    QLabel *m_hashLabel;
    QLabel *m_difficultyLabel;
    QLabel *m_syncStatusLabel;
    QProgressBar *m_syncProgress;
    QLabel *m_networkTypeLabel;
    
    // Network section  
    QGroupBox *m_networkGroup;
    QLabel *m_connectionsLabel;
    QLabel *m_versionLabel;
    QLabel *m_networkActiveLabel;
    QLabel *m_lastBlockTimeLabel;
    
    // Mempool section
    QGroupBox *m_mempoolGroup;
    QLabel *m_mempoolSizeLabel;
    QLabel *m_mempoolBytesLabel;
    QLabel *m_minFeeLabel;
    QLabel *m_maxFeeLabel;
    
    // Control section
    QGroupBox *m_controlGroup;
    QPushButton *m_refreshButton;
    QLabel *m_lastUpdateLabel;
    QTextEdit *m_recentEvents;
    
    // Backend
    RpcClient *m_rpcClient;
    QTimer *m_updateTimer;
    NetworkState *m_networkState;  // NEW: Unified state model
    
    // Legacy state (to be removed)
    bool m_isConnected;
    int m_currentHeight;
    QString m_bestHash;
    QDateTime m_lastUpdate;
};

// StatusIndicator is defined in components/status_indicator.h
