#pragma once
// Portfolio tab: every coin this wallet holds, and the built-in Bitcoin
// wallet's receive, send and history (docs/design/multi-coin-wallet.md).
// The node does the work (btc.* RPCs); this tab only asks and shows.
#include <functional>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class RpcClient;

class PortfolioWidget : public QWidget {
    Q_OBJECT
public:
    // Standalone: every coin including DIN. Within the Wallet page: the other
    // coins only (that page shows the DIN balance itself), as one more section.
    enum class Place { Standalone, WithinWallet };
    explicit PortfolioWidget(RpcClient* rpc, QWidget* parent = nullptr, Place place = Place::Standalone);

    // Tests answer the "turn off?" question without a dialog.
    void setTurnOffConfirmer(std::function<bool(const QString&)> confirm) { confirmTurnOff_ = std::move(confirm); }

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void refresh();
    void onTurnOn();
    void onTurnOff();
    void onNewAddress();
    void onReview();
    void onSend();
    void onRaiseFee();
    void onFormChanged();

private:
    void showCoins();
    void showBitcoin();
    // The user turned the Bitcoin wallet off here, and nothing else (swaps) keeps it on.
    bool switchedOff() const;
    std::function<bool(const QString&)> confirmTurnOff_;
    QPushButton* turnOff_;
    void showReview(const QJsonObject& payment, bool isBump);
    void clearReview();
    void say(const QString& text, bool problem);
    QJsonObject sendParams() const;

    RpcClient* rpc_;
    Place place_{Place::Standalone};
    double dinTotal_{-1};
    QJsonObject btcStatus_, btcBalance_;
    QString btcProblem_;
    QJsonObject reviewed_;     // the payment on show
    QJsonObject reviewedParams_;
    bool reviewIsBump_{false};

    QTableWidget* coins_;
    QGroupBox* btcBox_;
    QLabel* btcIntro_;
    QPushButton* turnOn_;
    QWidget* btcOn_;
    QLineEdit* address_;
    QPushButton* copyAddress_;
    QPushButton* newAddress_;
    QLineEdit* sendTo_;
    QLineEdit* sendAmount_;
    QCheckBox* sendAll_;
    QComboBox* speed_;
    QPushButton* review_;
    QLabel* reviewText_;
    QPushButton* send_;
    QLabel* status_;
    QTableWidget* history_;
    QPushButton* raiseFee_;
    QPushButton* refresh_;
};
