#pragma once
// Orchard desktop candidate: bound account discovery, receive and payments.
// DIN_ENABLE_ORCHARD_UI remains off by default. Backend/desktop composition
// and release qualification are required; see the RPC contract document.
#include "orchardflow.h"

#include <QJsonValue>
#include <QWidget>

#include <memory>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class RpcClient;

class OrchardWidget : public QWidget {
    Q_OBJECT
public:
    explicit OrchardWidget(RpcClient* rpc, QWidget* parent = nullptr);

    // Called whenever the main window selects another wallet. Bumps the
    // generation: every reply requested for the previous wallet is ignored.
    void setWalletScope(const QString& walletName);
    void setWalletUnlocked(bool unlocked);
    void setChain(const QString& chain);

    // For tests: what the current payment is doing.
    std::optional<OrchardFlow::State> paymentState() const;
    quint64 generation() const { return generation_; }

Q_SIGNALS:
    // Every request this screen sends (for diagnostics and tests).
    void requestSent(const QString& method, const QJsonObject& params, const QString& tag);

private Q_SLOTS:
    void onConnectionContextChanged();
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void onRpcErrorDetailed(const QString& method, int code, const QString& message, const QJsonValue& data);
    void onAccountChanged(int index);
    void onCreateAccount();
    void onNewAddress();
    void onReview();
    void onRetry();
    void onResumeSelected();
    void onPoll();
    void updateForm();

private:
    void setupUi();
    void send(const QString& shortName, const QString& method, const QJsonObject& params);
    void startPayment(OrchardContract::Request request);
    void advance();
    void showPayment();
    void refreshAccounts();
    bool accountKnown() const;
    bool canChooseAccount() const;
    void clearAccountView();
    void refreshOperations();
    bool discoveryReady() const;
    bool hasOtherPendingOperation() const;
    const OrchardContract::Operation* selectedResumableOperation() const;
    void refreshBalance();
    void clearHistory(const QString& message);
    void refreshHistory();
    void requestHistory(quint64 offset,quint64 revision=0);
    void showHistory();
    void handle(const QString& shortName, const QJsonValue& result);
    void showStatus(const QString& text, bool error);

    RpcClient* rpc_;
    QString wallet_;
    QString walletBinding_;
    bool activationObserved_ = false;
    QString chain_;  // unknown until the selected connection reports its network
    quint64 generation_ = 0;
    bool unlocked_ = false;
    quint64 account_ = 0;
    bool accountSelected_ = false, accountsInFlight_ = false, creatingAccount_ = false;
    std::optional<OrchardContract::AccountsReply> accountsSnapshot_;
    quint64 balanceRevision_ = 0;  // reject balance replies older than an observed account revision
    quint64 revision_ = 0;  // 0 = account not set up / unknown
    std::unique_ptr<OrchardFlow::OrchardPayment> payment_;
    std::optional<OrchardContract::OperationsReply> operationsSnapshot_;
    bool callInFlight_ = false;
    bool operationsInFlight_ = false, balanceInFlight_ = false, activationInFlight_ = false;

    std::optional<OrchardContract::ReceivedReply> historySnapshot_;
    QString historyRequestTag_;
    bool historyInFlight_=false;
    quint64 historyRequestedOffset_=0,historyRequestedRevision_=0;
    QLabel* historyStatus_;
    QTableWidget* history_;
    QPushButton* historyRefresh_;
    QPushButton* historyPrevious_;
    QPushButton* historyNext_;

    QComboBox* accountSelector_;
    QLabel* accountStatus_;
    QPushButton* createAccount_;
    QLabel* balance_;
    QPushButton* newAddress_;
    QLineEdit* receiveAddress_;
    QPushButton* copyAddress_;
    QComboBox* mode_;
    QLineEdit* recipient_;
    QLineEdit* amount_;
    QLineEdit* fee_;
    QLineEdit* memo_;
    QLabel* formHint_;
    QPushButton* review_;
    QLabel* paymentState_;
    QLabel* paymentDetail_;
    QPushButton* retry_;
    QPushButton* resume_;
    QTableWidget* operations_;
    QLabel* operationsEmpty_;
    QLabel* status_;
    QTimer* poll_;
};
