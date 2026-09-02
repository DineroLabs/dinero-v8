#include "shieldedwidget.h"
#include "rpcclient.h"
#include "shieldedtransferpolicy.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QColor>
#include <QHeaderView>
#include <QJsonArray>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollArea>
#include <QSpacerItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kDefaultFeeUna = 20000;  // 0.0002 DIN — clears 1 una/byte for typical bundles

QString MoneyDinFromUna(qint64 una) {
    return QString::number(static_cast<double>(una) / 1e8, 'f', 8) + " DIN";
}

void appendLog(QTextEdit* log, const QString& line) {
    if (!log) return;
    log->append(line);
}
}  // namespace

ShieldedWidget::ShieldedWidget(RpcClient* rpc, QWidget* parent)
    : QWidget(parent), rpc_(rpc) {
    setupUI();
    // Apply the lockout immediately. setActiveBanner is otherwise only reached
    // after a wallet.shieldedbalance reply, which would leave the action
    // buttons live during startup — and permanently so if the daemon never
    // answers.
    setActiveBanner(false);
    loadAddressBook();
    if (rpc_) {
        connect(rpc_, &RpcClient::rpcResult, this, &ShieldedWidget::onRpcResult);
        connect(rpc_, &RpcClient::rpcError,  this, &ShieldedWidget::onRpcError);
    }
    // Auto-refresh on new tip — polls getblockcount every 5s; only
    // fires `refresh()` when tip advances. Cheap; no daemon-side
    // subscription mechanism for shielded yet.
    tipPollTimer_ = new QTimer(this);
    tipPollTimer_->setInterval(5000);
    connect(tipPollTimer_, &QTimer::timeout, this, &ShieldedWidget::onTipPoll);
    tipPollTimer_->start();
    refresh();
}

void ShieldedWidget::setupUI() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // ── Status banner ──
    statusBanner_ = new QLabel("Checking shielded pool status…");
    statusBanner_->setStyleSheet(
        "QLabel { padding: 8px 12px; border-radius: 6px; background: #2f343c; "
        "color: #cfd3d8; font-weight: 600; }");
    root->addWidget(statusBanner_);

    // ── Balance + Receive (side-by-side) ──
    auto* topRow = new QHBoxLayout;

    auto* balanceBox = new QGroupBox("Shielded Balance");
    {
        auto* g = new QGridLayout(balanceBox);
        g->addWidget(new QLabel("Balance:"),     0, 0);
        balanceDinLabel_ = new QLabel("—");
        balanceDinLabel_->setStyleSheet("font-size: 18px; font-weight: 700;");
        g->addWidget(balanceDinLabel_, 0, 1);
        g->addWidget(new QLabel("Balance (una):"), 1, 0);
        balanceUnaLabel_ = new QLabel("—");
        g->addWidget(balanceUnaLabel_, 1, 1);
        g->addWidget(new QLabel("Confirmed notes:"), 2, 0);
        noteCountLabel_ = new QLabel("—");
        g->addWidget(noteCountLabel_, 2, 1);
        g->addWidget(new QLabel("Pending notes:"), 3, 0);
        pendingNoteCountLabel_ = new QLabel("—");
        g->addWidget(pendingNoteCountLabel_, 3, 1);
        g->addWidget(new QLabel("Tree size:"),   4, 0);
        treeSizeLabel_ = new QLabel("—");
        g->addWidget(treeSizeLabel_, 4, 1);

        auto* refreshBtn = new QPushButton("Refresh");
        connect(refreshBtn, &QPushButton::clicked, this, &ShieldedWidget::refresh);
        g->addWidget(refreshBtn, 5, 0, 1, 2);
    }
    topRow->addWidget(balanceBox, /*stretch=*/1);

    auto* receiveBox = new QGroupBox("Receive Address");
    {
        auto* v = new QVBoxLayout(receiveBox);
        addressLabel_ = new QLabel("—");
        addressLabel_->setWordWrap(true);
        addressLabel_->setStyleSheet(
            "QLabel { font-family: 'Menlo', monospace; font-size: 11px; "
            "padding: 6px; background: #1a1d22; border-radius: 4px; "
            "color: #98c379; selection-background-color: #4a5566; }");
        addressLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(addressLabel_);

        auto* btnRow = new QHBoxLayout;
        copyAddressBtn_ = new QPushButton("Copy");
        connect(copyAddressBtn_, &QPushButton::clicked, this, &ShieldedWidget::onCopyAddressClicked);
        btnRow->addWidget(copyAddressBtn_);
        newAddressBtn_ = new QPushButton("New (j+1)");
        connect(newAddressBtn_, &QPushButton::clicked, this, &ShieldedWidget::onNewAddressClicked);
        btnRow->addWidget(newAddressBtn_);
        btnRow->addStretch();
        v->addLayout(btnRow);

        // Address book — recall any j we've issued in this session.
        v->addWidget(new QLabel("Issued addresses:"));
        addressBookCombo_ = new QComboBox;
        addressBookCombo_->setMinimumContentsLength(40);
        connect(addressBookCombo_,
                static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
                this, &ShieldedWidget::onAddressBookActivated);
        v->addWidget(addressBookCombo_);
        v->addStretch();
    }
    topRow->addWidget(receiveBox, /*stretch=*/2);

    root->addLayout(topRow);

    // ── Shield ──
    auto* shieldBox = new QGroupBox("Shield (transparent → shielded)");
    {
        auto* h = new QHBoxLayout(shieldBox);
        h->addWidget(new QLabel("Amount (DIN):"));
        shieldAmountEdit_ = new QLineEdit;
        shieldAmountEdit_->setPlaceholderText("1.0");
        h->addWidget(shieldAmountEdit_);
        h->addWidget(new QLabel("Fee (una):"));
        shieldFeeEdit_ = new QLineEdit(QString::number(kDefaultFeeUna));
        shieldFeeEdit_->setMaximumWidth(120);
        h->addWidget(shieldFeeEdit_);
        shieldBtn_ = new QPushButton("Shield");
        connect(shieldBtn_, &QPushButton::clicked, this, &ShieldedWidget::onShieldClicked);
        h->addWidget(shieldBtn_);
        shieldResultLabel_ = new QLabel("");
        shieldResultLabel_->setStyleSheet("color: #888;");
        h->addWidget(shieldResultLabel_, /*stretch=*/2);
    }
    root->addWidget(shieldBox);

    // ── Transfer (any-recipient) ──
    // Title/placeholder are made network-aware in applyActiveHrp() once the
    // receive address loads; the generic title here is the pre-load fallback.
    transferBox_ = new QGroupBox("Send shielded");
    QGroupBox* transferBox = transferBox_;
    {
        auto* g = new QGridLayout(transferBox);
        g->addWidget(new QLabel("Recipient:"), 0, 0);
        transferAddressEdit_ = new QLineEdit;
        transferAddressEdit_->setPlaceholderText("shielded address");
        g->addWidget(transferAddressEdit_, 0, 1, 1, 4);

        g->addWidget(new QLabel("Amount (DIN):"), 1, 0);
        transferAmountDinEdit_ = new QLineEdit;
        transferAmountDinEdit_->setPlaceholderText("0.7");
        connect(transferAmountDinEdit_, &QLineEdit::textEdited,
                this, &ShieldedWidget::onAmountDinChanged);
        g->addWidget(transferAmountDinEdit_, 1, 1);

        g->addWidget(new QLabel("Amount (una):"), 1, 2);
        transferAmountUnaEdit_ = new QLineEdit;
        transferAmountUnaEdit_->setPlaceholderText("70000000");
        connect(transferAmountUnaEdit_, &QLineEdit::textEdited,
                this, &ShieldedWidget::onAmountUnaChanged);
        g->addWidget(transferAmountUnaEdit_, 1, 3);

        g->addWidget(new QLabel("Fee (una):"), 2, 0);
        transferFeeEdit_ = new QLineEdit;
        transferFeeEdit_->setPlaceholderText("Auto-sized");
        transferFeeEdit_->setMaximumWidth(120);
        g->addWidget(transferFeeEdit_, 2, 1);

        g->addWidget(new QLabel("Memo (≤512B):"), 2, 2);
        transferMemoEdit_ = new QLineEdit;
        transferMemoEdit_->setPlaceholderText("optional UTF-8 memo");
        transferMemoEdit_->setMaxLength(512);
        g->addWidget(transferMemoEdit_, 2, 3);

        transferBtn_ = new QPushButton("Send");
        connect(transferBtn_, &QPushButton::clicked, this, &ShieldedWidget::onTransferClicked);
        g->addWidget(transferBtn_, 2, 4);

        transferResultLabel_ = new QLabel("");
        transferResultLabel_->setStyleSheet("color: #888;");
        transferResultLabel_->setWordWrap(true);
        g->addWidget(transferResultLabel_, 3, 0, 1, 5);
    }
    root->addWidget(transferBox);

    // ── Unshield ──
    auto* unshieldBox = new QGroupBox("Unshield note (shielded → transparent)");
    {
        auto* v = new QVBoxLayout(unshieldBox);
        auto* h = new QHBoxLayout;
        h->addWidget(new QLabel("Minimum note (DIN):"));
        unshieldAmountEdit_ = new QLineEdit;
        unshieldAmountEdit_->setPlaceholderText("0.5");
        unshieldAmountEdit_->setToolTip(
            "The daemon selects the smallest confirmed shielded note at least this large. "
            "The full selected note minus fee is sent to a fresh wallet Taproot address.");
        h->addWidget(unshieldAmountEdit_);
        h->addWidget(new QLabel("Fee (una):"));
        unshieldFeeEdit_ = new QLineEdit(QString::number(kDefaultFeeUna));
        unshieldFeeEdit_->setMaximumWidth(120);
        h->addWidget(unshieldFeeEdit_);
        unshieldBtn_ = new QPushButton("Unshield");
        connect(unshieldBtn_, &QPushButton::clicked, this, &ShieldedWidget::onUnshieldClicked);
        h->addWidget(unshieldBtn_);
        unshieldResultLabel_ = new QLabel("");
        unshieldResultLabel_->setStyleSheet("color: #888;");
        h->addWidget(unshieldResultLabel_, /*stretch=*/2);
        v->addLayout(h);
        unshieldAddressLabel_ = new QLabel("To: fresh wallet Taproot address generated by daemon at submit time");
        unshieldAddressLabel_->setStyleSheet("color: #8f98a3;");
        unshieldAddressLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        unshieldAddressLabel_->setWordWrap(true);
        v->addWidget(unshieldAddressLabel_);
    }
    root->addWidget(unshieldBox);

    // ── Notes table ──
    auto* notesBox = new QGroupBox("Shielded Notes");
    {
        auto* v = new QVBoxLayout(notesBox);
        notesTable_ = new QTableWidget(0, 4);
        notesTable_->setHorizontalHeaderLabels({"leaf_index", "value (DIN)", "state", "commitment"});
        notesTable_->horizontalHeader()->setStretchLastSection(true);
        notesTable_->verticalHeader()->setVisible(false);
        notesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        notesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        notesTable_->setAlternatingRowColors(true);
        notesTable_->setStyleSheet(
            "QTableWidget { font-family: 'Menlo', monospace; font-size: 11px; "
            "background: #1a1d22; alternate-background-color: #20242b; "
            "color: #cfd3d8; gridline-color: #2f343c; }"
            "QHeaderView::section { background: #2f343c; padding: 4px; "
            "border: 0; color: #cfd3d8; font-weight: 600; }");
        notesTable_->setMaximumHeight(220);
        v->addWidget(notesTable_);
    }
    root->addWidget(notesBox);

    // ── Activity log ──
    auto* logBox = new QGroupBox("Activity");
    {
        auto* v = new QVBoxLayout(logBox);
        activityLog_ = new QTextEdit;
        activityLog_->setReadOnly(true);
        activityLog_->setStyleSheet(
            "QTextEdit { font-family: 'Menlo', monospace; font-size: 11px; "
            "background: #1a1d22; color: #cfd3d8; }");
        activityLog_->setMaximumHeight(140);
        v->addWidget(activityLog_);
    }
    root->addWidget(logBox);

    root->addStretch();
}

// Lockout constant lives in shieldedwidget.h — shared with mainwindow.cpp.
void ShieldedWidget::setActiveBanner(bool active, const QString& reason) {
    const bool activationLockApplies = kShieldedUiLockedOut && hrpFromAddress(currentAddress_) != "rdins";
    if (activationLockApplies) {
        shieldedActive_ = false;
        statusBanner_->setText(
            "🔒 Shielded transfers are temporarily unavailable  —  "
            "this feature is being finished and is held closed until its "
            "activation height is set. Receive addresses still derive locally; "
            "shield, unshield and private send are disabled.");
        statusBanner_->setStyleSheet(
            "QLabel { padding: 8px 12px; border-radius: 6px; background: #3a3f4a; "
            "color: #d5d9e0; font-weight: 600; }");
        if (shieldBtn_)   shieldBtn_->setEnabled(false);
        if (transferBtn_) transferBtn_->setEnabled(false);
        if (unshieldBtn_) unshieldBtn_->setEnabled(false);
        return;
    }
    shieldedActive_ = active;
    if (active) {
        statusBanner_->setText("✅ Shielded pool ACTIVE on this network");
        statusBanner_->setStyleSheet(
            "QLabel { padding: 8px 12px; border-radius: 6px; background: #2d4a32; "
            "color: #b8e0bf; font-weight: 600; }");
    } else {
        QString msg = "⚠️ Shielded pool not yet active on this network";
        if (!reason.isEmpty()) msg += "  —  " + reason;
        msg += "  —  receive addresses still derive locally; shield/unshield/send await activation.";
        statusBanner_->setText(msg);
        statusBanner_->setStyleSheet(
            "QLabel { padding: 8px 12px; border-radius: 6px; background: #4a402d; "
            "color: #e0d2b8; font-weight: 600; }");
    }
    // Disable on-chain actions when parked.
    if (shieldBtn_)    shieldBtn_->setEnabled(active);
    if (transferBtn_)  transferBtn_->setEnabled(active && !transferSubmitting_);
    if (unshieldBtn_)  unshieldBtn_->setEnabled(active);
}

void ShieldedWidget::refresh() {
    if (!rpc_) return;
    rpc_->call("wallet.shieldedbalance", {});
    rpc_->callNamed("wallet.getshieldedaddress",
                    QJsonObject{{"account", 0}, {"j", static_cast<qint64>(currentJ_)}});
    rpc_->call("wallet.listshielded", {});
}

void ShieldedWidget::setWalletScope(const QString& walletName) {
    const QString nextScope = walletName.trimmed();
    if (walletScope_ == nextScope) return;

    walletScope_ = nextScope;
    transferSubmitting_ = false;
    transferJournalStage_.clear();
    addressBook_.clear();
    currentJ_ = 0;
    currentAddress_.clear();

    if (addressBookCombo_) {
        addressBookCombo_->clear();
    }
    if (addressLabel_) {
        addressLabel_->setText(walletScope_.isEmpty() ? "(no wallet loaded)" : "—");
    }
    if (balanceDinLabel_) balanceDinLabel_->setText("—");
    if (balanceUnaLabel_) balanceUnaLabel_->setText("—");
    if (noteCountLabel_) noteCountLabel_->setText("—");
    if (pendingNoteCountLabel_) pendingNoteCountLabel_->setText("—");
    if (treeSizeLabel_) treeSizeLabel_->setText("—");
    if (notesTable_) notesTable_->setRowCount(0);
    if (shieldResultLabel_) shieldResultLabel_->clear();
    if (transferResultLabel_) transferResultLabel_->clear();
    if (transferBtn_) transferBtn_->setEnabled(shieldedActive_);
    if (unshieldResultLabel_) unshieldResultLabel_->clear();
    if (unshieldAddressLabel_) {
        unshieldAddressLabel_->setText("To: fresh wallet Taproot address generated by daemon at submit time");
    }

    loadAddressBook();
    loadTransferJournal();
}

void ShieldedWidget::onTipPoll() {
    if (!rpc_) return;
    // Cheap probe — getblockcount return triggers the rpcResult path
    // below where we compare against lastSeenTip_ and call refresh().
    rpc_->call("getblockcount", {});
}

void ShieldedWidget::onAddressBookActivated(int idx) {
    if (idx < 0 || idx >= addressBook_.size()) return;
    const auto& entry = addressBook_[idx];
    currentJ_ = entry.j;
    currentAddress_ = entry.address;
    addressLabel_->setText(QString("[account=0  j=%1]\n%2").arg(entry.j).arg(entry.address));
    applyActiveHrp();
}

void ShieldedWidget::onAmountDinChanged(const QString& text) {
    if (amountUpdating_) return;
    amountUpdating_ = true;
    bool ok = false;
    const double din = text.toDouble(&ok);
    if (ok && din >= 0) {
        const qint64 una = static_cast<qint64>(std::llround(din * 1e8));
        transferAmountUnaEdit_->setText(QString::number(una));
    } else if (text.isEmpty()) {
        transferAmountUnaEdit_->clear();
    }
    amountUpdating_ = false;
}

void ShieldedWidget::onAmountUnaChanged(const QString& text) {
    if (amountUpdating_) return;
    amountUpdating_ = true;
    bool ok = false;
    const qint64 una = text.toLongLong(&ok);
    if (ok && una >= 0) {
        transferAmountDinEdit_->setText(QString::number(static_cast<double>(una) / 1e8, 'f', 8));
    } else if (text.isEmpty()) {
        transferAmountDinEdit_->clear();
    }
    amountUpdating_ = false;
}

void ShieldedWidget::recordIssuedAddress(uint64_t j, const QString& addr) {
    // Dedupe by (hrp, j) so mainnet/testnet/regtest entries don't clash.
    const QString hrp = hrpFromAddress(addr);
    for (auto& e : addressBook_) {
        if (e.j == j && hrpFromAddress(e.address) == hrp) {
            e.address = addr;
            e.issuedAt = QDateTime::currentDateTime();
            saveAddressBook();
            return;
        }
    }
    addressBook_.append({j, addr, QDateTime::currentDateTime()});
    if (addressBookCombo_) {
        addressBookCombo_->addItem(QString("j=%1  %2  %3 [%4]")
                                       .arg(j)
                                       .arg(addr.left(16) + "…")
                                       .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                       .arg(hrp));
    }
    saveAddressBook();
}

QString ShieldedWidget::hrpFromAddress(const QString& addr) {
    if (addr.startsWith("rdins1")) return "rdins";
    if (addr.startsWith("tdins1")) return "tdins";
    if (addr.startsWith("dins1"))  return "dins";
    return "unknown";
}

void ShieldedWidget::applyActiveHrp() {
    const QString hrp = hrpFromAddress(currentAddress_);
    if (hrp == "unknown") return;  // address not loaded yet — keep generic labels
    if (transferBox_) {
        transferBox_->setTitle(QString("Send shielded (to %1 address)").arg(hrp));
    }
    if (transferAddressEdit_) {
        transferAddressEdit_->setPlaceholderText(hrp + QStringLiteral("1…"));
    }
}

void ShieldedWidget::loadAddressBook() {
    QSettings s;
    s.beginGroup("shielded/addressBook");
    s.beginGroup(settingsWalletScope());
    const QStringList groups = s.childGroups();
    uint64_t maxJ = 0;
    for (const QString& hrp : groups) {
        s.beginGroup(hrp);
        const int n = s.beginReadArray("entries");
        for (int i = 0; i < n; ++i) {
            s.setArrayIndex(i);
            AddressBookEntry e;
            e.j        = s.value("j").toULongLong();
            e.address  = s.value("address").toString();
            e.issuedAt = s.value("issuedAt").toDateTime();
            if (e.address.isEmpty()) continue;
            addressBook_.append(e);
            maxJ = std::max(maxJ, e.j);
            if (addressBookCombo_) {
                addressBookCombo_->addItem(
                    QString("j=%1  %2  %3 [%4]")
                        .arg(e.j)
                        .arg(e.address.left(16) + "…")
                        .arg(e.issuedAt.toString("yyyy-MM-dd HH:mm"))
                        .arg(hrp));
            }
        }
        s.endArray();
        s.endGroup();
    }
    currentJ_ = maxJ;
    s.endGroup();
    s.endGroup();
}

void ShieldedWidget::saveAddressBook() const {
    // Group by HRP and overwrite the addressBook subtree atomically.
    QMap<QString, QList<AddressBookEntry>> byHrp;
    for (const AddressBookEntry& e : addressBook_) {
        byHrp[hrpFromAddress(e.address)].append(e);
    }
    QSettings s;
    s.beginGroup("shielded/addressBook");
    s.beginGroup(settingsWalletScope());
    s.remove(QString());  // clear all sub-groups for a clean overwrite
    for (auto it = byHrp.begin(); it != byHrp.end(); ++it) {
        s.beginGroup(it.key());
        s.beginWriteArray("entries", it.value().size());
        for (int i = 0; i < it.value().size(); ++i) {
            s.setArrayIndex(i);
            const auto& e = it.value()[i];
            s.setValue("j",        static_cast<qulonglong>(e.j));
            s.setValue("address",  e.address);
            s.setValue("issuedAt", e.issuedAt);
        }
        s.endArray();
        s.endGroup();
    }
    s.endGroup();
    s.endGroup();
    s.sync();
}

QString ShieldedWidget::settingsWalletScope() const {
    QString scope = walletScope_.trimmed();
    if (scope.isEmpty()) return "no-wallet";
    scope.replace("/", "_");
    scope.replace("\\", "_");
    return scope;
}

void ShieldedWidget::updateNotesTable(const QJsonValue& result) {
    if (!notesTable_) return;
    if (!result.isObject()) {
        notesTable_->setRowCount(0);
        return;
    }
    const auto obj = result.toObject();
    const auto notes = obj.value("notes").toArray();
    notesTable_->setRowCount(notes.size());
    int row = 0;
    for (const auto& v : notes) {
        const auto n = v.toObject();
        const bool confirmed = n.value("confirmed").toBool();
        const bool spent     = n.value("spent").toBool();
        const qint64 leaf    = n.value("leaf_index").toVariant().toLongLong();
        const qint64 una     = n.value("value_una").toVariant().toLongLong();
        const QString cm     = n.value("commitment_hex").toString();
        QString state;
        if (spent) state = "spent";
        else if (confirmed) state = "confirmed";
        else state = "pending";

        notesTable_->setItem(row, 0,
            new QTableWidgetItem(confirmed ? QString::number(leaf) : QString("—")));
        notesTable_->setItem(row, 1, new QTableWidgetItem(MoneyDinFromUna(una)));
        auto* stateItem = new QTableWidgetItem(state);
        if (state == "spent")     stateItem->setForeground(QColor("#888"));
        else if (state == "pending") stateItem->setForeground(QColor("#e0d2b8"));
        else                      stateItem->setForeground(QColor("#b8e0bf"));
        notesTable_->setItem(row, 2, stateItem);
        notesTable_->setItem(row, 3, new QTableWidgetItem(cm));
        ++row;
    }
}

void ShieldedWidget::onNewAddressClicked() {
    if (!rpc_) return;
    ++currentJ_;
    rpc_->callNamed("wallet.getshieldedaddress",
                    QJsonObject{{"account", 0}, {"j", static_cast<qint64>(currentJ_)}});
}

void ShieldedWidget::onCopyAddressClicked() {
    if (currentAddress_.isEmpty()) return;
    QApplication::clipboard()->setText(currentAddress_);
    appendLog(activityLog_, "[copy] " + currentAddress_);
}

void ShieldedWidget::onShieldClicked() {
    // Defence in depth for the lockout above. Disabling the buttons is what a
    // user hits; this is what a future refactor hits if it wires another
    // trigger to this slot.
    if (kShieldedUiLockedOut && hrpFromAddress(currentAddress_) != "rdins") return;
    if (!rpc_ || !shieldedActive_) return;
    bool ok = false;
    double amount = shieldAmountEdit_->text().toDouble(&ok);
    if (!ok || amount <= 0) {
        shieldResultLabel_->setText("invalid amount");
        return;
    }
    qint64 fee = shieldFeeEdit_->text().toLongLong(&ok);
    if (!ok || fee <= 0) fee = kDefaultFeeUna;
    rpc_->call("wallet.shield", QJsonArray{amount, fee});
    shieldResultLabel_->setText("submitting…");
}

void ShieldedWidget::onTransferClicked() {
    // Defence in depth for the lockout above. Disabling the buttons is what a
    // user hits; this is what a future refactor hits if it wires another
    // trigger to this slot.
    if (kShieldedUiLockedOut && hrpFromAddress(currentAddress_) != "rdins") return;
    if (transferJournalStage_ == "accepted") {
        clearTransferJournal();
        transferAddressEdit_->clear();
        transferAmountUnaEdit_->clear();
        transferAmountDinEdit_->clear();
        transferMemoEdit_->clear();
        transferResultLabel_->setText("ready for a new private payment");
        transferBtn_->setText("Send");
        return;
    }
    if (!rpc_ || !shieldedActive_ ||
        !ShieldedTransferPolicy::maySubmit(transferJournalStage_, transferSubmitting_)) return;
    const QString addr = transferAddressEdit_->text().trimmed();
    if (addr.isEmpty()) {
        transferResultLabel_->setText("enter recipient address");
        return;
    }
    const QString expectedHrp = hrpFromAddress(currentAddress_);
    if (expectedHrp == "unknown" || hrpFromAddress(addr) != expectedHrp) {
        transferResultLabel_->setText("recipient is not a valid address for the active network");
        return;
    }
    bool ok = false;
    qint64 amountUna = transferAmountUnaEdit_->text().toLongLong(&ok);
    if (!ok || amountUna <= 0) {
        transferResultLabel_->setText("invalid amount_una");
        return;
    }
    qint64 fee = 0;
    if (!transferFeeEdit_->text().trimmed().isEmpty()) {
        fee = transferFeeEdit_->text().toLongLong(&ok);
        if (!ok || fee <= 0) {
            transferResultLabel_->setText("invalid fee");
            return;
        }
    }
    const QString memo = transferMemoEdit_->text();
    const QString feeReview = fee > 0 ? QString::number(fee) + " una" : "Auto-sized by local prover";
    const auto answer = QMessageBox::question(
        this, "Review Private Payment",
        QString("Recipient:\n%1\n\nAmount: %2\nNetwork fee: %3\n\n"
                "The wallet may select multiple shielded notes. Proving and submission "
                "are one operation and cannot be undone.")
            .arg(addr, MoneyDinFromUna(amountUna), feeReview),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    if (!saveTransferJournal("authorized", addr, amountUna, memo)) {
        transferResultLabel_->setText("could not persist authorization; nothing submitted");
        return;
    }
    if (!saveTransferJournal("submitting", addr, amountUna, memo)) {
        transferResultLabel_->setText("could not persist submission state; nothing submitted");
        return;
    }
    QJsonObject p{
        {"amount_una", amountUna},
        {"address",    addr},
    };
    if (fee > 0) p.insert("fee_una", fee);
    if (!memo.isEmpty()) p.insert("memo", memo);
    setTransferSubmitting(true);
    rpc_->callNamed("wallet.transfer", p);
    transferResultLabel_->setText("proving and submitting…");
}

void ShieldedWidget::onUnshieldClicked() {
    // Defence in depth for the lockout above. Disabling the buttons is what a
    // user hits; this is what a future refactor hits if it wires another
    // trigger to this slot.
    if (kShieldedUiLockedOut && hrpFromAddress(currentAddress_) != "rdins") return;
    if (!rpc_ || !shieldedActive_) return;
    bool ok = false;
    double amount = unshieldAmountEdit_->text().toDouble(&ok);
    if (!ok || amount <= 0) {
        unshieldResultLabel_->setText("invalid amount");
        return;
    }
    qint64 fee = unshieldFeeEdit_->text().toLongLong(&ok);
    if (!ok || fee <= 0) fee = kDefaultFeeUna;
    rpc_->call("wallet.unshield", QJsonArray{amount, fee});
    unshieldResultLabel_->setText("submitting…");
}

void ShieldedWidget::updateBalanceLabels(const QJsonValue& result) {
    if (!result.isObject()) return;
    const QJsonObject obj = result.toObject();
    qint64 una = obj.value("balance_una").toVariant().toLongLong();
    qint64 tree = obj.value("tree_size").toVariant().toLongLong();
    qint64 confirmed = obj.value("note_count").toVariant().toLongLong();
    qint64 pending = obj.value("pending_note_count").toVariant().toLongLong();
    balanceUnaLabel_->setText(QString::number(una));
    balanceDinLabel_->setText(MoneyDinFromUna(una));
    treeSizeLabel_->setText(QString::number(tree));
    noteCountLabel_->setText(QString::number(confirmed));
    if (pendingNoteCountLabel_) {
        pendingNoteCountLabel_->setText(QString::number(pending));
    }
}

void ShieldedWidget::updateReceiveAddress(const QJsonValue& result) {
    if (!result.isObject()) return;
    const QJsonObject obj = result.toObject();
    currentAddress_ = obj.value("address").toString();
    const qint64 j = obj.value("j").toVariant().toLongLong();
    addressLabel_->setText(
        currentAddress_.isEmpty()
            ? QString("(unavailable)")
            : QString("[account=0  j=%1]\n%2").arg(j).arg(currentAddress_));
    if (!currentAddress_.isEmpty()) {
        recordIssuedAddress(static_cast<uint64_t>(j), currentAddress_);
    }
    applyActiveHrp();
    if (hrpFromAddress(currentAddress_) == "rdins") {
        setActiveBanner(true);
    }
}

void ShieldedWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method == "getblockcount") {
        // Auto-refresh path: refresh() if tip advanced.
        const qint64 tip = result.toVariant().toLongLong();
        if (lastSeenTip_ < 0) {
            lastSeenTip_ = tip;
        } else if (tip > lastSeenTip_) {
            lastSeenTip_ = tip;
            refresh();
        }
        return;
    }

    // Some daemon RPCs wrap server-side errors in `result.error` instead of
    // a JSON-RPC error envelope. Detect both shapes.
    QString innerError;
    if (result.isObject()) {
        const auto obj = result.toObject();
        if (obj.contains("error") && !obj.value("error").isNull()) {
            innerError = obj.value("error").toString();
        }
    }

    if (method == "wallet.listshielded") {
        if (innerError.isEmpty()) updateNotesTable(result);
        return;
    }

    if (method == "wallet.shieldedbalance") {
        if (innerError == "shielded_not_active") {
            setActiveBanner(false, "chainparams.shielded_activation_height = UINT32_MAX");
            return;
        }
        setActiveBanner(true);
        if (innerError.isEmpty()) updateBalanceLabels(result);
        return;
    }
    if (method == "wallet.getshieldedaddress") {
        if (innerError.isEmpty()) updateReceiveAddress(result);
        else appendLog(activityLog_, "[error] getshieldedaddress: " + innerError);
        return;
    }
    if (method == "wallet.shield") {
        if (!innerError.isEmpty()) {
            shieldResultLabel_->setText("rejected: " + innerError);
            appendLog(activityLog_, "[shield] rejected: " + innerError);
        } else {
            QString txid = result.toObject().value("txid").toString();
            shieldResultLabel_->setText("ok — txid " + txid.left(16) + "…");
            appendLog(activityLog_, "[shield] " + txid);
            refresh();
        }
        return;
    }
    if (method == "wallet.transfer") {
        setTransferSubmitting(false);
        if (!innerError.isEmpty()) {
            const QSettings s;
            const QString base = "shielded/transferJournal/" + settingsWalletScope();
            saveTransferJournal("rejected", s.value(base + "/address").toString(),
                                s.value(base + "/amountUna").toLongLong(),
                                s.value(base + "/memo").toString());
            transferResultLabel_->setText("rejected: " + innerError);
            appendLog(activityLog_, "[transfer] rejected: " + innerError);
        } else {
            const auto obj = result.toObject();
            const QString txid = obj.value("txid").toString();
            const QString wave = obj.value("wave").toString();
            const qint64 fee = obj.value("fee_una").toVariant().toLongLong();
            const QSettings s;
            const QString base = "shielded/transferJournal/" + settingsWalletScope();
            saveTransferJournal("accepted", s.value(base + "/address").toString(),
                                s.value(base + "/amountUna").toLongLong(),
                                s.value(base + "/memo").toString(), txid, fee);
            transferResultLabel_->setText(QString("ok (wave=%1) — txid %2…")
                                             .arg(wave).arg(txid.left(16)));
            appendLog(activityLog_, QString("[transfer/%1] %2").arg(wave).arg(txid));
            refresh();
        }
        return;
    }
    if (method == "wallet.unshield") {
        if (!innerError.isEmpty()) {
            unshieldResultLabel_->setText("rejected: " + innerError);
            appendLog(activityLog_, "[unshield] rejected: " + innerError);
        } else {
            const auto obj = result.toObject();
            const QString txid = obj.value("txid").toString();
            const QString recipient = obj.value("recipient_address").toString();
            const qint64 recipientUna = obj.value("recipient_una").toVariant().toLongLong();
            const QString amountText = MoneyDinFromUna(recipientUna);
            unshieldResultLabel_->setText("ok - " + amountText + " - txid " + txid.left(16) + "...");
            if (unshieldAddressLabel_) {
                unshieldAddressLabel_->setText("To: " + recipient);
            }
            appendLog(activityLog_, "[unshield] " + amountText + " to " + recipient + " txid=" + txid);
            refresh();
        }
        return;
    }
}

void ShieldedWidget::onRpcError(const QString& method, int code, const QString& message) {
    if (method == "wallet.shieldedbalance" && message.contains("shielded_not_active",
                                                              Qt::CaseInsensitive)) {
        setActiveBanner(false, "daemon: " + message);
        return;
    }
    if (method.startsWith("wallet.")) {
        appendLog(activityLog_,
                  QString("[rpc-error] %1: code=%2 %3").arg(method).arg(code).arg(message));
    }
    if (method == "wallet.transfer") {
        // Transport-level errors are ambiguous: the daemon may have accepted
        // the transaction before the response was lost. Keep `submitting`
        // durable and refuse an automatic or click-driven retry.
        setTransferSubmitting(true);
        transferResultLabel_->setText(
            "outcome uncertain — refresh notes and transaction history before retrying");
    }
}

void ShieldedWidget::setTransferSubmitting(bool submitting) {
    transferSubmitting_ = submitting;
    if (transferBtn_) {
        transferBtn_->setEnabled(shieldedActive_ && !submitting);
        if (submitting) transferBtn_->setText("Proving and submitting…");
    }
}

bool ShieldedWidget::saveTransferJournal(const QString& stage, const QString& address,
                                         qint64 amountUna, const QString& memo,
                                         const QString& txid, qint64 feeUna) {
    QSettings s;
    const QString base = "shielded/transferJournal/" + settingsWalletScope();
    s.setValue(base + "/stage", stage);
    s.setValue(base + "/address", address);
    s.setValue(base + "/amountUna", amountUna);
    s.setValue(base + "/memo", memo);
    s.setValue(base + "/txid", txid);
    s.setValue(base + "/feeUna", feeUna);
    s.setValue(base + "/updatedAt", QDateTime::currentDateTimeUtc());
    s.sync();
    transferJournalStage_ = stage;
    if (transferBtn_) {
        if (stage == "rejected") transferBtn_->setText("Review and Retry");
        else if (stage == "accepted") transferBtn_->setText("New Private Payment");
    }
    return s.status() == QSettings::NoError && s.value(base + "/stage").toString() == stage;
}

void ShieldedWidget::clearTransferJournal() {
    QSettings s;
    s.remove("shielded/transferJournal/" + settingsWalletScope());
    s.sync();
    transferJournalStage_.clear();
}

void ShieldedWidget::loadTransferJournal() {
    QSettings s;
    const QString base = "shielded/transferJournal/" + settingsWalletScope();
    transferJournalStage_ = s.value(base + "/stage").toString();
    if (transferJournalStage_.isEmpty()) return;
    transferAddressEdit_->setText(s.value(base + "/address").toString());
    transferAmountUnaEdit_->setText(QString::number(s.value(base + "/amountUna").toLongLong()));
    transferMemoEdit_->setText(s.value(base + "/memo").toString());
    if (transferJournalStage_ == "submitting") {
        setTransferSubmitting(true);
        transferResultLabel_->setText(
            "previous private-payment outcome is uncertain; no retry will be started automatically");
    } else if (transferJournalStage_ == "rejected") {
        transferBtn_->setText("Review and Retry");
        transferResultLabel_->setText("previous private payment was rejected; review before explicit retry");
    } else if (transferJournalStage_ == "accepted") {
        transferBtn_->setText("New Private Payment");
        transferResultLabel_->setText("accepted — txid " + s.value(base + "/txid").toString());
    }
}
