#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QGroupBox>
#include <QTimer>
#include <QJsonObject>
#include <QJsonValue>

class RpcClient;

class DpiWidget : public QWidget {
    Q_OBJECT

public:
    explicit DpiWidget(RpcClient* rpc, QWidget* parent = nullptr);

public Q_SLOTS:
    // Drop wallet-bound state on wallet switch. Invoices encode the
    // active wallet's destination address; a tier-tracked txid was
    // produced by the previous wallet's UTXOs. Neither should bleed
    // into the next wallet's session.
    void clearWalletState();

private Q_SLOTS:
    void onCreateInvoice();
    void onCopyInvoice();
    void onVerifyPackage();
    void onDecodeInvoice();
    void onPayInvoice();
    void onCopyPackage();
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void onCountdownTick();
    void onTierPollTick();

private:
    void setupUI();
    void setupCollectTab();
    void setupPayTab();
    void startTierTracking(const QString& txid);
    void updateTierBadge(QLabel* badge, int tier, int confirmations);

    RpcClient* rpc_;
    QTabWidget* tabs_;

    // Collect tab
    QLineEdit* collectAmountEdit_;
    QLineEdit* collectMemoEdit_;
    QComboBox* collectExpiryCombo_;
    QPushButton* createInvoiceBtn_;
    QLabel* invoiceQrLabel_;
    QLabel* invoiceIdLabel_;
    QLabel* invoiceDestLabel_;
    QLabel* invoiceAmountLabel_;
    QLabel* invoiceExpiryLabel_;
    QTextEdit* invoiceTextEdit_;
    QPushButton* copyInvoiceBtn_;
    QGroupBox* detailsGroup_;
    QTextEdit* packageInputEdit_;
    QPushButton* verifyPackageBtn_;
    QLabel* verifyResultLabel_;
    QTextEdit* verifyDetailsEdit_;

    // Pay tab
    QTextEdit* payInvoiceInputEdit_;
    QPushButton* decodeInvoiceBtn_;
    QLabel* decodedAmountLabel_;
    QLabel* decodedDestLabel_;
    QLabel* decodedMemoLabel_;
    QLabel* decodedExpiryLabel_;
    QPushButton* payInvoiceBtn_;
    QLabel* payStatusLabel_;
    QTextEdit* packageOutputEdit_;
    QPushButton* copyPackageBtn_;

    // State
    QString collectInvoiceBase64_;
    QString payInvoiceBase64_;
    qint64 invoiceExpiryTimestamp_ = 0;
    QTimer* countdownTimer_;

    // Tier tracking
    QString trackedPayTxid_;
    QString trackedCollectTxid_;
    int payTier_ = 0;
    int payConfirmations_ = 0;
    int collectTier_ = 0;
    int collectConfirmations_ = 0;
    QTimer* tierPollTimer_;
    QLabel* payTierBadge_;
    QLabel* collectTierBadge_;
};
