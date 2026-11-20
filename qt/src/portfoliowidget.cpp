#include "portfoliowidget.h"

#include "chromestyle.h"
#include "paycollectpolicy.h"
#include "portfoliopolicy.h"
#include "rpcclient.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

using PayCollectPolicy::Tone;

namespace {
// Reply tags: every request of this tab comes back under its own name.
const QString kDin = "portfolio.din";
const QString kStatus = "portfolio.btcstatus";
const QString kBalance = "portfolio.btcbalance";
const QString kHistory = "portfolio.btchistory";
const QString kEnable = "portfolio.btcenable";
const QString kNewAddress = "portfolio.btcnewaddress";
const QString kPreview = "portfolio.btcpreview";
const QString kSend = "portfolio.btcsend";
const QString kBumpPreview = "portfolio.btcbumppreview";
const QString kBump = "portfolio.btcbump";

const char* const kTableStyle =
    "QTableWidget { background: #181b20; alternate-background-color: #20252b; color: #d6dde6; "
    "gridline-color: #2f343c; selection-background-color: #3e4550; selection-color: #ffffff; }";

QPushButton* Button(const QString& text, const QString& tip, const char* name, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setObjectName(name);
    b->setStyleSheet(chromeButtonStyle());
    b->setToolTip(tip);
    return b;
}

QTableWidget* Table(const QStringList& headers, const char* name, QWidget* parent) {
    auto* t = new QTableWidget(0, headers.size(), parent);
    t->setObjectName(name);
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setAlternatingRowColors(true);
    t->setStyleSheet(kTableStyle);
    t->verticalHeader()->setVisible(false);
    return t;
}

QString ErrorOf(const QJsonValue& result) {
    if (!result.isObject()) return {};
    const QJsonValue e = result.toObject().value("error");
    return e.isNull() || e.isUndefined() ? QString() : e.toVariant().toString();
}
}  // namespace

PortfolioWidget::PortfolioWidget(RpcClient* rpc, QWidget* parent) : QWidget(parent), rpc_(rpc) {
    auto* root = new QVBoxLayout(this);

    auto* coinsBox = new QGroupBox("Your coins", this);
    coinsBox->setObjectName("portfolioCoins");
    auto* coinsLayout = new QVBoxLayout(coinsBox);
    coins_ = Table({"Coin", "You hold", "How it is known"}, "coinsTable", coinsBox);
    coins_->setSelectionMode(QAbstractItemView::NoSelection);
    coins_->setMaximumHeight(170);
    refresh_ = Button("Refresh", "Ask your node again for every balance", "portfolioRefresh", coinsBox);
    auto* coinsButtons = new QHBoxLayout;
    coinsButtons->addStretch();
    coinsButtons->addWidget(refresh_);
    coinsLayout->addWidget(coins_);
    coinsLayout->addLayout(coinsButtons);
    root->addWidget(coinsBox);

    btcBox_ = new QGroupBox("Bitcoin wallet", this);
    btcBox_->setObjectName("portfolioBitcoin");
    auto* btcLayout = new QVBoxLayout(btcBox_);
    btcIntro_ = new QLabel(btcBox_);
    btcIntro_->setObjectName("bitcoinIntro");
    btcIntro_->setWordWrap(true);
    btcIntro_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);  // as tall as its text
    btcBox_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    coinsBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    turnOn_ = Button("Turn on the Bitcoin wallet",
                     "Calculates this wallet's Bitcoin addresses from your recovery words. The wallet must be unlocked",
                     "bitcoinTurnOn", btcBox_);
    btcLayout->addWidget(btcIntro_);
    auto* turnOnRow = new QHBoxLayout;
    turnOnRow->addWidget(turnOn_);
    turnOnRow->addStretch();
    btcLayout->addLayout(turnOnRow);

    btcOn_ = new QWidget(btcBox_);
    auto* on = new QVBoxLayout(btcOn_);
    on->setContentsMargins(0, 0, 0, 0);
    auto* receiveRow = new QHBoxLayout;
    address_ = new QLineEdit(btcOn_);
    address_->setObjectName("bitcoinAddress");
    address_->setReadOnly(true);
    address_->setToolTip("Give this address to whoever pays you Bitcoin, or paste it into an exchange's withdrawal form");
    copyAddress_ = Button("Copy", "Copy your Bitcoin receive address", "bitcoinCopyAddress", btcOn_);
    newAddress_ = Button("New address", "A fresh receive address. Earlier ones keep working", "bitcoinNewAddress", btcOn_);
    receiveRow->addWidget(new QLabel("Receive at:", btcOn_));
    receiveRow->addWidget(address_, 1);
    receiveRow->addWidget(copyAddress_);
    receiveRow->addWidget(newAddress_);
    on->addLayout(receiveRow);

    auto* sendBox = new QGroupBox("Send Bitcoin", btcOn_);
    sendBox->setObjectName("bitcoinSend");
    auto* form = new QFormLayout(sendBox);
    sendTo_ = new QLineEdit(sendBox);
    sendTo_->setObjectName("bitcoinSendTo");
    sendTo_->setPlaceholderText("The Bitcoin address to pay");
    sendAmount_ = new QLineEdit(sendBox);
    sendAmount_->setObjectName("bitcoinSendAmount");
    sendAmount_->setPlaceholderText("Amount in BTC, for example 0.0002");
    sendAll_ = new QCheckBox("Send everything in this wallet", sendBox);
    sendAll_->setObjectName("bitcoinSendAll");
    speed_ = new QComboBox(sendBox);
    speed_->setObjectName("bitcoinSpeed");
    speed_->addItem("Normal (about an hour)", 6);
    speed_->addItem("Fast (next blocks, higher fee)", 1);
    speed_->addItem("Slow (hours, lowest fee)", 24);
    review_ = Button("Review payment", "Shows the fee and what arrives. Nothing is sent yet", "bitcoinReview", sendBox);
    reviewText_ = new QLabel(sendBox);
    reviewText_->setObjectName("bitcoinReviewText");
    reviewText_->setWordWrap(true);
    reviewText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    reviewText_->setVisible(false);
    send_ = Button("Send", "Sends exactly the payment shown above. It cannot be taken back", "bitcoinSendNow", sendBox);
    send_->setVisible(false);
    form->addRow("To:", sendTo_);
    form->addRow("Amount:", sendAmount_);
    form->addRow("", sendAll_);
    form->addRow("Speed:", speed_);
    auto* reviewRow = new QHBoxLayout;
    reviewRow->addWidget(review_);
    reviewRow->addStretch();
    form->addRow("", reviewRow);
    form->addRow("", reviewText_);
    auto* sendRow = new QHBoxLayout;
    sendRow->addWidget(send_);
    sendRow->addStretch();
    form->addRow("", sendRow);
    on->addWidget(sendBox);

    history_ = Table({"", "Amount", "State", "Transaction"}, "bitcoinHistory", btcOn_);
    history_->setMaximumHeight(200);
    history_->setVisible(false);
    raiseFee_ = Button("Raise the fee", "Re-send the selected payment with a higher fee, taken from its change, so it "
                       "confirms sooner", "bitcoinRaiseFee", btcOn_);
    raiseFee_->setEnabled(false);
    auto* historyRow = new QHBoxLayout;
    historyRow->addWidget(raiseFee_);
    historyRow->addStretch();
    on->addWidget(history_);
    on->addLayout(historyRow);
    btcLayout->addWidget(btcOn_);

    status_ = new QLabel(btcBox_);
    status_->setObjectName("portfolioStatus");
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->setVisible(false);
    btcLayout->addWidget(status_);
    root->addWidget(btcBox_);
    root->addStretch(1);

    connect(rpc_, &RpcClient::rpcResult, this, &PortfolioWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &PortfolioWidget::onRpcError);
    connect(refresh_, &QPushButton::clicked, this, &PortfolioWidget::refresh);
    connect(turnOn_, &QPushButton::clicked, this, &PortfolioWidget::onTurnOn);
    connect(newAddress_, &QPushButton::clicked, this, &PortfolioWidget::onNewAddress);
    connect(copyAddress_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(address_->text());
        say("Address copied.", false);
    });
    connect(review_, &QPushButton::clicked, this, &PortfolioWidget::onReview);
    connect(send_, &QPushButton::clicked, this, &PortfolioWidget::onSend);
    connect(raiseFee_, &QPushButton::clicked, this, &PortfolioWidget::onRaiseFee);
    connect(sendTo_, &QLineEdit::textChanged, this, &PortfolioWidget::onFormChanged);
    connect(sendAmount_, &QLineEdit::textChanged, this, &PortfolioWidget::onFormChanged);
    connect(sendAll_, &QCheckBox::toggled, this, &PortfolioWidget::onFormChanged);
    connect(speed_, &QComboBox::currentIndexChanged, this, &PortfolioWidget::onFormChanged);
    connect(history_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = history_->currentRow();
        raiseFee_->setEnabled(row >= 0 && history_->item(row, 0)->data(Qt::UserRole).toBool());
    });
    showCoins();
    showBitcoin();
    onFormChanged();
}

void PortfolioWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}

// The Bitcoin balance asks public servers about this wallet's addresses.
// Turning the wallet on is the user's consent to that; before, nothing is asked.
void PortfolioWidget::refresh() {
    rpc_->callAs("wallet.getbalance", {}, kDin);
    rpc_->callNamedAs("btc.status", QJsonObject{}, kStatus);
}

void PortfolioWidget::say(const QString& text, bool problem) {
    status_->setText(text);
    status_->setStyleSheet(PayCollectPolicy::pillStyle(problem ? Tone::Warn : Tone::Good));
    status_->setVisible(!text.isEmpty());
}

void PortfolioWidget::showCoins() {
    const auto rows = PortfolioPolicy::coinRows(dinTotal_, btcStatus_, btcBalance_, btcProblem_);
    coins_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const QString values[3] = {rows[i].coin, rows[i].amount.isEmpty() ? QString::fromUtf8("—") : rows[i].amount, rows[i].note};
        for (int c = 0; c < 3; ++c) {
            auto* item = new QTableWidgetItem(values[c]);
            if (!rows[i].available) item->setForeground(QColor("#8b949e"));
            coins_->setItem(i, c, item);
        }
    }
}

void PortfolioWidget::showBitcoin() {
    const bool enabled = btcStatus_.value("enabled").toBool();
    const bool offered = btcStatus_.value("offered").toBool();
    btcOn_->setVisible(enabled);
    turnOn_->setVisible(!enabled && offered);
    if (enabled) {
        btcIntro_->setText("A Bitcoin wallet from your own recovery words: the same words restore it in any Bitcoin wallet. "
                           "Balances come from public Bitcoin servers and are checked against Bitcoin's own block headers; "
                           "the servers can see which addresses are yours.");
        address_->setText(btcStatus_.value("address").toString());
    } else if (offered) {
        btcIntro_->setText("Hold, receive and send Bitcoin in this wallet, with the recovery words you already have. Turning "
                           "it on calculates your Bitcoin addresses from those words (unlock the wallet first) and lets this "
                           "node ask public Bitcoin servers about them. Make sure your recovery words are written down: they "
                           "are the only backup of your Bitcoin too.");
    } else if (btcStatus_.isEmpty()) {
        btcIntro_->setText("Waiting for your node.");
    } else {
        btcIntro_->setText("The Bitcoin wallet is not available here: " +
                           btcStatus_.value("why_not").toString("this node does not offer it") + ".");
    }
}

QJsonObject PortfolioWidget::sendParams() const {
    QJsonObject p{{"address", sendTo_->text().trimmed()}, {"target_blocks", speed_->currentData().toInt()}};
    if (sendAll_->isChecked()) p["send_all"] = true;
    else p["amount_sat"] = QJsonValue(static_cast<qint64>(PortfolioPolicy::parseBtc(sendAmount_->text()).value_or(0)));
    return p;
}

void PortfolioWidget::clearReview() {
    reviewed_ = {};
    reviewText_->setVisible(false);
    send_->setVisible(false);
}

// Any change to the form withdraws the payment on show: what is sent is what was reviewed.
void PortfolioWidget::onFormChanged() {
    clearReview();
    sendAmount_->setEnabled(!sendAll_->isChecked());
    const bool amountOk = sendAll_->isChecked() || PortfolioPolicy::parseBtc(sendAmount_->text()).has_value();
    review_->setEnabled(!sendTo_->text().trimmed().isEmpty() && amountOk);
}

void PortfolioWidget::onTurnOn() {
    turnOn_->setEnabled(false);
    rpc_->callNamedAs("btc.enable", QJsonObject{}, kEnable);
}

void PortfolioWidget::onNewAddress() { rpc_->callNamedAs("btc.getnewaddress", QJsonObject{}, kNewAddress); }

void PortfolioWidget::onReview() {
    clearReview();
    reviewedParams_ = sendParams();
    review_->setEnabled(false);
    rpc_->callNamedAs("btc.preview", reviewedParams_, kPreview);
}

void PortfolioWidget::onRaiseFee() {
    const int row = history_->currentRow();
    if (row < 0) return;
    clearReview();
    reviewedParams_ = QJsonObject{{"txid", history_->item(row, 3)->data(Qt::UserRole).toString()}, {"target_blocks", 1}};
    rpc_->callNamedAs("btc.bumpfee", reviewedParams_, kBumpPreview);
}

void PortfolioWidget::onSend() {
    if (reviewed_.isEmpty()) return;
    send_->setEnabled(false);
    QJsonObject p = reviewedParams_;
    if (reviewIsBump_) {
        p["new_txid"] = reviewed_.value("txid").toString();
        rpc_->callNamedAs("btc.bumpfee", p, kBump);
    } else {
        p["txid"] = reviewed_.value("txid").toString();
        rpc_->callNamedAs("btc.send", p, kSend);
    }
}

void PortfolioWidget::showReview(const QJsonObject& payment, bool isBump) {
    reviewed_ = payment;
    reviewIsBump_ = isBump;
    reviewText_->setText(PortfolioPolicy::paymentReview(payment).join("\n"));
    reviewText_->setVisible(true);
    send_->setText(isBump ? "Send with the higher fee" : "Send");
    send_->setEnabled(true);
    send_->setVisible(true);
}

void PortfolioWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (!method.startsWith("portfolio.")) return;
    const QString error = ErrorOf(result);
    if (method == kDin) {
        if (error.isEmpty()) dinTotal_ = result.toObject().value("total").toDouble(-1);
        return showCoins();
    }
    if (method == kStatus) {
        // Swaps off, or an older node: no Bitcoin wallet, with the node's reason.
        btcStatus_ = error.isEmpty() ? result.toObject() : QJsonObject{{"offered", false}, {"why_not", error}};
        showBitcoin();
        showCoins();
        if (btcStatus_.value("enabled").toBool()) {
            rpc_->callNamedAs("btc.getbalance", QJsonObject{}, kBalance);
            rpc_->callNamedAs("btc.listtransactions", QJsonObject{}, kHistory);
        }
        return;
    }
    if (method == kBalance) {
        btcProblem_ = error;
        if (error.isEmpty()) btcBalance_ = result.toObject();
        return showCoins();
    }
    if (method == kHistory) {
        if (!error.isEmpty()) return;
        const auto rows = PortfolioPolicy::historyRows(result.toArray());
        history_->setRowCount(rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            auto* what = new QTableWidgetItem(rows[i].what);
            what->setData(Qt::UserRole, rows[i].canRaiseFee);
            history_->setItem(i, 0, what);
            history_->setItem(i, 1, new QTableWidgetItem(rows[i].amount));
            history_->setItem(i, 2, new QTableWidgetItem(rows[i].state));
            auto* txid = new QTableWidgetItem(rows[i].txid.left(16) + QString::fromUtf8("…"));
            txid->setToolTip(rows[i].txid);
            txid->setData(Qt::UserRole, rows[i].txid);
            history_->setItem(i, 3, txid);
        }
        history_->setVisible(!rows.isEmpty());
        raiseFee_->setEnabled(false);
        return;
    }
    if (method == kEnable) turnOn_->setEnabled(true);
    if (method == kPreview) review_->setEnabled(true);
    if (!error.isEmpty()) {
        if (method == kSend || method == kBump) clearReview();  // review it again
        return say(error, true);
    }
    if (method == kEnable) {
        say("The Bitcoin wallet is on.", false);
        return refresh();
    }
    if (method == kNewAddress) {
        address_->setText(result.toObject().value("address").toString());
        return say("A new receive address. Earlier ones keep working.", false);
    }
    if (method == kPreview) {
        say(QString(), false);
        return showReview(result.toObject(), false);
    }
    if (method == kBumpPreview) {
        say(QString(), false);
        return showReview(result.toObject(), true);
    }
    if (method == kSend || method == kBump) {
        clearReview();
        say("Sent. Transaction " + result.toObject().value("txid").toString(), false);
        if (method == kSend) {
            sendTo_->clear();
            sendAmount_->clear();
            sendAll_->setChecked(false);
        }
        return refresh();
    }
}

void PortfolioWidget::onRpcError(const QString& method, int, const QString& message) {
    if (!method.startsWith("portfolio.")) return;
    // The node did not answer (starting, or restarting): what is on show
    // stays. A wallet that is on must not turn into "not available".
    if (method == kStatus || method == kDin || method == kHistory) return;
    if (method == kBalance) {
        if (btcBalance_.isEmpty()) btcProblem_ = message;  // a balance already read stays on show
        return showCoins();
    }
    if (method == kEnable) turnOn_->setEnabled(true);
    if (method == kPreview) review_->setEnabled(true);
    if (method == kSend || method == kBump) clearReview();
    say(message, true);
}
