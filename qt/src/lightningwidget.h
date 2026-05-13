#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QComboBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QJsonObject>

class RpcClient;

namespace dinero {

/**
 * Lightning Network Widget - Complete UI for Lightning Network operations
 *
 * Features:
 * - Channel Management (open/close channels, view balances)
 * - Invoice Creation/Payment (BOLT 11 with QR codes)
 * - Payment History
 * - Watchtower Registration
 * - Network Graph Visualization
 * - Real-time Payment Monitoring
 */
class LightningWidget : public QWidget {
    Q_OBJECT

public:
    explicit LightningWidget(RpcClient* rpc, QWidget* parent = nullptr);
    ~LightningWidget();

    // Refresh all data
    void refresh();

Q_SIGNALS:
    void statusMessage(const QString& message);
    void errorOccurred(const QString& error);

private Q_SLOTS:
    // Channel management
    void onOpenChannel();
    void onCloseChannel();
    void onForceCloseChannel();
    void onRefreshChannels();
    void onChannelSelected(int row, int column);

    // Invoice operations
    void onCreateInvoice();
    void onCreateOpenInvoice();
    void onPayInvoice();
    void onDecodeInvoice();
    void onGenerateQR();
    void onCopyInvoice();
    void onRefreshInvoices();

    // Payment operations
    void onRefreshPayments();
    void onCancelPayment();

    // Watchtower
    void onRegisterWatchtower();
    void onUnregisterWatchtower();
    void onRefreshWatchtowers();

    // Network
    void onRefreshNetworkInfo();
    void onConnectPeer();
    void onDisconnectPeer();
    void onFindRoute();

    // RPC responses
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& error);

    // Auto-refresh timer
    void onRefreshTimer();

private:
    void setupUI();
    void createChannelsTab();
    void createInvoicesTab();
    void createPaymentsTab();
    void createWatchtowerTab();
    void createNetworkTab();

    // Update UI with data
    void updateChannelTable(const QJsonArray& channels);
    void updateInvoiceTable(const QJsonArray& invoices);
    void updatePaymentTable(const QJsonArray& payments);
    void updateWatchtowerTable(const QJsonArray& watchtowers);
    void updateNetworkInfo(const QJsonObject& info);
    void updateNodeTable(const QJsonArray& nodes);

    // Utility functions
    QString formatAmount(qint64 amount_msat) const;
    QString formatTimestamp(qint64 timestamp) const;
    QString formatChannelStatus(const QString& status) const;
    QString formatPaymentStatus(const QString& status) const;
    QPixmap generateQRCode(const QString& data, int size = 300);

    RpcClient* rpc_;
    QTimer* refreshTimer_;

    // Main tab widget
    QTabWidget* tabs_;

    // === CHANNELS TAB ===
    QTableWidget* tblChannels_;
    QPushButton* btnOpenChannel_;
    QPushButton* btnCloseChannel_;
    QPushButton* btnForceCloseChannel_;
    QPushButton* btnRefreshChannels_;
    QLabel* lblTotalCapacity_;
    QLabel* lblLocalBalance_;
    QLabel* lblRemoteBalance_;
    QLabel* lblActiveChannels_;

    // Open channel dialog fields
    QLineEdit* edtPeerNodeId_;
    QLineEdit* edtChannelCapacity_;
    QLineEdit* edtPushAmount_;
    QSpinBox* spnMinConf_;

    // === INVOICES TAB ===
    QTableWidget* tblInvoices_;
    QPushButton* btnCreateInvoice_;
    QPushButton* btnCreateOpenInvoice_;
    QPushButton* btnPayInvoice_;
    QPushButton* btnDecodeInvoice_;
    QPushButton* btnRefreshInvoices_;
    QComboBox* cmbInvoiceFilter_;  // Filter: All/Pending/Paid/Expired

    // Invoice creation fields
    QLineEdit* edtInvoiceAmount_;
    QLineEdit* edtInvoiceDescription_;
    QSpinBox* spnInvoiceExpiry_;
    QLabel* lblInvoiceQR_;
    QLineEdit* edtInvoiceBolt11_;
    QPushButton* btnCopyInvoice_;
    QPushButton* btnGenerateQR_;

    // Invoice payment fields
    QLineEdit* edtPayBolt11_;
    QLineEdit* edtPayCustomAmount_;  // For open invoices
    QTextEdit* txtInvoiceDecoded_;
    QLabel* lblInvoiceStats_;

    // === PAYMENTS TAB ===
    QTableWidget* tblPayments_;
    QPushButton* btnRefreshPayments_;
    QPushButton* btnCancelPayment_;
    QComboBox* cmbPaymentFilter_;  // Filter: All/Pending/Success/Failed
    QLabel* lblPaymentStats_;
    QTextEdit* txtPaymentDetails_;

    // === WATCHTOWER TAB ===
    QTableWidget* tblWatchtowers_;
    QPushButton* btnRegisterWatchtower_;
    QPushButton* btnUnregisterWatchtower_;
    QPushButton* btnRefreshWatchtowers_;
    QLineEdit* edtWatchtowerUrl_;
    QLineEdit* edtWatchtowerReward_;
    QLabel* lblWatchtowerStats_;
    QTextEdit* txtWatchtowerInfo_;

    // === NETWORK TAB ===
    QTableWidget* tblNodes_;
    QPushButton* btnRefreshNetwork_;
    QPushButton* btnConnectPeer_;
    QPushButton* btnDisconnectPeer_;
    QPushButton* btnFindRoute_;
    QLineEdit* edtPeerAddress_;
    QLineEdit* edtRouteDestination_;
    QLineEdit* edtRouteAmount_;
    QTextEdit* txtRouteResult_;
    QLabel* lblNetworkStats_;

    // Network statistics
    QLabel* lblTotalNodes_;
    QLabel* lblTotalChannels_;
    QLabel* lblNetworkCapacity_;
    QLabel* lblAvgChannelSize_;

    // Track current selections
    QString selectedChannelId_;
    QString selectedPaymentHash_;
    QString selectedWatchtowerId_;
};

} // namespace dinero
