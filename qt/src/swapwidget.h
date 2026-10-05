#pragma once
// Swap tab: DIN <-> BTC atomic swaps through the local node's swap.* RPCs
// (docs/design/din-btc-atomic-swaps.md). Copy-paste offers, a review screen
// before anything is committed, and a live list of swaps with their deadlines.
// The node does the work in the background; this tab only asks and shows.
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QLineEdit;
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
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

private:
    void setupUi();
    void showStatus(const QString& text, bool error);
    void acceptOffer(const QString& offerText, const QJsonObject& decoded);
    void startFromAccept(const QString& acceptText);

    RpcClient* rpc_;
    QString chain_{"mainnet"};
    QJsonObject lastDecoded_;
    QString pastedText_;

    QLineEdit* dinAmount_;
    QLineEdit* btcAmount_;
    QLineEdit* btcAddress_;
    QLabel* offerReview_;
    QPushButton* createOffer_;
    QPlainTextEdit* offerOut_;
    QPushButton* copyOffer_;

    QPlainTextEdit* pasteIn_;
    QLineEdit* btcRefundAddress_;
    QPushButton* reviewPasted_;
    QPlainTextEdit* acceptOut_;
    QPushButton* copyAccept_;

    QTableWidget* table_;
    QPlainTextEdit* events_;
    QPushButton* cancel_;
    QLabel* status_;
    QTimer* timer_;
};
