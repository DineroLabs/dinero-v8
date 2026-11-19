#pragma once
// Swap tab: DIN <-> BTC atomic swaps through the local node's swap.* RPCs
// (docs/design/din-btc-atomic-swaps.md), and DIN <-> ZEC when the node reports
// it (swap.capabilities; docs/design/din-zec-atomic-swaps.md: the buyer pays a
// per-swap deposit first, shown with its QR code). Copy-paste offers, a review screen
// before anything is committed, and a live list of swaps with their deadlines.
// The node does the work in the background; this tab only asks and shows.
#include <QJsonObject>
#include <QWidget>

class QFrame;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;
class QTableWidget;
class QTimer;
class RpcClient;

class SwapWidget : public QWidget {
    Q_OBJECT
public:
    explicit SwapWidget(RpcClient* rpc, QWidget* parent = nullptr);

private Q_SLOTS:
    void onCreateOffer();
    void onReviewPasted();
    void onCancelSelected();
    void onSelectionChanged();
    void refresh();
    void updateOfferReview();
    void updatePasteReview();
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void onChainPicked();
    void onAcceptDeposit();
    void onWithdrawDeposit();

private:
    void setupUi();
    void showStatus(const QString& text, bool error);
    void acceptOffer(const QString& offerText, const QJsonObject& decoded);
    void startFromAccept(const QString& acceptText);
    void showDeposit(const QJsonObject& status);
    void hideDeposit();

    RpcClient* rpc_;
    QString chain_{"mainnet"};
    QJsonObject lastDecoded_;
    QString pastedText_;

    double dinFeeEstimate_{0};  // una/vB from wallet.estimatefee (0: none yet)
    bool zecSupported_{false};  // the node reports "zec" in swap.capabilities
    bool lightBtc_{false};      // the node reports btc_backend "light": it sells DIN for BTC, it cannot buy
    QLabel* lightNotice_{nullptr};
    QLabel* depositTitle_{nullptr};
    QString depositCoin_{"ZEC"};  // the coin of the deposit being shown
    bool sellZec_{false};       // the sell form makes a ZEC offer
    QLabel* intro_;
    QGroupBox* sellBox_;
    QLabel* reviewClaimName_;
    QLabel* stepsText_;
    QLabel* chainLabel_;
    QWidget* chainPick_;
    QPushButton* chainBtc_;
    QPushButton* chainZec_;
    QLabel* btcAmountLabel_;
    QLabel* btcAddressLabel_;
    QLineEdit* dinAmount_;
    QLineEdit* btcAmount_;
    QLineEdit* btcAddress_;
    QLabel* offerHint_;
    QLabel* reviewSell_;
    QLabel* reviewReceive_;
    QLabel* reviewRate_;
    QLabel* reviewRefund_;
    QLabel* reviewClaim_;
    QPushButton* createOffer_;
    QPlainTextEdit* offerOut_;
    QPushButton* copyOffer_;

    QPlainTextEdit* pasteIn_;
    QLabel* refundLabel_;
    QLineEdit* btcRefundAddress_;
    QLabel* pasteHint_;
    QPushButton* reviewPasted_;
    QPlainTextEdit* acceptOut_;
    QPushButton* copyAccept_;

    // Bob's Zcash deposit, between swap.deposit and his accept.
    QString depositId_;
    QString depositOffer_;
    QString depositAddressText_;
    QFrame* depositBox_;
    QLabel* depositQr_;
    QLabel* depositAddress_;
    QLabel* depositDue_;
    QLabel* depositReceived_;
    QProgressBar* depositBar_;
    QLabel* depositHint_;
    QLabel* depositNetwork_{nullptr};  // which network to send on: wallets and exchanges offer several
    QPushButton* copyDeposit_;
    QPushButton* acceptDeposit_;
    QPushButton* withdrawDeposit_;

    QLabel* listEmpty_;
    QTableWidget* table_;
    QPlainTextEdit* events_;
    QPushButton* cancel_;
    QLabel* status_;
    QTimer* timer_;
};
