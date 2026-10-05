#include "swapwidget.h"

#include "chromestyle.h"
#include "paycollectpolicy.h"
#include "rpcclient.h"
#include "swapformpolicy.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

using PayCollectPolicy::Tone;

namespace {
// Reply tags: every swap request is routed back under its own name.
const QString kList = "swaptab.list";
const QString kOffer = "swaptab.offer";
const QString kDecode = "swaptab.decode";
const QString kAccept = "swaptab.accept";
const QString kStart = "swaptab.start";
const QString kCancel = "swaptab.cancel";
const QString kChain = "swaptab.chain";

// The app's layout: form and its review side by side at 3:2, 12 px apart.
constexpr int kFormStretch = 3;
constexpr int kCardStretch = 2;
constexpr int kGutter = 12;
const QString kDash = QString::fromUtf8("—");

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

QPushButton* ChromeButton(const QString& text, const QString& tip, const char* name, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setObjectName(name);
    b->setStyleSheet(chromeButtonStyle());
    b->setToolTip(tip);
    return b;
}

QLabel* Pill(Tone tone, const QString& text, const char* name, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setObjectName(name);
    l->setWordWrap(true);
    l->setStyleSheet(PayCollectPolicy::pillStyle(tone));
    l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);  // as tall as its wrapped text
    return l;
}

QLabel* Muted(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("QLabel { color: #8b949e; background: transparent; }");
    return l;
}

// A field name on a card (no page-coloured strip behind it).
QLabel* Plain(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("QLabel { background: transparent; }");
    return l;
}

QLabel* Value(const char* name, QWidget* parent) {
    auto* l = new QLabel(kDash, parent);
    l->setObjectName(name);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet("QLabel { background: transparent; }");
    return l;
}

// A form box and its card share one row at 3:2, whatever their content.
void ShareRow(QHBoxLayout* row, QGroupBox* form, QGroupBox* card) {
    row->setSpacing(kGutter);
    form->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(form, kFormStretch, Qt::AlignTop);  // each keeps its own height
    row->addWidget(card, kCardStretch, Qt::AlignTop);
}

// "To continue: ..." (amber) or a ready line (green), like Covenants.
void SetHint(QLabel* hint, const QString& blocker, const QString& ready) {
    hint->setText(blocker.isEmpty() ? ready : "To continue: " + blocker);
    hint->setStyleSheet(PayCollectPolicy::pillStyle(blocker.isEmpty() ? Tone::Good : Tone::Warn));
}

// The app's final review: a named action button, Cancel as the default.
bool Confirm(QWidget* parent, const QString& title, const QString& heading, const QString& details,
             const QString& action) {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setIcon(QMessageBox::Information);
    box.setText("<b>" + heading.toHtmlEscaped() + "</b>");
    box.setInformativeText(details);
    QPushButton* go = box.addButton(action, QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == go;
}

QString HtmlLines(const QString& text) { return text.toHtmlEscaped().replace("\n", "<br>"); }

Tone ToneOfState(const QString& state) {
    if (state == "done") return Tone::Good;
    if (state == "lost") return Tone::Bad;
    if (state == "refunded" || state == "aborted") return Tone::Info;
    if (state.startsWith("paused") || state == "sweeping") return Tone::Warn;
    return Tone::Neutral;
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
    root->setSpacing(kGutter);

    root->addWidget(Pill(Tone::Info,
                         "Swap DIN and BTC directly with another person. Both sides lock coins that either complete "
                         "together or go back to their owners: no exchange ever holds your coins.",
                         "swapIntro", this));
    root->addWidget(Pill(Tone::Warn,
                         "Keep this node online and the wallet unlocked until each swap finishes, and only start "
                         "swaps you can follow through. Buying DIN? A watchtower (dinero-swap-tower) can stand in "
                         "while you are offline.",
                         "swapDuties", this));

    // --- Row 1: sell DIN (make an offer) | its live review ---
    auto* sell = new QGroupBox("Sell DIN for BTC", this);
    sell->setObjectName("swapSellForm");
    auto* sellGrid = new QGridLayout(sell);
    sellGrid->setHorizontalSpacing(12);
    sellGrid->setVerticalSpacing(8);
    sellGrid->setColumnStretch(1, 1);
    dinAmount_ = new QLineEdit(sell);
    dinAmount_->setObjectName("dinAmount");
    dinAmount_->setPlaceholderText("e.g. 100");
    dinAmount_->setToolTip("How much DIN you sell (up to 8 decimals)");
    btcAmount_ = new QLineEdit(sell);
    btcAmount_->setObjectName("btcAmount");
    btcAmount_->setPlaceholderText("e.g. 0.001");
    btcAmount_->setToolTip("How much BTC you want for it (up to 8 decimals)");
    btcAddress_ = new QLineEdit(sell);
    btcAddress_->setObjectName("btcAddress");
    btcAddress_->setPlaceholderText("Your Bitcoin address");
    btcAddress_->setToolTip("The BTC is paid to this address of yours when you claim it");
    sellGrid->addWidget(Plain("DIN you sell:", sell), 0, 0);
    sellGrid->addWidget(dinAmount_, 0, 1);
    sellGrid->addWidget(Plain("BTC you want:", sell), 1, 0);
    sellGrid->addWidget(btcAmount_, 1, 1);
    sellGrid->addWidget(Plain("Your BTC address:", sell), 2, 0);
    sellGrid->addWidget(btcAddress_, 2, 1);
    offerHint_ = Pill(Tone::Warn, "", "swapOfferHint", sell);
    sellGrid->addWidget(offerHint_, 3, 0, 1, 2);
    createOffer_ = ChromeButton("Create offer", "Review the offer, then create the text you send to the buyer",
                                "createOffer", sell);
    sellGrid->addWidget(createOffer_, 4, 0, 1, 2, Qt::AlignLeft);
    offerOut_ = ReadOnlyText(sell, 70);
    offerOut_->setObjectName("offerOut");
    offerOut_->setToolTip("Send this text to the buyer");
    offerOut_->setVisible(false);
    sellGrid->addWidget(offerOut_, 5, 0, 1, 2);
    copyOffer_ = ChromeButton("Copy offer", "Copy the offer to the clipboard", "copyOffer", sell);
    copyOffer_->setVisible(false);
    sellGrid->addWidget(copyOffer_, 6, 0, 1, 2, Qt::AlignLeft);

    auto* review = new QGroupBox("Offer review", this);
    review->setObjectName("swapOfferReview");
    auto* reviewGrid = new QGridLayout(review);
    reviewGrid->setHorizontalSpacing(12);
    reviewGrid->setVerticalSpacing(8);
    reviewGrid->setColumnStretch(1, 1);
    reviewSell_ = Value("swapReviewSell", review);
    reviewReceive_ = Value("swapReviewReceive", review);
    reviewRate_ = Value("swapReviewRate", review);
    reviewClaim_ = Value("swapReviewClaim", review);
    reviewRefund_ = Value("swapReviewRefund", review);
    int r = 0;
    for (auto [name, value] : std::initializer_list<std::pair<const char*, QLabel*>>{
             {"You sell", reviewSell_},
             {"You receive", reviewReceive_},
             {"Rate", reviewRate_},
             {"Claim the BTC by", reviewClaim_},
             {"DIN back if unanswered", reviewRefund_}}) {
        reviewGrid->addWidget(Muted(name, review), r, 0);
        reviewGrid->addWidget(value, r++, 1);
    }
    auto* rule = new QLabel("Times are counted from when you create the offer.", review);
    rule->setWordWrap(true);
    rule->setStyleSheet("QLabel { color: #9fb3c8; background: transparent; }");
    reviewGrid->addWidget(rule, r, 0, 1, 2);

    auto* row1 = new QHBoxLayout;
    ShareRow(row1, sell, review);
    root->addLayout(row1);

    // --- Row 2: paste an offer (buy DIN) or a reply | how a swap goes ---
    auto* paste = new QGroupBox("Buy DIN, or start your offer", this);
    paste->setObjectName("swapPasteForm");
    auto* pasteGrid = new QGridLayout(paste);
    pasteGrid->setHorizontalSpacing(12);
    pasteGrid->setVerticalSpacing(8);
    pasteGrid->setColumnStretch(1, 1);
    pasteIn_ = new QPlainTextEdit(paste);
    pasteIn_->setObjectName("pasteIn");
    pasteIn_->setMaximumHeight(70);
    pasteIn_->setPlaceholderText("Paste an offer you received, or the buyer's reply to your offer");
    pasteIn_->setToolTip("An offer starts a purchase; a reply to your offer starts your sale");
    btcRefundAddress_ = new QLineEdit(paste);
    btcRefundAddress_->setObjectName("btcRefundAddress");
    btcRefundAddress_->setPlaceholderText("Your Bitcoin address (buying DIN only)");
    btcRefundAddress_->setToolTip("If the swap does not complete, your BTC comes back to this address");
    pasteGrid->addWidget(pasteIn_, 0, 0, 1, 2);
    pasteGrid->addWidget(Plain("BTC refund address:", paste), 1, 0);
    pasteGrid->addWidget(btcRefundAddress_, 1, 1);
    pasteHint_ = Pill(Tone::Warn, "", "swapPasteHint", paste);
    pasteGrid->addWidget(pasteHint_, 2, 0, 1, 2);
    reviewPasted_ = ChromeButton("Review", "Check the pasted offer or reply before anything is locked",
                                 "reviewPasted", paste);
    pasteGrid->addWidget(reviewPasted_, 3, 0, 1, 2, Qt::AlignLeft);
    acceptOut_ = ReadOnlyText(paste, 70);
    acceptOut_->setObjectName("acceptOut");
    acceptOut_->setToolTip("Send this reply back to the seller");
    acceptOut_->setVisible(false);
    pasteGrid->addWidget(acceptOut_, 4, 0, 1, 2);
    copyAccept_ = ChromeButton("Copy reply", "Copy your reply to the clipboard", "copyAccept", paste);
    copyAccept_->setVisible(false);
    pasteGrid->addWidget(copyAccept_, 5, 0, 1, 2, Qt::AlignLeft);

    auto* steps = new QGroupBox("How a swap goes", this);
    steps->setObjectName("swapSteps");
    auto* stepsLayout = new QVBoxLayout(steps);
    auto* stepsText = new QLabel(
        "1. The seller creates an offer and sends it to the buyer.<br>"
        "2. The buyer pastes it here and sends back a reply.<br>"
        "3. The seller pastes the reply: their node locks the DIN.<br>"
        "4. Once the DIN lock is deep enough, the buyer's node locks the BTC.<br>"
        "5. The seller claims the BTC, which lets the buyer claim the DIN.<br>"
        "If either side stops, both get their coins back after the deadlines.",
        steps);
    stepsText->setWordWrap(true);
    stepsText->setStyleSheet("QLabel { color: #9fb3c8; background: transparent; }");
    stepsLayout->addWidget(stepsText);

    auto* row2 = new QHBoxLayout;
    ShareRow(row2, paste, steps);
    root->addLayout(row2);

    status_ = new QLabel(this);
    status_->setObjectName("swapStatus");
    status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    status_->setVisible(false);
    root->addWidget(status_);

    // --- Your swaps ---
    auto* list = new QGroupBox("Your swaps", this);
    list->setObjectName("swapList");
    auto* listLayout = new QVBoxLayout(list);
    listEmpty_ = new QLabel("No swaps yet. Create an offer, or paste one you received.", list);
    listEmpty_->setObjectName("swapListEmpty");
    listEmpty_->setAlignment(Qt::AlignCenter);
    listEmpty_->setStyleSheet(
        "QLabel { color: #8b949e; padding: 24px; border: 1px dashed #3d434d; border-radius: 8px; "
        "background: transparent; }");
    table_ = new QTableWidget(0, 7, list);
    table_->setObjectName("swapTable");
    table_->setHorizontalHeaderLabels({"Swap", "Role", "Status", "DIN", "BTC", "BTC deadline", "DIN deadline"});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->setToolTip("Select a swap to see what your node did");
    table_->setVisible(false);
    events_ = ReadOnlyText(list, 110);
    events_->setObjectName("swapEvents");
    events_->setToolTip("What your node did for the selected swap");
    events_->setVisible(false);
    auto* buttons = new QHBoxLayout;
    cancel_ = ChromeButton("Cancel swap", "Cancel the selected swap. Only possible before your coins are locked",
                           "cancelSwap", list);
    cancel_->setEnabled(false);
    auto* refreshButton = ChromeButton("Refresh", "Ask your node for the latest state of every swap",
                                       "refreshSwaps", list);
    buttons->addWidget(cancel_);
    buttons->addStretch();
    buttons->addWidget(refreshButton);
    listLayout->addWidget(listEmpty_);
    listLayout->addWidget(table_);
    listLayout->addWidget(events_);
    listLayout->addLayout(buttons);
    root->addWidget(list);
    root->addStretch(1);  // spare height stays below the content

    for (auto* e : {dinAmount_, btcAmount_, btcAddress_}) {
        connect(e, &QLineEdit::textChanged, this, &SwapWidget::updateOfferReview);
    }
    connect(pasteIn_, &QPlainTextEdit::textChanged, this, &SwapWidget::updatePasteReview);
    connect(btcRefundAddress_, &QLineEdit::textChanged, this, &SwapWidget::updatePasteReview);
    updateOfferReview();
    updatePasteReview();
    connect(createOffer_, &QPushButton::clicked, this, &SwapWidget::onCreateOffer);
    connect(reviewPasted_, &QPushButton::clicked, this, &SwapWidget::onReviewPasted);
    connect(cancel_, &QPushButton::clicked, this, &SwapWidget::onCancelSelected);
    connect(refreshButton, &QPushButton::clicked, this, &SwapWidget::refresh);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &SwapWidget::onSelectionChanged);
    connect(copyOffer_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(offerOut_->toPlainText()); showStatus("Offer copied", false); });
    connect(copyAccept_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(acceptOut_->toPlainText()); showStatus("Reply copied", false); });
}

void SwapWidget::updateOfferReview() {
    const auto r = SwapFormPolicy::reviewOffer(dinAmount_->text(), btcAmount_->text(), btcAddress_->text(),
                                               SwapFormPolicy::btcHrpForChain(chain_));
    const bool ok = r.blocker.isEmpty();
    SetHint(offerHint_, r.blocker, "Ready. Create offer shows a final review before anything is created.");
    createOffer_->setEnabled(ok);
    reviewSell_->setText(ok ? SwapFormPolicy::formatUnits(r.dinUna) + " DIN" : kDash);
    reviewReceive_->setText(ok ? SwapFormPolicy::formatUnits(r.btcSat) + " BTC" : kDash);
    reviewRate_->setText(ok ? SwapFormPolicy::rate(r.dinUna, r.btcSat) : kDash);
    reviewClaim_->setText(ok ? SwapFormPolicy::timeLeft(48 * 3600) + " (minus 6 h)" : kDash);
    reviewRefund_->setText(ok ? "after " + SwapFormPolicy::timeLeft(96 * 3600) : kDash);
}

void SwapWidget::updatePasteReview() {
    const QString text = pasteIn_->toPlainText().trimmed();
    const QString kind = SwapFormPolicy::kindOfText(text);
    const QString hrp = SwapFormPolicy::btcHrpForChain(chain_);
    QString blocker;
    if (text.isEmpty()) {
        blocker = "paste an offer you received, or the buyer's reply to your offer";
    } else if (kind.isEmpty()) {
        blocker = "this is not a swap offer or reply; paste the full text you received";
    } else if (kind == "offer" && !btcRefundAddress_->text().trimmed().toLower().startsWith(hrp + "1")) {
        blocker = "enter your Bitcoin refund address (" + hrp + "1…)";
    }
    SetHint(pasteHint_, blocker,
            kind == "offer" ? "Ready. Review shows the offer's terms before you accept."
                            : "Ready. Review asks before your DIN is locked.");
    reviewPasted_->setEnabled(blocker.isEmpty());
}

void SwapWidget::showStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(PayCollectPolicy::pillStyle(error ? Tone::Bad : Tone::Good));
    status_->setVisible(!text.isEmpty());
}

void SwapWidget::refresh() { rpc_->callNamedAs("swap.list", QJsonObject{}, kList); }

void SwapWidget::onCreateOffer() {
    const auto r = SwapFormPolicy::reviewOffer(dinAmount_->text(), btcAmount_->text(), btcAddress_->text(),
                                               SwapFormPolicy::btcHrpForChain(chain_));
    if (!r.blocker.isEmpty()) return showStatus(r.blocker, true);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString heading = QString("You sell %1 DIN for %2 BTC")
                                .arg(SwapFormPolicy::formatUnits(r.dinUna), SwapFormPolicy::formatUnits(r.btcSat));
    const QString details =
        QString("<table style='border-spacing: 6px;'>"
                "<tr><td><b>Rate:</b></td><td>%1</td></tr>"
                "<tr><td><b>Claim the BTC by:</b></td><td>%2 (minus a 6 h safety margin)</td></tr>"
                "<tr><td><b>DIN back if unanswered:</b></td><td>after %3</td></tr>"
                "</table><br>"
                "Your DIN is locked as soon as the buyer accepts. It goes to the buyer only when you claim the BTC.<br>"
                "<b>Keep this node online and the wallet unlocked until the swap finishes.</b>")
            .arg(SwapFormPolicy::rate(r.dinUna, r.btcSat), SwapFormPolicy::localTime(now + 48 * 3600),
                 SwapFormPolicy::localTime(now + 96 * 3600));
    if (!Confirm(this, "Review your offer", heading, details, "Create offer")) return;
    QJsonObject p{{"din_amount_una", r.dinUna}, {"btc_amount_sat", r.btcSat}, {"btc_address", btcAddress_->text().trimmed()}};
    rpc_->callNamedAs("swap.offer", p, kOffer);
    showStatus("Creating offer…", false);
}

void SwapWidget::onReviewPasted() {
    pastedText_ = pasteIn_->toPlainText().trimmed();
    if (SwapFormPolicy::kindOfText(pastedText_).isEmpty()) {
        return showStatus("This is not a swap offer or reply. Paste the full text you received.", true);
    }
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
    const QString first = review.text.section('\n', 0, 0);
    const QString rest = review.text.section('\n', 1);
    if (!Confirm(this, "Review the offer", first, HtmlLines(rest.trimmed()), "Accept offer")) return;
    rpc_->callNamedAs("swap.accept", QJsonObject{{"text", offerText}, {"btc_refund_address", refund}}, kAccept);
    showStatus("Accepting…", false);
}

void SwapWidget::startFromAccept(const QString& acceptText) {
    if (!Confirm(this, "Start the swap", "The buyer accepted your offer.",
                 "Starting now locks your DIN.<br>Keep this node online and the wallet unlocked until the swap "
                 "finishes.",
                 "Start swap")) {
        return;
    }
    rpc_->callNamedAs("swap.accept", QJsonObject{{"text", acceptText}}, kStart);
    showStatus("Starting the swap…", false);
}

void SwapWidget::onCancelSelected() {
    const int row = table_->currentRow();
    if (row < 0) return;
    const QString id = table_->item(row, 0)->data(Qt::UserRole).toString();
    if (!Confirm(this, "Cancel swap", "Cancel this swap?",
                 "Swap " + id.toHtmlEscaped() + ". Nothing has been locked yet, so nothing is lost.", "Cancel swap")) {
        return;
    }
    rpc_->callNamedAs("swap.cancel", QJsonObject{{"id", id}}, kCancel);
}

void SwapWidget::onSelectionChanged() {
    const int row = table_->currentRow();
    if (row < 0) {
        cancel_->setEnabled(false);
        events_->clear();
        events_->setVisible(false);
        return;
    }
    const QJsonObject s = table_->item(row, 0)->data(Qt::UserRole + 1).toJsonObject();
    cancel_->setEnabled(SwapFormPolicy::canCancel(s.value("state").toString()));
    QStringList lines;
    for (const auto& e : s.value("events").toArray()) lines << e.toString();
    events_->setPlainText(lines.isEmpty() ? "No events yet." : lines.join("\n"));
    events_->setVisible(true);
}

void SwapWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (!method.startsWith("swaptab.")) return;
    // swap.* report refusals in-band as {"error": "..."}.
    if (result.isObject() && result.toObject().contains("error") && !result.toObject().value("error").isNull()) {
        const QString message = result.toObject().value("error").toVariant().toString();
        if (method == kList) {
            table_->setRowCount(0);
            table_->setVisible(false);
            listEmpty_->setVisible(true);
            showStatus(message.contains("disabled")
                           ? "Swaps are off on this node: start dinerod with swap.enable=1 and swap.btc_rpc=HOST:PORT "
                             "(on mainnet also swap.mainnet_beta=1; see the swap tester guide)"
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
        updatePasteReview();
        return;
    }
    if (method == kOffer) {
        offerOut_->setPlainText(result.toObject().value("offer").toString());
        offerOut_->setVisible(true);
        copyOffer_->setVisible(true);
        showStatus("Offer created — send it to the buyer, then paste their reply here.", false);
        refresh();
    } else if (method == kDecode) {
        const QJsonObject d = result.toObject();
        if (d.value("kind").toString() == "offer") acceptOffer(pastedText_, d);
        else startFromAccept(pastedText_);
    } else if (method == kAccept) {
        acceptOut_->setPlainText(result.toObject().value("accept").toString());
        acceptOut_->setVisible(true);
        copyAccept_->setVisible(true);
        showStatus("Accepted — send your reply to the seller. Your node locks BTC once their DIN lock is deep enough.",
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
        table_->setVisible(!swaps.isEmpty());
        listEmpty_->setVisible(swaps.isEmpty());
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (int i = 0; i < swaps.size(); ++i) {
            const QJsonObject s = swaps[i].toObject();
            const QString state = s.value("state").toString();
            const qint64 tBtc = s.value("t_btc_unix").toVariant().toLongLong();
            const qint64 tDin = s.value("t_din_unix").toVariant().toLongLong();
            auto deadline = [&](qint64 t) {
                return t > 0 ? SwapFormPolicy::localTime(t) + " (" + SwapFormPolicy::timeLeft(t - now) + ")" : QString();
            };
            const QString idText = s.value("id").toString();
            auto* id = new QTableWidgetItem(idText.left(8) + QString::fromUtf8("…"));
            id->setToolTip(idText);
            id->setData(Qt::UserRole, idText);
            id->setData(Qt::UserRole + 1, s);
            table_->setItem(i, 0, id);
            table_->setItem(i, 1, new QTableWidgetItem(s.value("role").toString() == "din-seller" ? "Selling DIN"
                                                       : s.value("role").toString().isEmpty() ? "" : "Buying DIN"));
            auto* st = new QTableWidgetItem(SwapFormPolicy::stateLabel(state, s.value("role").toString()));
            const Tone tone = ToneOfState(state);
            if (tone != Tone::Neutral) st->setForeground(QColor(PayCollectPolicy::toneColor(tone)));
            table_->setItem(i, 2, st);
            table_->setItem(i, 3, new QTableWidgetItem(
                                      SwapFormPolicy::formatUnits(s.value("din_amount_una").toVariant().toLongLong())));
            table_->setItem(i, 4, new QTableWidgetItem(
                                      SwapFormPolicy::formatUnits(s.value("btc_amount_sat").toVariant().toLongLong())));
            table_->setItem(i, 5, new QTableWidgetItem(deadline(tBtc)));
            table_->setItem(i, 6, new QTableWidgetItem(deadline(tDin)));
            if (idText == selected) table_->selectRow(i);
        }
        onSelectionChanged();
    }
}

void SwapWidget::onRpcError(const QString& method, int code, const QString& message) {
    if (!method.startsWith("swaptab.") || method == kChain) return;
    if (method == kList && code == -32601) {
        table_->setRowCount(0);
        table_->setVisible(false);
        listEmpty_->setVisible(true);
        return showStatus("This node does not offer swaps (connect to your own dinerod with swap.enable=1).", true);
    }
    showStatus(message, true);
}
