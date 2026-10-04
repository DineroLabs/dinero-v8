#include "swapwidget.h"

#include "rpcclient.h"
#include "swapformpolicy.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// Reply tags: every swap request is routed back under its own name.
const QString kList = "swaptab.list";
const QString kOffer = "swaptab.offer";
const QString kDecode = "swaptab.decode";
const QString kAccept = "swaptab.accept";
const QString kStart = "swaptab.start";
const QString kCancel = "swaptab.cancel";
const QString kChain = "swaptab.chain";

QString NetworkFromChain(const QString& chain) {
    const QString c = chain.toLower();
    if (c == "main" || c == "mainnet") return "mainnet";
    if (c == "test" || c == "testnet") return "testnet";
    return "regtest";
}

QPlainTextEdit* ReadOnlyText(QWidget* parent, int height) {
    auto* t = new QPlainTextEdit(parent);
    t->setReadOnly(true);
    t->setMaximumHeight(height);
    t->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return t;
}
}  // namespace

SwapWidget::SwapWidget(RpcClient* rpc, QWidget* parent) : QWidget(parent), rpc_(rpc) {
    setupUi();
    connect(rpc_, &RpcClient::rpcResult, this, &SwapWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &SwapWidget::onRpcError);
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &SwapWidget::refresh);
    timer_->start(10000);
    rpc_->callNamedAs("getblockchaininfo", QJsonObject{}, kChain);
    refresh();
}

void SwapWidget::setupUi() {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        "<b>Swap DIN ↔ BTC</b> — a trustless atomic swap: both sides lock coins in scripts that either complete "
        "together or return to their owners. No exchange, no custody.<br>"
        "<span style='color:#e0a64a'>Two duties: keep this node online and the wallet unlocked until the swap "
        "finishes (the BTC buyer can run <i>dinero-swap-tower</i> instead), and start only swaps you can follow "
        "through.</span>",
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* top = new QHBoxLayout;
    // --- Sell DIN: make an offer ---
    auto* sell = new QGroupBox("Sell DIN for BTC — make an offer", this);
    auto* sellForm = new QFormLayout(sell);
    dinAmount_ = new QLineEdit(sell);
    dinAmount_->setObjectName("dinAmount");
    dinAmount_->setPlaceholderText("e.g. 100.0");
    btcAmount_ = new QLineEdit(sell);
    btcAmount_->setObjectName("btcAmount");
    btcAmount_->setPlaceholderText("e.g. 0.01");
    btcAddress_ = new QLineEdit(sell);
    btcAddress_->setObjectName("btcAddress");
    btcAddress_->setPlaceholderText("your Bitcoin address (receives the BTC)");
    offerReview_ = new QLabel(sell);
    offerReview_->setWordWrap(true);
    createOffer_ = new QPushButton("Create offer…", sell);
    createOffer_->setObjectName("createOffer");
    offerOut_ = ReadOnlyText(sell, 70);
    offerOut_->setObjectName("offerOut");
    offerOut_->setPlaceholderText("The offer to send to the buyer appears here.");
    copyOffer_ = new QPushButton("Copy offer", sell);
    copyOffer_->setEnabled(false);
    sellForm->addRow("DIN you sell", dinAmount_);
    sellForm->addRow("BTC you want", btcAmount_);
    sellForm->addRow("Your BTC address", btcAddress_);
    sellForm->addRow(offerReview_);
    sellForm->addRow(createOffer_);
    sellForm->addRow(offerOut_);
    sellForm->addRow(copyOffer_);
    top->addWidget(sell);

    // --- Paste an offer (buy DIN) or an accept (start your offer) ---
    auto* paste = new QGroupBox("Paste an offer (buy DIN) or a buyer's accept", this);
    auto* pasteForm = new QFormLayout(paste);
    pasteIn_ = new QPlainTextEdit(paste);
    pasteIn_->setObjectName("pasteIn");
    pasteIn_->setMaximumHeight(70);
    pasteIn_->setPlaceholderText("dinswap1o… (an offer)  or  dinswap1a… (the buyer's answer to your offer)");
    btcRefundAddress_ = new QLineEdit(paste);
    btcRefundAddress_->setObjectName("btcRefundAddress");
    btcRefundAddress_->setPlaceholderText("buying DIN: your Bitcoin address for a refund");
    reviewPasted_ = new QPushButton("Review…", paste);
    reviewPasted_->setObjectName("reviewPasted");
    acceptOut_ = ReadOnlyText(paste, 70);
    acceptOut_->setObjectName("acceptOut");
    acceptOut_->setPlaceholderText("Your accept (send it back to the seller) appears here.");
    copyAccept_ = new QPushButton("Copy accept", paste);
    copyAccept_->setEnabled(false);
    pasteForm->addRow(pasteIn_);
    pasteForm->addRow("BTC refund address", btcRefundAddress_);
    pasteForm->addRow(reviewPasted_);
    pasteForm->addRow(acceptOut_);
    pasteForm->addRow(copyAccept_);
    top->addWidget(paste);
    root->addLayout(top);

    // --- Swaps ---
    auto* list = new QGroupBox("Your swaps", this);
    auto* listLayout = new QVBoxLayout(list);
    table_ = new QTableWidget(0, 7, list);
    table_->setObjectName("swapTable");
    table_->setHorizontalHeaderLabels({"Swap", "Role", "Status", "DIN", "BTC", "BTC deadline", "DIN deadline"});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    events_ = ReadOnlyText(list, 110);
    events_->setPlaceholderText("Select a swap to see what the node did.");
    auto* row = new QHBoxLayout;
    cancel_ = new QPushButton("Cancel swap", list);
    cancel_->setObjectName("cancelSwap");
    cancel_->setEnabled(false);
    cancel_->setToolTip("Only before your coins are locked");
    auto* refreshButton = new QPushButton("Refresh", list);
    refreshButton->setObjectName("refreshSwaps");
    row->addWidget(cancel_);
    row->addStretch();
    row->addWidget(refreshButton);
    listLayout->addWidget(table_);
    listLayout->addWidget(events_);
    listLayout->addLayout(row);
    root->addWidget(list, 1);

    status_ = new QLabel(this);
    status_->setObjectName("swapStatus");
    status_->setWordWrap(true);
    root->addWidget(status_);

    for (auto* e : {dinAmount_, btcAmount_, btcAddress_}) {
        connect(e, &QLineEdit::textChanged, this, &SwapWidget::updateOfferReview);
    }
    updateOfferReview();
    connect(createOffer_, &QPushButton::clicked, this, &SwapWidget::onCreateOffer);
    connect(reviewPasted_, &QPushButton::clicked, this, &SwapWidget::onReviewPasted);
    connect(cancel_, &QPushButton::clicked, this, &SwapWidget::onCancelSelected);
    connect(refreshButton, &QPushButton::clicked, this, &SwapWidget::refresh);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &SwapWidget::onSelectionChanged);
    connect(copyOffer_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(offerOut_->toPlainText()); showStatus("Offer copied", false); });
    connect(copyAccept_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(acceptOut_->toPlainText()); showStatus("Accept copied", false); });
}

void SwapWidget::updateOfferReview() {
    const auto r = SwapFormPolicy::reviewOffer(dinAmount_->text(), btcAmount_->text(), btcAddress_->text(),
                                               SwapFormPolicy::btcHrpForChain(chain_));
    offerReview_->setText(r.blocker.isEmpty() ? QString("Rate: %1").arg(SwapFormPolicy::rate(r.dinUna, r.btcSat))
                                              : r.blocker);
    createOffer_->setEnabled(r.blocker.isEmpty());
}

void SwapWidget::showStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? "QLabel { color: #ff6b6b; }" : "QLabel { color: #9fd59f; }");
}

void SwapWidget::refresh() { rpc_->callNamedAs("swap.list", QJsonObject{}, kList); }

void SwapWidget::onCreateOffer() {
    const auto r = SwapFormPolicy::reviewOffer(dinAmount_->text(), btcAmount_->text(), btcAddress_->text(),
                                               SwapFormPolicy::btcHrpForChain(chain_));
    if (!r.blocker.isEmpty()) return showStatus(r.blocker, true);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString text =
        QString("You sell %1 DIN for %2 BTC (%3).\n\n"
                "Your DIN is locked as soon as the buyer accepts. It is released to the buyer only when you claim "
                "the BTC, or comes back to you after %4 (in about 4 days) if the buyer never locks BTC.\n"
                "You must claim the BTC before %5 (in about 2 days, minus a 6 h safety margin).\n\n"
                "Keep this node online and the wallet unlocked until the swap finishes.")
            .arg(SwapFormPolicy::formatUnits(r.dinUna), SwapFormPolicy::formatUnits(r.btcSat),
                 SwapFormPolicy::rate(r.dinUna, r.btcSat), SwapFormPolicy::localTime(now + 96 * 3600),
                 SwapFormPolicy::localTime(now + 48 * 3600));
    if (QMessageBox::question(this, "Review your offer", text, QMessageBox::Yes | QMessageBox::Cancel) !=
        QMessageBox::Yes) {
        return;
    }
    QJsonObject p{{"din_amount_una", r.dinUna}, {"btc_amount_sat", r.btcSat}, {"btc_address", btcAddress_->text().trimmed()}};
    rpc_->callNamedAs("swap.offer", p, kOffer);
    showStatus("Creating offer…", false);
}

void SwapWidget::onReviewPasted() {
    pastedText_ = pasteIn_->toPlainText().trimmed();
    const QString kind = SwapFormPolicy::kindOfText(pastedText_);
    if (kind.isEmpty()) return showStatus("Paste a dinswap1o… offer or a dinswap1a… accept", true);
    rpc_->callNamedAs("swap.decode", QJsonObject{{"text", pastedText_}}, kDecode);
}

void SwapWidget::acceptOffer(const QString& offerText, const QJsonObject& decoded) {
    const auto review = SwapFormPolicy::reviewDecodedOffer(decoded, NetworkFromChain(chain_),
                                                           QDateTime::currentSecsSinceEpoch());
    if (!review.blocker.isEmpty()) return showStatus(review.blocker, true);
    const QString refund = btcRefundAddress_->text().trimmed();
    if (!refund.toLower().startsWith(SwapFormPolicy::btcHrpForChain(chain_) + "1")) {
        return showStatus("Enter your Bitcoin refund address (" + SwapFormPolicy::btcHrpForChain(chain_) + "1…)", true);
    }
    if (QMessageBox::question(this, "Review the offer", review.text, QMessageBox::Yes | QMessageBox::Cancel) !=
        QMessageBox::Yes) {
        return;
    }
    rpc_->callNamedAs("swap.accept", QJsonObject{{"text", offerText}, {"btc_refund_address", refund}}, kAccept);
    showStatus("Accepting…", false);
}

void SwapWidget::startFromAccept(const QString& acceptText) {
    if (QMessageBox::question(this, "Start the swap",
                              "The buyer accepted your offer. Starting now locks your DIN.\n\nStart the swap?",
                              QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    rpc_->callNamedAs("swap.accept", QJsonObject{{"text", acceptText}}, kStart);
    showStatus("Starting the swap…", false);
}

void SwapWidget::onCancelSelected() {
    const int row = table_->currentRow();
    if (row < 0) return;
    const QString id = table_->item(row, 0)->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, "Cancel swap", "Cancel swap " + id + "? Nothing has been locked yet.",
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    rpc_->callNamedAs("swap.cancel", QJsonObject{{"id", id}}, kCancel);
}

void SwapWidget::onSelectionChanged() {
    const int row = table_->currentRow();
    if (row < 0) {
        cancel_->setEnabled(false);
        events_->clear();
        return;
    }
    const QJsonObject s = table_->item(row, 0)->data(Qt::UserRole + 1).toJsonObject();
    cancel_->setEnabled(SwapFormPolicy::canCancel(s.value("state").toString()));
    QStringList lines;
    for (const auto& e : s.value("events").toArray()) lines << e.toString();
    events_->setPlainText(lines.isEmpty() ? "No events yet." : lines.join("\n"));
}

void SwapWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (!method.startsWith("swaptab.")) return;
    // swap.* report refusals in-band as {"error": "..."}.
    if (result.isObject() && result.toObject().contains("error") && !result.toObject().value("error").isNull()) {
        const QString message = result.toObject().value("error").toVariant().toString();
        if (method == kList) {
            table_->setRowCount(0);
            showStatus(message.contains("disabled")
                           ? "Swaps are off on this node: start dinerod with swap.enable=1 and swap.btc_rpc=HOST:PORT"
                           : message,
                       true);
        } else {
            showStatus(message, true);
        }
        return;
    }
    if (method == kChain) {
        chain_ = result.toObject().value("chain").toString(chain_);
        updateOfferReview();  // the address prefix depends on the chain
        return;
    }
    if (method == kOffer) {
        offerOut_->setPlainText(result.toObject().value("offer").toString());
        copyOffer_->setEnabled(true);
        showStatus("Offer created — send it to the buyer, then paste their accept here.", false);
        refresh();
    } else if (method == kDecode) {
        const QJsonObject d = result.toObject();
        if (d.value("kind").toString() == "offer") acceptOffer(pastedText_, d);
        else startFromAccept(pastedText_);
    } else if (method == kAccept) {
        acceptOut_->setPlainText(result.toObject().value("accept").toString());
        copyAccept_->setEnabled(true);
        showStatus("Accepted — send your accept to the seller. Your node locks BTC once their DIN lock is deep enough.",
                   false);
        refresh();
    } else if (method == kStart) {
        showStatus("Swap started — your node is locking the DIN.", false);
        refresh();
    } else if (method == kCancel) {
        showStatus("Swap cancelled.", false);
        refresh();
    } else if (method == kList) {
        const QString selected =
            table_->currentRow() >= 0 ? table_->item(table_->currentRow(), 0)->data(Qt::UserRole).toString() : QString();
        const QJsonArray swaps = result.toArray();
        table_->setRowCount(swaps.size());
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (int i = 0; i < swaps.size(); ++i) {
            const QJsonObject s = swaps[i].toObject();
            const QString state = s.value("state").toString();
            const qint64 tBtc = s.value("t_btc_unix").toVariant().toLongLong();
            const qint64 tDin = s.value("t_din_unix").toVariant().toLongLong();
            auto deadline = [&](qint64 t) {
                return t > 0 ? SwapFormPolicy::localTime(t) + " (" + SwapFormPolicy::timeLeft(t - now) + ")" : QString();
            };
            auto* id = new QTableWidgetItem(s.value("id").toString());
            id->setData(Qt::UserRole, s.value("id").toString());
            id->setData(Qt::UserRole + 1, s);
            table_->setItem(i, 0, id);
            table_->setItem(i, 1, new QTableWidgetItem(s.value("role").toString() == "din-seller" ? "Selling DIN"
                                                       : s.value("role").toString().isEmpty() ? "" : "Buying DIN"));
            auto* st = new QTableWidgetItem(SwapFormPolicy::stateLabel(state, s.value("role").toString()));
            if (state == "lost") st->setForeground(QColor("#ff6b6b"));
            table_->setItem(i, 2, st);
            table_->setItem(i, 3, new QTableWidgetItem(
                                      SwapFormPolicy::formatUnits(s.value("din_amount_una").toVariant().toLongLong())));
            table_->setItem(i, 4, new QTableWidgetItem(
                                      SwapFormPolicy::formatUnits(s.value("btc_amount_sat").toVariant().toLongLong())));
            table_->setItem(i, 5, new QTableWidgetItem(deadline(tBtc)));
            table_->setItem(i, 6, new QTableWidgetItem(deadline(tDin)));
            if (s.value("id").toString() == selected) table_->selectRow(i);
        }
        onSelectionChanged();
    }
}

void SwapWidget::onRpcError(const QString& method, int code, const QString& message) {
    if (!method.startsWith("swaptab.") || method == kChain) return;
    if (method == kList && code == -32601) {
        table_->setRowCount(0);
        return showStatus("This node does not offer swaps (connect to your own dinerod with swap.enable=1).", true);
    }
    showStatus(message, true);
}
