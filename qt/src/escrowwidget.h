#pragma once

#include <QWidget>
#include <QTableView>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QStandardItemModel>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QProgressBar>

class RpcClient;
class WebSocketClient;

/**
 * EscrowWidget - Smart Contract Escrow Management Interface
 *
 * Features:
 * - Create new 2-of-3 multisig escrow contracts
 * - View all active/pending/completed contracts
 * - Release funds to seller (buyer + seller signatures)
 * - Refund to buyer (after timelock expiry)
 * - Real-time status updates via WebSocket
 * - Transaction history and event log
 * - Contract details with decoded Dinero Script
 *
 * RPC Methods Used:
 * - contract.createescrow    - Create new escrow
 * - contract.list            - List all contracts
 * - contract.status          - Get contract details
 * - contract.setlocktx       - Set funding transaction
 * - contract.broadcastrelease - Release funds to seller
 * - contract.broadcastrefund  - Refund to buyer
 * - contract.getsighash      - Get data for external signing
 */
class EscrowWidget : public QWidget {
    Q_OBJECT

public:
    explicit EscrowWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent = nullptr);
    ~EscrowWidget();

private Q_SLOTS:
    // Contract Management
    void onCreateEscrow();
    void onReleaseSelected();
    void onRefundSelected();
    void onViewDetails();
    void onViewScript();
    void onCopyAddress();
    void onExportCsv();
    void onExportSighash();
    void onImportSignatures();
    void onShowQRCode();
    void onExportQRCode();
    void onImportFromQR();

    // Data Updates
    void onRefreshContracts();
    void onContractSelected(const QModelIndex& index);

    // RPC Callbacks
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

    // WebSocket Events
    void onWebSocketEvent(const QString& topic, const QJsonObject& data);
    void onContractStatusUpdate(const QString& contractId, const QString& status);
    void onNewBlock(int height);

private:
    void setupUi();
    void setupConnections();
    void callRpc(const QString& method, const QJsonArray& params = QJsonArray());
    void populateTable(const QJsonArray& contracts);
    void updateContractRow(const QString& contractId, const QJsonObject& data);
    void appendLog(const QString& message);
    QString formatStatus(const QString& status);
    QColor statusColor(const QString& status);
    QString formatTimelock(int blockHeight, int currentHeight);
    void showCreateDialog();
    void showDetailsDialog(const QJsonObject& contract);
    void showScriptDialog(const QString& redeemScript);
    void showQRDialog(const QString& data, const QString& title, bool allowSave = true);
    void showSighashQRDialog(const QString& jsonData, const QJsonObject& sighashData);
    void showImportQRDialog();

    // UI Components - Header
    QLabel* titleLabel_;
    QLabel* statsLabel_;
    QPushButton* createButton_;
    QPushButton* refreshButton_;
    QPushButton* exportButton_;

    // UI Components - Contracts Table
    QTableView* contractsTable_;
    QStandardItemModel* tableModel_;
    QLabel* filterLabel_;
    QComboBox* filterCombo_;

    // UI Components - Details Panel
    QGroupBox* detailsGroup_;
    QLabel* contractIdLabel_;
    QLabel* p2shAddressLabel_;
    QLabel* amountLabel_;
    QLabel* statusLabel_;
    QLabel* buyerLabel_;
    QLabel* sellerLabel_;
    QLabel* mediatorLabel_;
    QLabel* lockTxidLabel_;
    QLabel* refundHeightLabel_;
    QLabel* currentHeightLabel_;
    QLabel* timeRemainingLabel_;
    QPushButton* copyAddressButton_;
    QPushButton* viewScriptButton_;
    QPushButton* exportSighashButton_;
    QPushButton* showQRButton_;
    QProgressBar* timelockProgress_;

    // UI Components - Action Buttons
    QPushButton* releaseButton_;
    QPushButton* refundButton_;
    QPushButton* viewDetailsButton_;
    QPushButton* importSigsButton_;

    // UI Components - Event Log
    QTextEdit* eventLog_;

    // Data
    RpcClient* rpc_;
    WebSocketClient* ws_;
    QTimer refreshTimer_;
    int currentBlockHeight_;
    QString selectedContractId_;
    QMap<QString, QJsonObject> contractsCache_;
    QString pendingExportFile_;
    bool exportForRelease_;

    // Status filter
    enum class StatusFilter {
        All,
        Pending,
        Locked,
        Released,
        Refunded,
        Expired
    };

    // Refresh interval
    static constexpr int REFRESH_INTERVAL_MS = 10000;  // 10 seconds

    // Table columns
    enum Column {
        COL_ID = 0,
        COL_P2SH,
        COL_AMOUNT,
        COL_STATUS,
        COL_BUYER,
        COL_SELLER,
        COL_REFUND_HEIGHT,
        COL_CREATED,
        COL_COUNT
    };
};
