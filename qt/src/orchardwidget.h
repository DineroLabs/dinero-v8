#pragma once
// Orchard desktop candidate: bound account discovery, receive and payments.
// DIN_ENABLE_ORCHARD_UI remains off by default. Backend/desktop composition
// and release qualification are required; see the RPC contract document.
#include "orcharddisplaypolicy.h"
#include "orchardflow.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QWidget>

#include <memory>
#include <optional>

class QShowEvent;
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

    // The Wallet page shows this account's balance too. While it wants it,
    // the balance is read (slowly) even though this page is hidden.
    void setBalanceWanted(bool wanted);
    // The node's chain height, from the main window. An account's data can
    // only change when a block arrives, so that is when it is read again.
    void setTipHeight(quint64 height);
    std::optional<OrchardDisplay::KnownBalance> knownBalance() const { return lastBalance_; }
    // How long "ask again" refusals may go on before the balance is shown as
    // old (default kRefusedShownMs). For tests.
    void setRefusalGraceMs(int ms) { refusalGraceMs_ = ms; }

    // For tests: what the current payment is doing.
    std::optional<OrchardFlow::State> paymentState() const;
    quint64 generation() const { return generation_; }

Q_SIGNALS:
    // Every request this screen sends (for diagnostics and tests).
    void requestSent(const QString& method, const QJsonObject& params, const QString& tag);
    // The selected account's balance as this page now shows it (see
    // OrchardDisplay::KnownBalance). `known` false: never read, or another
    // wallet/account was selected.
    void balanceChanged(bool known, quint64 confirmedUna, bool current, quint64 height);

protected:
    void showEvent(QShowEvent* event) override;

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
    void onCancelSelected();
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
    const OrchardContract::Operation* selectedCancellableOperation() const;
    void refreshBalance();
    void clearHistory(const QString& message);
    void refreshHistory();
    void requestHistory(quint64 offset,quint64 revision=0);
    void showHistory();
    void handle(const QString& shortName, const QJsonValue& result);
    void showStatus(const QString& text, bool error, bool operationsError = false);
    void showBalance();           // from lastBalance_
    void balanceNoLongerCurrent(); // something newer exists than lastBalance_ covers
    void forgetBalance();          // another wallet or account
    void showOperationDetail();
    const OrchardContract::Operation* selectedOperation() const;

    RpcClient* rpc_;
    QString wallet_;
    QString walletBinding_;
    bool activationObserved_ = false;
    QString chain_;  // unknown until the selected connection reports its network
    quint64 generation_ = 0;
    bool unlocked_ = false;
    quint64 account_ = 0;
    bool accountSelected_ = false, accountsInFlight_ = false, creatingAccount_ = false;
    QElapsedTimer accountDiscoveryElapsed_;
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
    QPushButton* cancel_ = nullptr;
    QString cancelOperationId_;   // the request a cancel call is in flight for
    QTableWidget* operations_;
    QLabel* operationsEmpty_;
    QLabel* status_;
    bool operationsErrorVisible_ = false;
    bool paymentNeedsFollowing() const;
    QTimer* poll_;
    std::optional<OrchardDisplay::KnownBalance> lastBalance_;
    bool balanceWanted_ = false;
    quint64 tipHeight_ = 0, readAtTip_ = 0;
    QElapsedTimer sinceRead_;   // since the account's data was last asked for
    bool readRefused_ = false;  // the node said "ask again" to the last read
    QElapsedTimer refusedSince_; // the first refusal since the last answer
    int refusalGraceMs_;
    void noteAskAgain();
    bool readsDue() const;
    void markRead();
    QLabel* operationDetail_ = nullptr;
    QString detailTxid_;                    // the transaction whose chain details were asked for
    QHash<QString, QJsonObject> txDetails_; // txid -> blockchain.gettransaction answer
    QHash<QString, QString> txProblems_;    // txid -> why there are no details
};
