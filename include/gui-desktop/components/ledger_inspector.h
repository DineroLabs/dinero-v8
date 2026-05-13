#pragma once

#include <QWidget>
#include <QGroupBox>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTreeView>
#include <QPushButton>
#include <QTimer>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QProgressBar>
#include <QComboBox>
#include <QJsonObject>
#include <QJsonArray>

class RpcClient;

class LedgerInspector : public QWidget {
    Q_OBJECT

public:
    explicit LedgerInspector(QWidget* parent = nullptr);
    
    void setRpcClient(RpcClient* client);
    void refreshLedgerData();
    
    // Network context
    void setNetwork(const QString& network);
    QString getCurrentNetwork() const { return m_currentNetwork; }

public slots:
    void onNetworkChanged(const QString& network);
    void onNewBlockReceived();
    void onBalanceChanged();

private slots:
    void refreshNetworkStats();
    void refreshMyUTXOs();
    void onRefreshTimer();
    void onUTXODoubleClicked(const QModelIndex& index);
    void onFilterChanged();

private:
    // Core components
    RpcClient* m_rpcClient = nullptr;
    QTimer* m_refreshTimer;
    QString m_currentNetwork = "regtest";
    
    // UI Layout
    QVBoxLayout* m_mainLayout;
    QGroupBox* m_ledgerGroup;
    QGridLayout* m_ledgerLayout;
    
    // Public Network Stats (visible to everyone)
    QLabel* m_networkLabel;
    QLabel* m_heightValue;
    QLabel* m_utxoCountValue;
    QLabel* m_utxoTotalValue;
    QLabel* m_difficultyValue;
    QLabel* m_hashRateValue;
    QProgressBar* m_syncProgress;
    
    // Private UTXO Viewer (wallet-specific)
    QTreeView* m_utxoTreeView;
    QStandardItemModel* m_utxoModel;
    QSortFilterProxyModel* m_utxoProxyModel;
    QComboBox* m_utxoFilter;
    QPushButton* m_refreshButton;
    QPushButton* m_exportButton;
    
    // Statistics
    QLabel* m_myUtxoCount;
    QLabel* m_mySpendableAmount;
    QLabel* m_myImmatureAmount;
    QLabel* m_myUnconfirmedAmount;
    
    // Setup methods
    void setupUI();
    void setupNetworkStatsSection();
    void setupUTXOSection();
    void setupConnections();
    
    // Data processing
    void updateNetworkStats(const QJsonObject& blockchainInfo, const QJsonObject& txoutsetInfo);
    void updateMyUTXOs(const QJsonArray& utxos);
    void calculateUTXOStatistics();
    
    // UTXO model helpers
    void populateUTXOModel(const QJsonArray& utxos);
    QStandardItem* createUTXOItem(const QJsonObject& utxo);
    QString formatUTXOStatus(const QJsonObject& utxo);
    QString getUTXOIcon(const QJsonObject& utxo);
    QColor getUTXOColor(const QJsonObject& utxo);
    
    // Network helpers
    QString getNetworkDisplayName() const;
    QColor getNetworkColor() const;
    QString getNetworkIcon() const;
    
    // Constants
    static constexpr int REFRESH_INTERVAL_MS = 10000; // 10 seconds
    static constexpr int COINBASE_MATURITY = 100;
    static constexpr int CONFIRMATION_THRESHOLD = 6;
};
