#include "orchardwidget.h"

#include "chromestyle.h"
#include "paycollectpolicy.h"
#include "rpcclient.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

using PayCollectPolicy::Tone;
namespace OC = OrchardContract;
namespace OF = OrchardFlow;

namespace {
constexpr int kFormStretch = 3, kCardStretch = 2, kGutter = 12;
constexpr int kPollMs = 3000;
const QString kDash = QString::fromUtf8("—");
const QString kUnknown = QStringLiteral("Unknown");

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
    l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    return l;
}
QLabel* Plain(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("QLabel { background: transparent; }");
    return l;
}
QLabel* Muted(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet("QLabel { color: #9fb3c8; background: transparent; }");
    return l;
}
void ShareRow(QHBoxLayout* row, QGroupBox* form, QGroupBox* card) {
    row->setSpacing(kGutter);
    form->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(form, kFormStretch, Qt::AlignTop);
    row->addWidget(card, kCardStretch, Qt::AlignTop);
}
Tone ToneOf(OF::State s) {
    switch (s) {
        case OF::State::Confirmed: return Tone::Good;
        case OF::State::Submitted: return Tone::Info;
        case OF::State::Rejected: return Tone::Bad;
        case OF::State::NeedsRetry: return Tone::Warn;
        case OF::State::Archived: return Tone::Neutral;
        default: return Tone::Neutral;
    }
}
QString FriendlyError(const QString& e) {
    if (e.contains("backend unavailable", Qt::CaseInsensitive) ||
        e.contains("Method not found", Qt::CaseInsensitive))
        return "Orchard isn't available on this node yet.";
    return e;
}
// Fee may be zero; amounts may not. Same exact parsing either way.
std::optional<quint64> ParseFee(const QString& text) {
    const QString t = text.trimmed();
    if (t == "0" || t == "0.0" || t == "0.00000000") return quint64(0);
    return OF::ParseDin(t);
}
}  // namespace

OrchardWidget::OrchardWidget(RpcClient* rpc, QWidget* parent) : QWidget(parent), rpc_(rpc) {
    setupUi();
    // A null client is a passive view: requests are observable, no I/O occurs.
    if (rpc_) {
        connect(rpc_, &RpcClient::rpcResult, this, &OrchardWidget::onRpcResult);
        connect(rpc_, &RpcClient::rpcErrorDetailed, this, &OrchardWidget::onRpcErrorDetailed);
        connect(rpc_, &RpcClient::connectionContextChanged, this, &OrchardWidget::onConnectionContextChanged);
    }
    poll_ = new QTimer(this);
    poll_->setInterval(kPollMs);
    connect(poll_, &QTimer::timeout, this, &OrchardWidget::onPoll);
    setChain(chain_);
}

void OrchardWidget::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(kGutter);
    root->addWidget(Pill(Tone::Info,
                         "Orchard preview — activation status is unknown until the node reports it.",
                         "orchardIntro", this));

    // --- Row 1: account | balance ---
    auto* account = new QGroupBox("Orchard account", this);
    account->setObjectName("orchardAccount");
    auto* accountGrid = new QGridLayout(account);
    accountSelector_=new QComboBox(account);
    accountSelector_->setObjectName("orchardAccountSelector");
    accountSelector_->setToolTip("Choose an existing account reported by this wallet. Account numbers need not be consecutive.");
    accountStatus_ = Plain("Accounts unknown", account);
    accountStatus_->setObjectName("orchardAccountStatus");
    createAccount_ = ChromeButton("Set up Orchard account",
                                  "Create the Orchard account in this wallet (account 0). Needs the wallet unlocked.",
                                  "orchardCreateAccount", account);
    accountGrid->addWidget(accountSelector_,0,0);
    accountGrid->addWidget(accountStatus_, 1, 0);
    accountGrid->addWidget(createAccount_, 2, 0, Qt::AlignLeft);
    accountGrid->addWidget(Muted("After setup the account needs to synchronize before it can spend.", account), 3, 0);
    connect(accountSelector_,&QComboBox::currentIndexChanged,this,&OrchardWidget::onAccountChanged);

    auto* balanceCard = new QGroupBox("Orchard balance", this);
    balanceCard->setObjectName("orchardBalanceCard");
    auto* balanceLayout = new QVBoxLayout(balanceCard);
    balance_ = Plain(kUnknown, balanceCard);
    balance_->setObjectName("orchardBalance");
    balance_->setToolTip("Shown as Unknown until the node can report it; never assumed to be zero");
    balanceLayout->addWidget(balance_);
    balanceLayout->addWidget(Muted("Unknown means the node did not report a balance, not that it is empty.", balanceCard));
    auto* row1 = new QHBoxLayout;
    ShareRow(row1, account, balanceCard);
    root->addLayout(row1);

    // --- Row 2: pay form | current payment ---
    auto* pay = new QGroupBox("Shield, send or unshield", this);
    pay->setObjectName("orchardPayForm");
    auto* g = new QGridLayout(pay);
    g->setHorizontalSpacing(12);
    g->setVerticalSpacing(8);
    g->setColumnStretch(1, 1);
    mode_ = new QComboBox(pay);
    mode_->setObjectName("orchardMode");
    mode_->addItem("Shield: public DIN into Orchard", "shield");
    mode_->addItem("Send privately to an Orchard address", "send");
    mode_->addItem("Unshield: Orchard to a public address", "unshield");
    mode_->setToolTip("What this payment does");
    recipient_ = new QLineEdit(pay);
    recipient_->setObjectName("orchardRecipient");
    recipient_->setToolTip("Who receives it");
    amount_ = new QLineEdit(pay);
    amount_->setObjectName("orchardAmount");
    amount_->setPlaceholderText("e.g. 1.5");
    amount_->setToolTip("Amount in DIN (up to 8 decimals)");
    fee_ = new QLineEdit("0.0001", pay);
    fee_->setObjectName("orchardFee");
    fee_->setToolTip("Network fee in DIN");
    memo_ = new QLineEdit(pay);
    memo_->setObjectName("orchardMemo");
    memo_->setPlaceholderText("Optional private note to the recipient");
    memo_->setToolTip("Only the recipient can read it (up to 512 bytes)");
    g->addWidget(Plain("Payment:", pay), 0, 0);
    g->addWidget(mode_, 0, 1);
    g->addWidget(Plain("To:", pay), 1, 0);
    g->addWidget(recipient_, 1, 1);
    g->addWidget(Plain("Amount (DIN):", pay), 2, 0);
    g->addWidget(amount_, 2, 1);
    g->addWidget(Plain("Fee (DIN):", pay), 3, 0);
    g->addWidget(fee_, 3, 1);
    g->addWidget(Plain("Note:", pay), 4, 0);
    g->addWidget(memo_, 4, 1);
    formHint_ = Pill(Tone::Warn, "", "orchardFormHint", pay);
    g->addWidget(formHint_, 5, 0, 1, 2);
    review_ = ChromeButton("Review", "Check the payment before anything is sent", "orchardReview", pay);
    g->addWidget(review_, 6, 0, 1, 2, Qt::AlignLeft);

    auto* current = new QGroupBox("Current payment", this);
    current->setObjectName("orchardCurrentPayment");
    auto* cl = new QVBoxLayout(current);
    paymentState_ = Pill(Tone::Neutral, "No payment in progress", "orchardPaymentState", current);
    paymentDetail_ = Muted("", current);
    paymentDetail_->setObjectName("orchardPaymentDetail");
    paymentDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    retry_ = ChromeButton("Retry", "Send the same request again. It can never create a second payment.",
                          "orchardRetry", current);
    retry_->setVisible(false);
    cl->addWidget(paymentState_);
    cl->addWidget(paymentDetail_);
    cl->addWidget(retry_, 0, Qt::AlignLeft);
    cl->addWidget(Muted("A finished proof is not a payment: it counts only once it is submitted and then "
                        "confirmed in a block.", current));
    auto* row2 = new QHBoxLayout;
    ShareRow(row2, pay, current);
    root->addLayout(row2);

    // --- Receive ---
    auto* receive = new QGroupBox("Receive privately", this);
    receive->setObjectName("orchardReceive");
    auto* rl = new QHBoxLayout(receive);
    newAddress_ = ChromeButton("New Orchard address", "Create a fresh private receiving address", "orchardNewAddress",
                               receive);
    receiveAddress_ = new QLineEdit(receive);
    receiveAddress_->setObjectName("orchardReceiveAddress");
    receiveAddress_->setReadOnly(true);
    receiveAddress_->setToolTip("Share this address to be paid privately");
    receiveAddress_->setVisible(false);
    copyAddress_ = ChromeButton("Copy", "Copy the address to the clipboard", "orchardCopyAddress", receive);
    copyAddress_->setVisible(false);
    receiveAddress_->setMinimumWidth(420);
    rl->addWidget(newAddress_);
    rl->addWidget(receiveAddress_, 1);
    rl->addWidget(copyAddress_);
    rl->addStretch();
    root->addWidget(receive);

    status_ = new QLabel(this);
    status_->setObjectName("orchardStatus");
    status_->setWordWrap(true);
    status_->setVisible(false);
    root->addWidget(status_);

    // --- Your operations (payments you started; not incoming history) ---
    auto* ops = new QGroupBox("Your operations", this);
    ops->setObjectName("orchardOperations");
    auto* ol = new QVBoxLayout(ops);
    ol->addWidget(Muted("Payments this wallet started. Incoming payments are not listed here.", ops));
    operationsEmpty_ = new QLabel("No Orchard operations yet.", ops);
    operationsEmpty_->setObjectName("orchardOperationsEmpty");
    operationsEmpty_->setAlignment(Qt::AlignCenter);
    operationsEmpty_->setStyleSheet(
        "QLabel { color: #8b949e; padding: 24px; border: 1px dashed #3d434d; border-radius: 8px; background: transparent; }");
    operations_ = new QTableWidget(0, 3, ops);
    operations_->setObjectName("orchardOperationsTable");
    operations_->setHorizontalHeaderLabels({"Operation", "State", "Transaction"});
    operations_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    operations_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    operations_->setAlternatingRowColors(true);
    operations_->verticalHeader()->setVisible(false);
    operations_->setVisible(false);
    ol->addWidget(operationsEmpty_);
    ol->addWidget(operations_);
    operations_->setSelectionBehavior(QAbstractItemView::SelectRows);
    operations_->setSelectionMode(QAbstractItemView::SingleSelection);
    resume_=new QPushButton("Resume selected",ops);
    resume_->setObjectName("orchardResume");
    resume_->setStyleSheet(chromeButtonStyle());
    resume_->setToolTip("Complete this node's saved request using its original recipients and fee. This may submit the payment.");
    ol->addWidget(resume_);
    connect(resume_,&QPushButton::clicked,this,&OrchardWidget::onResumeSelected);
    connect(operations_,&QTableWidget::itemSelectionChanged,this,&OrchardWidget::updateForm);
    root->addWidget(ops);

    auto* received=new QGroupBox("Received notes",this);
    auto* hl=new QVBoxLayout(received);
    hl->addWidget(Muted("Confirmed receipts, including notes already spent. Change is listed separately from incoming payments.",received));
    historyStatus_=new QLabel("History unknown",received);historyStatus_->setObjectName("orchardHistoryStatus");historyStatus_->setWordWrap(true);hl->addWidget(historyStatus_);
    history_=new QTableWidget(0,4,received);history_->setObjectName("orchardHistoryTable");
    history_->setHorizontalHeaderLabels({"Type","DIN received","Block","Transaction"});
    history_->setStyleSheet("QTableWidget { background: #181b20; alternate-background-color: #242932; color: #d6dde6; selection-background-color: #3e4550; selection-color: #f2f5f8; }");
    history_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);history_->verticalHeader()->setVisible(false);
    history_->setEditTriggers(QAbstractItemView::NoEditTriggers);history_->setAlternatingRowColors(true);history_->setMaximumHeight(210);history_->setVisible(false);hl->addWidget(history_);
    auto* hp=new QHBoxLayout();
    historyRefresh_=ChromeButton("Refresh","Read the latest authenticated receipt history.","orchardHistoryRefresh",received);
    historyPrevious_=ChromeButton("Previous","Read the previous page from this account revision.","orchardHistoryPrevious",received);
    historyNext_=ChromeButton("Next","Read the next page from this account revision.","orchardHistoryNext",received);
    hp->addWidget(historyRefresh_);hp->addStretch();hp->addWidget(historyPrevious_);hp->addWidget(historyNext_);hl->addLayout(hp);root->addWidget(received);
    connect(historyRefresh_,&QPushButton::clicked,this,[this]{requestHistory(0);});
    connect(historyPrevious_,&QPushButton::clicked,this,[this]{if(historySnapshot_ && historySnapshot_->offset)requestHistory(historySnapshot_->offset>=100?historySnapshot_->offset-100:0,historySnapshot_->revision);});
    connect(historyNext_,&QPushButton::clicked,this,[this]{if(historySnapshot_ && historySnapshot_->nextOffset)requestHistory(*historySnapshot_->nextOffset,historySnapshot_->revision);});
    root->addStretch(1);

    connect(createAccount_, &QPushButton::clicked, this, &OrchardWidget::onCreateAccount);
    connect(newAddress_, &QPushButton::clicked, this, &OrchardWidget::onNewAddress);
    connect(review_, &QPushButton::clicked, this, &OrchardWidget::onReview);
    connect(retry_, &QPushButton::clicked, this, &OrchardWidget::onRetry);
    connect(copyAddress_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(receiveAddress_->text()); showStatus("Address copied", false); });
    for (auto* e : {recipient_, amount_, fee_, memo_}) connect(e, &QLineEdit::textChanged, this, &OrchardWidget::updateForm);
    connect(mode_, &QComboBox::currentIndexChanged, this, &OrchardWidget::updateForm);
}

void OrchardWidget::setWalletScope(const QString& walletName) {
    // A request already accepted under the previous binding may still finish
    // for that wallet. New requests must obtain the new selection's binding.
    const bool unfinished = payment_ && !OF::IsFinal(payment_->state()) && payment_->state() != OF::State::Draft;
    const QString previous = wallet_;
    ++generation_;  // replies for the previous wallet no longer match
    wallet_ = walletName;
    walletBinding_.clear();
    clearHistory("History unknown");
    accountsSnapshot_.reset();accountSelected_=false;accountsInFlight_=false;creatingAccount_=false;
    {QSignalBlocker blocked(accountSelector_);accountSelector_->clear();}
    activationObserved_ = false;
    chain_.clear();
    revision_ = 0;
    balanceRevision_ = 0;
    payment_.reset();
    operationsSnapshot_.reset();
    callInFlight_ = false;
    operationsInFlight_=balanceInFlight_=activationInFlight_=false;
    poll_->stop();
    accountStatus_->setText(wallet_.isEmpty() ? "No wallet loaded" : "Checking…");
    balance_->setText(kUnknown);
    receiveAddress_->clear();
    receiveAddress_->setVisible(false);
    copyAddress_->setVisible(false);
    operations_->setRowCount(0);
    operations_->setVisible(false);
    operationsEmpty_->setVisible(true);
    showPayment();
    updateForm();
    showStatus(unfinished ? "A payment started in wallet \"" + previous +
                                "\" may still complete there. Open that wallet again to check it."
                          : QString(),
               true);
    if (!wallet_.isEmpty()) {
        send("binding", OC::kWalletBinding, {{"wallet_name", wallet_}});
        activationInFlight_=true;send("activation", OC::kActivationStatus, {});
        send("chain", "getblockchaininfo", {});
        poll_->start();
    }
}

void OrchardWidget::onConnectionContextChanged() {
    // Preserve the old request in memory, but revoke its ability to send on
    // this connection. A new binding requires an explicit wallet selection.
    auto previousPayment=std::move(payment_);
    const auto selectedWallet=wallet_;
    setWalletScope(QString()); // increments reply generation; stops polling; clears balances/addresses
    wallet_=selectedWallet;payment_=std::move(previousPayment);unlocked_=false;
    showPayment();retry_->setVisible(false);
    if(payment_) {
        paymentState_->setText("Connection changed — check the original node");
        paymentDetail_->setText("An earlier request may still complete on the previous node.");
    }
    accountStatus_->setText("Connection changed — reopen the wallet");
    updateForm();
    showStatus("RPC connection changed. Reopen the wallet and check earlier payments before paying again.",true);
}

void OrchardWidget::setWalletUnlocked(bool unlocked) {
    unlocked_ = unlocked;
    if(unlocked_)refreshAccounts();
    updateForm();
}

void OrchardWidget::setChain(const QString& chain) {
    if(chain_!=chain)activationObserved_=false;
    chain_ = chain;
    recipient_->setPlaceholderText(OF::OrchardHrp(chain_) + "1… or " + OF::TransparentHrp(chain_) + "1…");
    updateForm();
}

std::optional<OF::State> OrchardWidget::paymentState() const {
    if (!payment_) return std::nullopt;
    return payment_->state();
}

void OrchardWidget::send(const QString& shortName, const QString& method, const QJsonObject& params) {
    if (wallet_.isEmpty()) return;
    QJsonObject bound=params;
    if (method.startsWith("wallet.orchard.") && method!=OC::kWalletBinding) {
        if (!OC::ValidBinding(walletBinding_)) return;
        bound["wallet_binding"]=walletBinding_;
    }
    const auto requestId=shortName=="history" ? historyRequestTag_ :
        (shortName=="queue" || shortName=="finish") && payment_ ? payment_->requestId() : QString();
    const QString tag = OF::ReplyTag::Make(shortName, wallet_, generation_,requestId);
    const auto sendingGeneration=generation_;const auto sendingWallet=wallet_;const auto sendingBinding=walletBinding_;
    Q_EMIT requestSent(method, bound, tag);
    // A direct diagnostic observer can process a selection/context change.
    // Do not dispatch a captured request after that change.
    if(sendingGeneration!=generation_ || sendingWallet!=wallet_ || sendingBinding!=walletBinding_)return;
    if (rpc_) rpc_->callNamedAs(method, bound, tag);
}

bool OrchardWidget::accountKnown() const {
    if(!accountSelected_ || !accountsSnapshot_)return false;
    for(const auto& a:accountsSnapshot_->accounts)if(a.account==account_)return true;
    return false;
}
bool OrchardWidget::canChooseAccount() const {
    return !callInFlight_ && !creatingAccount_ && (!payment_ || OF::IsFinal(payment_->state()));
}
void OrchardWidget::clearAccountView() {
    clearHistory("History unknown");
    revision_=balanceRevision_=0;operationsSnapshot_.reset();
    operationsInFlight_=balanceInFlight_=false;balance_->setText(kUnknown);
    receiveAddress_->clear();receiveAddress_->setVisible(false);copyAddress_->setVisible(false);
    operations_->setRowCount(0);operations_->setVisible(false);operationsEmpty_->setVisible(true);
}
void OrchardWidget::refreshAccounts() {
    if(accountsInFlight_ || creatingAccount_ || wallet_.isEmpty() || !OC::ValidBinding(walletBinding_))return;
    accountsInFlight_=true;send("accounts",OC::kListAccounts,{});
}
void OrchardWidget::onAccountChanged(int index) {
    if(!accountsSnapshot_ || index<0 || index>=accountsSnapshot_->accounts.size())return;
    const auto& next=accountsSnapshot_->accounts[index];
    if(accountSelected_ && account_==next.account)return;
    if(!canChooseAccount()) {
        QSignalBlocker blocked(accountSelector_);int previous=-1;
        for(int i=0;i<accountsSnapshot_->accounts.size();++i)if(accountsSnapshot_->accounts[i].account==account_)previous=i;
        accountSelector_->setCurrentIndex(previous);return;
    }
    ++generation_; // Late balances, errors and effects belong to the old account selection.
    account_=next.account;accountSelected_=true;accountsInFlight_=false;activationInFlight_=false;
    payment_.reset();clearAccountView();revision_=next.revision;
    accountStatus_->setText(QString("Account %1 — checking saved payments").arg(account_));
    showPayment();refreshOperations();refreshBalance();refreshHistory();
    activationInFlight_=true;send("activation",OC::kActivationStatus,{});updateForm();
}
void OrchardWidget::refreshOperations() {
    if(operationsInFlight_ || !accountKnown() || !OC::ValidBinding(walletBinding_))return;
    operationsInFlight_=true;send("ops", OC::kListOperations, OC::AccountParams(account_));
}
void OrchardWidget::refreshBalance() {
    if(balanceInFlight_ || !accountKnown() || !OC::ValidBinding(walletBinding_))return;
    balanceInFlight_=true;send("balance", OC::kBalance, OC::AccountParams(account_));
}


void OrchardWidget::clearHistory(const QString& message) {
    historySnapshot_.reset();historyInFlight_=false;historyRequestTag_.clear();
    history_->setRowCount(0);history_->setVisible(false);historyStatus_->setText(message);
    historyPrevious_->setEnabled(false);historyNext_->setEnabled(false);
}
void OrchardWidget::requestHistory(quint64 offset,quint64 revision) {
    if(historyInFlight_ || !accountKnown() || !OC::ValidBinding(walletBinding_))return;
    if(offset && !revision)return;
    historyRequestedOffset_=offset;historyRequestedRevision_=revision;historyInFlight_=true;
    // Correlation exists only in the local transport tag, never a payment ID on the wire.
    historyRequestTag_=OC::NewRequestId();historyStatus_->setText("Loading receipt history…");
    history_->setVisible(false);historyPrevious_->setEnabled(false);historyNext_->setEnabled(false);historyRefresh_->setEnabled(false);
    send("history",OC::kListReceived,OC::ReceivedParams(account_,offset,revision));
}
void OrchardWidget::refreshHistory() {
    if(historyInFlight_ || !accountKnown())return;
    if(!historySnapshot_ || historySnapshot_->revision<revision_ || historySnapshot_->revision<balanceRevision_ ||
       (accountsSnapshot_ && historySnapshot_->sourceSequence<accountsSnapshot_->sourceSequence))requestHistory(0);
}
void OrchardWidget::showHistory() {
    if(!historySnapshot_)return;const auto& r=*historySnapshot_;
    history_->setRowCount(r.notes.size());
    for(int i=0;i<r.notes.size();++i) {
        const auto& n=r.notes[i];
        const QStringList cells{n.scope=="internal"?"Change":"Incoming",OF::FormatDin(n.amount),QString::number(n.height),n.txid.left(16)+QString::fromUtf8("…")};
        for(int col=0;col<cells.size();++col) {
            auto* item=new QTableWidgetItem(cells[col]);
            item->setToolTip("Transaction: "+n.txid+"\nRecipient (hex): "+n.recipientHex+"\nMemo (hex): "+n.memoHex);
            history_->setItem(i,col,item);
        }
    }
    history_->setVisible(!r.notes.isEmpty());
    const auto label=r.total?QString("Receipts %1–%2 of %3").arg(r.offset+1).arg(r.offset+quint64(r.notes.size())).arg(r.total):QString("No receipts at this checkpoint");
    historyStatus_->setText(label+QString(" — wallet block %1").arg(r.height)+(r.caughtUp?QString():QString(" — synchronizing")));
    historyPrevious_->setEnabled(r.offset>0);historyNext_->setEnabled(r.nextOffset.has_value());
}

void OrchardWidget::onCreateAccount() {
    if(!unlocked_ || !activationObserved_ || !OC::ValidBinding(walletBinding_) || creatingAccount_ ||
       !accountsSnapshot_ || !accountsSnapshot_->accounts.isEmpty() || !canChooseAccount())return;
    creatingAccount_=true;send("create",OC::kCreateAccount,OC::AccountParams(0));updateForm();
}
void OrchardWidget::onNewAddress() {
    if(!accountKnown() || !unlocked_ || !activationObserved_ || !OC::ValidBinding(walletBinding_))return;
    send("address", OC::kNewAddress, OC::AccountParams(account_));
}

bool OrchardWidget::discoveryReady() const {
    return accountKnown() && operationsSnapshot_ && operationsSnapshot_->account==account_ &&
        operationsSnapshot_->accountRevision>=revision_ && revision_!=0;
}
bool OrchardWidget::hasOtherPendingOperation() const {
    if(!discoveryReady())return true;
    for(const auto& op:operationsSnapshot_->operations)
        if(op.outcome!="confirmed" && (!payment_ || op.operationId!=payment_->requestId()))return true;
    return false;
}
const OC::Operation* OrchardWidget::selectedResumableOperation() const {
    if(!discoveryReady() || operationsSnapshot_->syncing() || !unlocked_ || !activationObserved_ ||
       !OC::ValidBinding(walletBinding_) || callInFlight_ ||
       (payment_ && !OF::IsFinal(payment_->state())))return nullptr;
    const auto row=operations_->currentRow();
    if(row<0 || row>=operationsSnapshot_->operations.size())return nullptr;
    const auto& op=operationsSnapshot_->operations[row];
    if(!op.outcome.isEmpty() || (op.completionMethod!=OC::kFinishShield && op.completionMethod!=OC::kFinishSpend))return nullptr;
    return &op;
}
void OrchardWidget::onResumeSelected() {
    const auto* selected=selectedResumableOperation();if(!selected)return;
    const auto reviewed=*selected;const auto generation=generation_;const auto binding=walletBinding_;
    const auto revision=operationsSnapshot_->accountRevision;
    QMessageBox box(this);box.setWindowTitle("Resume saved payment");box.setIcon(QMessageBox::Question);
    box.setText("Resume request "+reviewed.operationId+"?");
    box.setInformativeText("The node will use this request's original stored recipients and fee. This may submit the payment. No new payment will be created.");
    auto* go=box.addButton("Resume saved payment",QMessageBox::AcceptRole);box.addButton(QMessageBox::Cancel);box.setDefaultButton(QMessageBox::Cancel);box.exec();
    if(box.clickedButton()!=go)return;
    selected=selectedResumableOperation();
    if(generation!=generation_ || binding!=walletBinding_ || !selected ||
       operationsSnapshot_->accountRevision!=revision || selected->operationId!=reviewed.operationId ||
       selected->completionMethod!=reviewed.completionMethod || selected->txid!=reviewed.txid ||
       selected->durableState!=reviewed.durableState) {
        showStatus("Wallet or saved operation changed; review it again.",true);return;
    }
    auto resumed=OF::OrchardPayment::Resume(account_,*selected,revision);if(!resumed)return;
    payment_=std::make_unique<OF::OrchardPayment>(std::move(*resumed));callInFlight_=true;
    showPayment();updateForm();send("finish",payment_->finishMethod(),payment_->finishParams());
}

void OrchardWidget::updateForm() {
    const QString mode = mode_->currentData().toString();
    const QString to = recipient_->text().trimmed();
    QString blocker;
    if (wallet_.isEmpty()) blocker = "load a wallet";
    else if (walletBinding_.isEmpty()) blocker = "verify the selected wallet";
    else if (!activationObserved_) blocker = "wait for Orchard availability on this node";
    else if (!unlocked_) blocker = "unlock the wallet";
    else if (!accountKnown()) blocker = "discover and select an Orchard account";
    else if (revision_ == 0) blocker = "wait for the account state";
    else if (payment_ && !OF::IsFinal(payment_->state())) blocker = "wait for the current payment to finish";
    else if (!discoveryReady()) blocker = "wait for saved-payment discovery";
    else if (hasOtherPendingOperation()) blocker = "check the saved payments before starting another";
    else if (to.isEmpty()) blocker = "enter who receives it";
    else if (mode != "unshield" && !OF::LooksLikeOrchardAddress(to, chain_))
        blocker = "enter an Orchard address (" + OF::OrchardHrp(chain_) + "1…)";
    else if (mode == "unshield" && !OF::LooksLikeTransparentAddress(to, chain_))
        blocker = "enter a public address (" + OF::TransparentHrp(chain_) + "1…)";
    else if (!OF::ParseDin(amount_->text())) blocker = "enter an amount in DIN (up to 8 decimals)";
    else if (!ParseFee(fee_->text())) blocker = "enter the fee in DIN";
    else if (memo_->text().toUtf8().size() > OC::kMaxMemoBytes) blocker = "shorten the note (512 bytes at most)";
    else if (mode == "unshield" && !memo_->text().isEmpty()) blocker = "remove the note (public payments cannot carry one)";
    memo_->setEnabled(mode != "unshield");
    const bool emptyCatalog=accountsSnapshot_ && accountsSnapshot_->accounts.isEmpty();
    createAccount_->setVisible(emptyCatalog && !wallet_.isEmpty());
    createAccount_->setEnabled(emptyCatalog && !creatingAccount_ && canChooseAccount() && unlocked_ && activationObserved_ && OC::ValidBinding(walletBinding_));
    accountSelector_->setEnabled(accountsSnapshot_ && canChooseAccount() && OC::ValidBinding(walletBinding_));
    newAddress_->setEnabled(accountKnown() && unlocked_ && activationObserved_ && OC::ValidBinding(walletBinding_) && revision_!=0);
    review_->setEnabled(blocker.isEmpty());
    resume_->setEnabled(selectedResumableOperation()!=nullptr);
    historyRefresh_->setEnabled(!historyInFlight_ && accountKnown() && OC::ValidBinding(walletBinding_));
    formHint_->setText(blocker.isEmpty() ? "Ready. Review shows the payment before anything is sent."
                                         : "To continue: " + blocker);
    formHint_->setStyleSheet(PayCollectPolicy::pillStyle(blocker.isEmpty() ? Tone::Good : Tone::Warn));
}

void OrchardWidget::onReview() {
    const QString mode = mode_->currentData().toString();
    const auto amount = OF::ParseDin(amount_->text());
    const auto fee = ParseFee(fee_->text());
    if (!amount || !fee || revision_ == 0 || !unlocked_ || !activationObserved_ ||
        !OC::ValidBinding(walletBinding_) || !discoveryReady() || hasOtherPendingOperation() || (payment_ && !OF::IsFinal(payment_->state()))) return;
    const auto reviewedGeneration=generation_;
    const auto reviewedBinding=walletBinding_;
    OC::Request r;
    r.kind = mode == "shield" ? OC::Request::Kind::Shield : OC::Request::Kind::Spend;
    r.account = account_;
    r.requestId = OC::NewRequestId();
    r.expectedRevision = revision_;
    r.feeUna = *fee;
    const OC::Recipient to{recipient_->text().trimmed(), *amount, memo_->text().toUtf8()};
    if (mode == "unshield") r.outputs = {to};
    else r.payments = {to};

    QMessageBox box(this);
    box.setWindowTitle("Review payment");
    box.setIcon(QMessageBox::Information);
    box.setText("<b>" + QString(mode == "shield" ? "Shield %1 DIN" : mode == "send" ? "Send %1 DIN privately"
                                                                                    : "Unshield %1 DIN")
                            .arg(OF::FormatDin(*amount)) + "</b>");
    box.setInformativeText(
        QString("<table style='border-spacing: 6px;'><tr><td><b>To:</b></td><td>%1</td></tr>"
                "<tr><td><b>Fee:</b></td><td>%2 DIN</td></tr></table><br>"
                "Building the private proof can take a while. The payment counts only once it is "
                "submitted and confirmed.")
            .arg(to.address.toHtmlEscaped(), OF::FormatDin(*fee)));
    QPushButton* go = box.addButton("Send payment", QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go) return;
    // A modal dialog processes wallet-switch events. Approval is for the
    // captured selection only; never retarget its payment after the dialog.
    if (reviewedGeneration!=generation_ || reviewedBinding!=walletBinding_ ||
        !unlocked_ || !activationObserved_ || !discoveryReady() || hasOtherPendingOperation() ||
        (payment_ && !OF::IsFinal(payment_->state()))) {
        showStatus("Wallet selection changed; review the payment again.",true);return;
    }
    startPayment(std::move(r));
}

void OrchardWidget::startPayment(OC::Request request) {
    payment_ = std::make_unique<OF::OrchardPayment>(std::move(request));
    callInFlight_ = false;
    advance();
}

// Exactly one call per payment at a time; each reply decides the next step.
void OrchardWidget::advance() {
    showPayment();
    updateForm();
    if (!payment_ || callInFlight_ || !accountKnown() || !unlocked_ || !activationObserved_ || !OC::ValidBinding(walletBinding_)) return;
    switch (payment_->state()) {
        case OF::State::Draft:
            if(!payment_->canQueue())return;
            callInFlight_ = true;
            send("queue", OC::QueueMethod(payment_->request()), OC::QueueParams(payment_->request()));
            break;
        case OF::State::Queued:
        case OF::State::Proving:
        case OF::State::Submitted:
            if (!poll_->isActive()) poll_->start();
            break;
        case OF::State::Signed:
            if(payment_->requiresExplicitRetry()) {
                if(!poll_->isActive())poll_->start();
                break;
            }
            callInFlight_ = true;
            send("finish", payment_->finishMethod(), payment_->finishParams());
            break;
        case OF::State::NeedsRetry:
        case OF::State::Rejected:
        case OF::State::Confirmed:
        case OF::State::Archived:
            // Keep observing checkpoint/reorg changes; do not auto-resubmit.
            if(OC::ValidBinding(walletBinding_) && !poll_->isActive())poll_->start();
            break;
    }
}

void OrchardWidget::onPoll() {
    if(wallet_.isEmpty() || !OC::ValidBinding(walletBinding_))return;
    refreshAccounts();refreshOperations();refreshBalance();refreshHistory();
    if(!activationInFlight_){activationInFlight_=true;send("activation",OC::kActivationStatus,{});}
    if (!payment_ || callInFlight_ || !accountKnown() || !unlocked_ || !activationObserved_ || payment_->requiresExplicitRetry()) return;
    const auto s = payment_->state();
    if (s == OF::State::Queued || s == OF::State::Proving) {
        callInFlight_ = true;
        send("finish", payment_->finishMethod(), payment_->finishParams());
    } else if (s == OF::State::Submitted) {
        refreshOperations();
    }
}

void OrchardWidget::onRetry() {
    if (!payment_ || !payment_->canRetry() || callInFlight_ || !accountKnown() || !unlocked_ || !activationObserved_ || !OC::ValidBinding(walletBinding_)) return;
    payment_->adoptRevision(revision_);
    if(!payment_->authorizeRetry())return;
    callInFlight_ = true;
    // Same request id and parameters: the node returns the existing request.
    if (payment_->operationId().isEmpty() && payment_->canQueue())
        send("queue", OC::QueueMethod(payment_->request()), OC::QueueParams(payment_->request()));
    else
        send("finish", payment_->finishMethod(), payment_->finishParams());
}

void OrchardWidget::showPayment() {
    if (!payment_) {
        paymentState_->setText("No payment in progress");
        paymentState_->setStyleSheet(PayCollectPolicy::pillStyle(Tone::Neutral));
        paymentDetail_->clear();
        retry_->setVisible(false);
        return;
    }
    const auto s = payment_->state();
    paymentState_->setText(OF::Label(s));
    paymentState_->setStyleSheet(PayCollectPolicy::pillStyle(ToneOf(s)));
    QStringList lines;
    if (!payment_->detail().isEmpty()) lines << FriendlyError(payment_->detail());
    if (!payment_->txid().isEmpty()) lines << "Transaction: " + payment_->txid();
    paymentDetail_->setText(lines.join("\n"));
    retry_->setVisible(payment_->canRetry());
}

void OrchardWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    const auto tag = OF::ReplyTag::Parse(method);
    if (!tag) return;
    if (tag->wallet != wallet_ || tag->generation != generation_) return;  // previous wallet: ignore
    if((tag->method=="queue" || tag->method=="finish") &&
        (!payment_ || tag->requestId!=payment_->requestId()))return;
    if(tag->method=="history" && (!historyInFlight_ || tag->requestId!=historyRequestTag_))return;
    handle(tag->method, result);
}

void OrchardWidget::onRpcError(const QString& method, int code, const QString& message) {
    onRpcErrorDetailed(method, code, message, QJsonValue());
}

void OrchardWidget::onRpcErrorDetailed(const QString& method, int, const QString& message, const QJsonValue& data) {
    const auto tag = OF::ReplyTag::Parse(method);
    if (!tag || tag->wallet != wallet_ || tag->generation != generation_) return;
    if((tag->method=="queue" || tag->method=="finish") &&
        (!payment_ || tag->requestId!=payment_->requestId()))return;
    if(tag->method=="history" && (!historyInFlight_ || tag->requestId!=historyRequestTag_))return;
    QJsonObject failure{{"error", message}};
    const auto detail = data.toObject().value("orchard").toObject();
    auto token = [](const QJsonValue& value, int limit) {
        if (!value.isString()) return false;
        const auto text=value.toString();
        if(text.isEmpty() || text.size()>limit)return false;
        for(const auto c:text)if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_'))return false;
        return true;
    };
    if(token(detail.value("error_code"),64)) {
        failure["error_code"]=detail.value("error_code");
        if(token(detail.value("proof_state"),32))failure["proof_state"]=detail.value("proof_state");
        if(detail.value("reservation_retained").isBool())failure["reservation_retained"]=detail.value("reservation_retained");
    }
    handle(tag->method, failure);
}

void OrchardWidget::handle(const QString& shortName, const QJsonValue& result) {
    if(shortName=="accounts")accountsInFlight_=false;
    if(shortName=="ops")operationsInFlight_=false;
    if(shortName=="balance")balanceInFlight_=false;
    if(shortName=="activation")activationInFlight_=false;
    const auto object=result.toObject();
    if (object.value("error_code").toString().startsWith("wallet_binding_")) {
        walletBinding_.clear();accountsSnapshot_.reset();callInFlight_=false;poll_->stop();balance_->setText(kUnknown);
        clearHistory("History unavailable — wallet binding changed");
        showStatus("Wallet selection changed; reopen this wallet before retrying.",true);
        updateForm();return;
    }

    if(shortName=="history") {
        historyInFlight_=false;historyRequestTag_.clear();
        auto page=OC::ParseReceived(result);
        if(!page || !accountKnown() || page->account!=account_ || page->offset!=historyRequestedOffset_ || page->limit!=100 ||
           (historyRequestedRevision_ && page->revision!=historyRequestedRevision_) || page->revision<revision_ || page->revision<balanceRevision_) {
            const auto code=object.value("error_code").toString();
            clearHistory(code=="history_incomplete"?"History incomplete — this wallet needs receipt replay":
                code=="stale_account_revision"?"History changed — refresh to read the latest checkpoint":"Receipt history unavailable");
            updateForm();return;
        }
        historySnapshot_=std::move(*page);showHistory();updateForm();return;
    }
    if(shortName=="chain") {
        const auto network=result.isObject() && !OC::InBandError(result)?OC::CanonicalNetwork(object.value("chain").toString()):QString();
        if(network.isEmpty()){activationObserved_=false;showStatus("Cannot verify this node's network.",true);updateForm();return;}
        setChain(network);
        if(!activationInFlight_){activationInFlight_=true;send("activation",OC::kActivationStatus,{});}
        return;
    }
    if (shortName=="binding") {
        const auto binding=OC::ParseBinding(result,wallet_);
        if (!binding) {showStatus("Cannot verify the selected wallet.",true);return;}
        walletBinding_=*binding;refreshAccounts();updateForm();return;
    }
    if(shortName=="accounts") {
        const auto catalog=OC::ParseAccounts(result);
        bool stale=false;
        if(catalog && accountSelected_)for(const auto& a:catalog->accounts)
            if(a.account==account_ && (a.revision<revision_ || a.revision<balanceRevision_))stale=true;
        if(!catalog || stale) {
            accountsSnapshot_.reset();operationsSnapshot_.reset();balance_->setText(kUnknown);
            clearHistory("History unavailable — account discovery failed");
            accountStatus_->setText("Account discovery unavailable");updateForm();return;
        }
        accountsSnapshot_=*catalog;int selected=-1;
        {QSignalBlocker blocked(accountSelector_);accountSelector_->clear();
         for(int i=0;i<catalog->accounts.size();++i) {
             const auto& a=catalog->accounts[i];accountSelector_->addItem(QString("Account %1").arg(a.account),QVariant::fromValue(a.account));
             if(accountSelected_ && a.account==account_)selected=i;
         }
         accountSelector_->setCurrentIndex(selected);
        }
        if(selected<0 && accountSelected_) {
            ++generation_;accountSelected_=false;clearAccountView();activationInFlight_=false;
            accountStatus_->setText("Selected account is missing — reopen the wallet");
            showStatus("The selected account is no longer reported. Earlier payments may still complete; check them before paying again.",true);
        } else if(!accountSelected_ && !catalog->accounts.isEmpty() && canChooseAccount()) {
            accountSelector_->setCurrentIndex(0); // The first real catalog entry, never a guessed account zero.
        } else if(catalog->accounts.isEmpty())accountStatus_->setText("No accounts in the authenticated catalog");
        updateForm();return;
    }
    if (shortName=="activation") {
        const auto observed=OC::ParseActivation(result,chain_);
        const auto state=observed?observed->state:QString();
        activationObserved_=observed && observed->activeAtTip && observed->walletBackend;
        auto* intro=findChild<QLabel*>("orchardIntro");
        if (intro) intro->setText(activationObserved_ ? "Orchard rules are active. Wallet synchronization is still required."
            : state=="unscheduled" ? "Orchard activation is not scheduled on this node."
            : state=="scheduled" ? "Orchard is scheduled but not active on this node."
            : "Orchard availability is unknown or unavailable on this node.");
        updateForm();return;
    }
    if((shortName=="ops" || shortName=="balance") && !accountKnown())return;
    if (shortName == "create" || shortName == "address") {
        const bool creating=shortName=="create" && creatingAccount_;
        if(shortName=="create")creatingAccount_=false;
        const auto r = OC::ParseAccount(result,shortName=="create");
        if (!r || (creating ? r->account!=0 : !accountKnown() || r->account!=account_) || r->revision<revision_ || r->revision<balanceRevision_ ||
            !OF::LooksLikeOrchardAddress(r->address,chain_))
            return showStatus(FriendlyError(OC::InBandError(result).value_or("Unexpected account reply")), true);
        if(r->revision>balanceRevision_){balance_->setText(kUnknown);balance_->setToolTip("Waiting for the current account balance.");}
        if(creating){account_=r->account;accountSelected_=true;accountsSnapshot_.reset();refreshAccounts();}
        revision_ = r->revision;
        accountStatus_->setText(r->requiresSync ? QString("Set up — synchronizing") : QString("Address issued"));
        receiveAddress_->setText(r->address);
        receiveAddress_->setVisible(true);
        copyAddress_->setVisible(true);
        updateForm();
    } else if (shortName == "queue" || shortName == "finish") {
        callInFlight_ = false;
        if (!payment_) return;
        if (shortName == "queue") payment_->onQueueReply(result);
        else payment_->onFinishReply(result);
        advance();
    } else if (shortName == "ops") {
        const auto r = OC::ParseOperations(result);
        if (!r || r->account!=account_) {
            operationsSnapshot_.reset();
            const QString e = OC::InBandError(result).value_or("Unexpected reply");
            if (revision_ == 0) accountStatus_->setText(FriendlyError(e));
            else showStatus(FriendlyError(e), true);  // keep the known account state
            updateForm();
            return;
        }
        if(r->accountRevision<revision_)return;
        if(r->accountRevision>balanceRevision_){balance_->setText(kUnknown);balance_->setToolTip("Waiting for the current account balance.");}
        revision_ = r->accountRevision;
        operationsSnapshot_=*r;
        accountStatus_->setText(r->syncing() ? QString("Ready — synchronizing") : QString("Ready"));
        operations_->setRowCount(r->operations.size());
        for (int i = 0; i < r->operations.size(); ++i) {
            const auto& op = r->operations[i];
            const QString state = op.outcome == "confirmed" ? QString("Confirmed (block %1)").arg(op.height)
                                  : op.outcome == "conflicted" ? QString("Conflicted")
                                  : op.durableState == "signed" ? QString("Signed — not confirmed")
                                                                : QString("Queued");
            operations_->setItem(i, 0, new QTableWidgetItem(op.operationId.left(12) + QString::fromUtf8("…")));
            operations_->setItem(i, 1, new QTableWidgetItem(state));
            operations_->setItem(i, 2, new QTableWidgetItem(op.txid.isEmpty() ? kDash : op.txid.left(16) + QString::fromUtf8("…")));
        }
        operations_->setVisible(!r->operations.isEmpty());
        operationsEmpty_->setVisible(r->operations.isEmpty());
        if (payment_) {
            payment_->onOperations(*r);
            advance();
        }
        updateForm();
    } else if (shortName == "balance") {
        auto b = OC::ParseBalance(result);
        if(b && (b->account!=account_ || b->accountRevision<revision_ || b->accountRevision<balanceRevision_))b.reset();
        if(b)balanceRevision_=b->accountRevision;
        balance_->setText(b ? OF::FormatDin(b->confirmed) + " DIN confirmed" +
            (b->caughtUp ? QString() : QString(" — synchronizing")) : kUnknown);
        balance_->setToolTip(b ? QString("At the wallet checkpoint: %1 DIN reserved; %2 DIN unreserved. This is not a spendability guarantee.")
            .arg(OF::FormatDin(b->reserved),OF::FormatDin(b->unreserved)) : "The node did not report a complete balance.");
    }
}

void OrchardWidget::showStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(PayCollectPolicy::pillStyle(error ? Tone::Bad : Tone::Good));
    status_->setVisible(!text.isEmpty());
}
