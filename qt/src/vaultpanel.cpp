// Copyright (c) 2026 Dinero Labs.

#include "vaultpanel.h"

#include "rpcclient.h"

#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocale>
#include <QPushButton>
#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QVBoxLayout>

#include <limits>

namespace {

constexpr qint64 kUnaPerDin = 100000000;

QString safeString(const QJsonValue& v) {
    if (v.isString()) {
        return v.toString();
    }
    if (v.isDouble()) {
        return QString::number(static_cast<qint64>(v.toDouble()));
    }
    return {};
}

qint64 safeInt(const QJsonValue& v) {
    if (v.isDouble()) {
        return static_cast<qint64>(v.toDouble());
    }
    if (v.isString()) {
        return v.toString().toLongLong();
    }
    return 0;
}

QString formatDinAmount(qint64 una) {
    QLocale locale(QLocale::English);
    const bool negative = una < 0;
    const qulonglong absolute = negative
        ? static_cast<qulonglong>(-(una + 1)) + 1ULL
        : static_cast<qulonglong>(una);
    const qulonglong whole = absolute / static_cast<qulonglong>(kUnaPerDin);
    const qulonglong fraction = absolute % static_cast<qulonglong>(kUnaPerDin);
    const QString wholeText = locale.toString(static_cast<qlonglong>(whole));
    const QString fractionText = QStringLiteral("%1").arg(fraction, 8, 10, QLatin1Char('0'));
    return QStringLiteral("%1%2.%3")
        .arg(negative ? QStringLiteral("-") : QString(),
             wholeText,
             fractionText);
}

}  // namespace

VaultPanel::VaultPanel(RpcClient* rpc, QWidget* parent)
    : QWidget(parent), rpc_(rpc) {
    setupUi();
    setupConnections();
    onRefresh();
    refresh_timer_.start(REFRESH_INTERVAL_MS);
}

VaultPanel::~VaultPanel() = default;

QString VaultPanel::developerSummary() const {
    return developer_summary_;
}

QString VaultPanel::formatAmountPlain(qint64 una) {
    QLocale locale(QLocale::English);
    return QStringLiteral("%1 DIN (%2 una)")
        .arg(formatDinAmount(una), locale.toString(una));
}

QString VaultPanel::formatAmountRich(qint64 una) {
    QLocale locale(QLocale::English);
    return QStringLiteral(
               "<span style='font-size:15px; font-weight:600; color:#e6edf3;'>%1 DIN</span>"
               "<br/><span style='color:#9fb3c8; font-size:11px;'>%2 una</span>")
        .arg(formatDinAmount(una).toHtmlEscaped(),
             locale.toString(una).toHtmlEscaped());
}

QString VaultPanel::formatMetricTitle(const QString& title, const QString& helper) {
    return QStringLiteral(
               "<span style='font-weight:600; color:#e6edf3;'>%1</span>"
               "<br/><span style='color:#9fb3c8; font-size:11px;'>%2</span>")
        .arg(title.toHtmlEscaped(), helper.toHtmlEscaped());
}

QString VaultPanel::accountKeyForWalletScope(const QString& walletName) {
    Q_UNUSED(walletName);
    // Shared NodeCore binds mobile wallets to this account. Keep the
    // identifier platform-neutral: wallet display names are local metadata
    // and must never split the same vault ledger across Qt and mobile.
    return QStringLiteral("wallet");
}

QString VaultPanel::journalKeyForWalletScope(const QString& walletName) {
    QString scope = walletName.trimmed();
    if (scope.isEmpty()) scope = QStringLiteral("wallet");
    scope.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.:-]+")), QStringLiteral("_"));
    return QStringLiteral("wallet:%1").arg(scope.left(96));
}

bool VaultPanel::parseDinAmount(const QString& text, qint64* unaOut) {
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,10})(?:\\.([0-9]{1,8}))?$"));
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch()) return false;
    bool ok = false;
    const qulonglong whole = match.captured(1).toULongLong(&ok);
    if (!ok) return false;
    QString fraction = match.captured(2);
    fraction = fraction.leftJustified(8, QLatin1Char('0'));
    const qulonglong frac = fraction.isEmpty() ? 0 : fraction.toULongLong(&ok);
    if (!ok || whole > static_cast<qulonglong>(std::numeric_limits<qint64>::max()) / kUnaPerDin) return false;
    const qulonglong total = whole * kUnaPerDin + frac;
    if (total == 0 || total > static_cast<qulonglong>(std::numeric_limits<qint64>::max())) return false;
    *unaOut = static_cast<qint64>(total);
    return true;
}

bool VaultPanel::isTaprootAddress(const QString& address) {
    const QString lower = address.trimmed().toLower();
    return (lower.startsWith(QStringLiteral("din1p")) ||
            lower.startsWith(QStringLiteral("tdin1p")) ||
            lower.startsWith(QStringLiteral("rdin1p"))) && lower.size() > 50;
}

QString VaultPanel::activeAccountKey() const {
    return accountKeyForWalletScope(wallet_scope_);
}

void VaultPanel::clearWalletScopedFields(const QString& operatorText) {
    account_spendable_una_ = 0;
    if (lbl_operator_address_) lbl_operator_address_->setText(operatorText);
    if (lbl_account_confirmed_) lbl_account_confirmed_->setText("—");
    if (lbl_account_pending_)   lbl_account_pending_->setText("—");
    if (lbl_account_locked_)    lbl_account_locked_->setText("—");
    if (lbl_account_spendable_) lbl_account_spendable_->setText("—");
    if (lbl_account_loss_)      lbl_account_loss_->setText("—");
    if (lbl_last_request_id_)   lbl_last_request_id_->setText("Last request: —");
    if (lbl_status_value_)      lbl_status_value_->setText("—");
    if (status_request_id_input_) status_request_id_input_->clear();
}

void VaultPanel::requestWalletPrimaryBinding() {
    if (wallet_scope_.trimmed().isEmpty() || waiting_for_wallet_primary_ || operator_bind_inflight_) {
        return;
    }
    operator_bind_attempted_ = true;
    waiting_for_wallet_primary_ = true;
    if (lbl_operator_address_) {
        lbl_operator_address_->setText(
            "<span style='color:#d8a37b;'>rebinding to active wallet…</span>");
    }
    callRpc("wallet.getinfo", QJsonObject{});
}

void VaultPanel::updateDeveloperSummary() {
    developer_summary_ = QStringLiteral("Vault raw metrics: ledger seq %1 · queue %2 · accounts %3")
        .arg(lbl_ledger_seq_->text(),
             lbl_queue_depth_->text(),
             lbl_account_count_->text());
    Q_EMIT developerSummaryChanged(developer_summary_);
}

void VaultPanel::setWalletScope(const QString& walletName) {
    const QString nextScope = walletName.trimmed();
    if (wallet_scope_ == nextScope) {
        return;
    }

    wallet_scope_ = nextScope;
    operator_bind_attempted_ = false;
    waiting_for_wallet_primary_ = false;
    operator_bind_inflight_ = false;

    if (wallet_scope_.isEmpty()) {
        clearWalletScopedFields("<span style='color:#d8a37b;'>no wallet loaded</span>");
        if (account_id_input_) account_id_input_->clear();
        if (withdraw_account_input_) withdraw_account_input_->clear();
        return;
    }

    const QString account = activeAccountKey();
    clearWalletScopedFields("<span style='color:#d8a37b;'>rebinding to active wallet…</span>");
    if (account_id_input_) account_id_input_->setText(account);
    if (withdraw_account_input_) withdraw_account_input_->setText(account);
    appendLog(QString("Wallet switched — vault scope is now %1").arg(wallet_scope_));
    withdrawal_submitting_ = false;
    withdrawal_waiting_wallet_status_ = false;
    withdrawal_journal_stage_.clear();
    loadWithdrawalJournal();
    refresh();
}

void VaultPanel::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);
    root->setContentsMargins(12, 12, 12, 12);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("<h2>\xF0\x9F\x8F\xA6 Liquidity Vault</h2>");
    auto* refresh_btn = new QPushButton("\xF0\x9F\x94\x84 Refresh");
    refresh_btn->setObjectName("vault_refresh_button");
    header->addWidget(title);
    header->addStretch();
    header->addWidget(refresh_btn);
    root->addLayout(header);

    auto* hint = new QLabel(
        "Custodial deposit / withdrawal ledger backed by the daemon's "
        "Track-C vault service. Distinct from on-chain script vaults "
        "(see Contracts tab). When the daemon starts with an active "
        "wallet and no vault address configured, it auto-binds to your "
        "wallet's primary address and tracks deposits there; "
        "credits open at K=10 confirmations, settle at K=20. This is a "
        "separate vault ledger—not your normal wallet balance. Only new "
        "payments received at the Vault Deposit Address after binding are "
        "credited; existing wallet funds are never imported automatically.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #9fb3c8; padding: 4px;");
    root->addWidget(hint);

    // Service-level metrics.
    service_group_ = new QGroupBox("Vault Summary");
    auto* svc_grid = new QGridLayout(service_group_);
    lbl_runtime_status_ = new QLabel("unknown");
    lbl_connection_status_ = new QLabel("connecting…");
    lbl_operator_address_ = new QLabel("\xE2\x80\x93");
    lbl_operator_address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl_operator_address_->setStyleSheet("QLabel { font-family: monospace; font-size: 11px; }");
    lbl_total_credits_ = new QLabel("\xE2\x80\x93");
    lbl_total_loss_ = new QLabel("\xE2\x80\x93");
    lbl_queue_depth_ = new QLabel("\xE2\x80\x93");
    lbl_ledger_seq_ = new QLabel("\xE2\x80\x93");
    lbl_account_count_ = new QLabel("\xE2\x80\x93");
    svc_grid->addWidget(new QLabel("Runtime:"), 0, 0);
    svc_grid->addWidget(lbl_runtime_status_, 0, 1);
    svc_grid->addWidget(new QLabel("Connection:"), 0, 2);
    svc_grid->addWidget(lbl_connection_status_, 0, 3);
    svc_grid->addWidget(new QLabel("Withdrawal queue:"), 1, 2);
    svc_grid->addWidget(lbl_queue_depth_, 1, 3);
    svc_grid->addWidget(new QLabel("Settling:"), 1, 0);
    svc_grid->addWidget(lbl_total_credits_, 1, 1);
    svc_grid->addWidget(new QLabel("Vault Deposit Address:"), 2, 0);
    svc_grid->addWidget(lbl_operator_address_, 2, 1, 1, 3);
    root->addWidget(service_group_);

    // Per-account inspector.
    account_group_ = new QGroupBox("Vault Balance");
    auto* acc_layout = new QVBoxLayout(account_group_);
    account_id_input_ = new QLineEdit();
    account_id_input_->setText(activeAccountKey());
    account_id_input_->hide();

    auto* acc_grid = new QGridLayout();
    lbl_account_confirmed_ = new QLabel("\xE2\x80\x93");
    lbl_account_pending_ = new QLabel("\xE2\x80\x93");
    lbl_account_locked_ = new QLabel("\xE2\x80\x93");
    lbl_account_spendable_ = new QLabel("\xE2\x80\x93");
    lbl_account_loss_ = new QLabel("\xE2\x80\x93");
    for (QLabel* label : {lbl_total_credits_, lbl_total_loss_, lbl_account_confirmed_,
                          lbl_account_pending_, lbl_account_locked_, lbl_account_spendable_,
                          lbl_account_loss_}) {
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    acc_grid->addWidget(new QLabel(formatMetricTitle(
        "Vault Balance", "Fully settled credits already inside this vault account.")), 0, 0);
    acc_grid->addWidget(lbl_account_confirmed_, 0, 1);
    acc_grid->addWidget(new QLabel(formatMetricTitle(
        "Reserved for Withdrawal", "Funds held aside for a withdrawal already in progress.")), 0, 2);
    acc_grid->addWidget(lbl_account_locked_, 0, 3);
    acc_grid->addWidget(new QLabel(formatMetricTitle(
        "Settling", "Deposits that opened credit and are waiting for full settlement.")), 1, 0);
    acc_grid->addWidget(lbl_account_pending_, 1, 1);
    acc_grid->addWidget(new QLabel(formatMetricTitle(
        "Available Now", "What this vault account can withdraw right now.")), 1, 2);
    acc_grid->addWidget(lbl_account_spendable_, 1, 3);
    acc_layout->addLayout(acc_grid);
    root->addWidget(account_group_);

    // Withdrawal form + status check.
    withdraw_group_ = new QGroupBox("Withdrawal");
    auto* wd_layout = new QVBoxLayout(withdraw_group_);
    auto* wd_form = new QFormLayout();
    withdraw_account_input_ = new QLineEdit(activeAccountKey());
    withdraw_account_input_->hide();
    withdraw_amount_input_ = new QLineEdit(QStringLiteral("0.001"));
    withdraw_amount_input_->setPlaceholderText("0.00000001");
    withdraw_amount_input_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^(0|[1-9][0-9]{0,10})(?:\\.[0-9]{0,8})?$")),
        withdraw_amount_input_));
    wd_form->addRow("Amount (DIN):", withdraw_amount_input_);
    withdraw_destination_input_ = new QLineEdit();
    withdraw_destination_input_->setPlaceholderText(
        "din1p… Taproot address");
    wd_form->addRow("Destination:", withdraw_destination_input_);
    wd_layout->addLayout(wd_form);

    auto* wd_action_row = new QHBoxLayout();
    btn_withdraw_ = new QPushButton("Review Withdrawal");
    btn_withdraw_->setStyleSheet("font-weight: bold; padding: 6px 14px;");
    wd_action_row->addStretch();
    wd_action_row->addWidget(btn_withdraw_);
    wd_layout->addLayout(wd_action_row);

    lbl_last_request_id_ = new QLabel("Last request: \xE2\x80\x93");
    lbl_last_request_id_->setStyleSheet("color: #9fb3c8;");
    wd_layout->addWidget(lbl_last_request_id_);

    auto* status_row = new QHBoxLayout();
    status_row->addWidget(new QLabel("Status of:"));
    status_request_id_input_ = new QLineEdit();
    status_request_id_input_->setPlaceholderText("withdrawal_id");
    status_row->addWidget(status_request_id_input_);
    btn_check_status_ = new QPushButton("Check");
    status_row->addWidget(btn_check_status_);
    lbl_status_value_ = new QLabel("\xE2\x80\x93");
    lbl_status_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(withdraw_group_);

    // Raw operator/ledger diagnostics are intentionally outside the normal
    // consumer flow. They remain available for support and operators.
    advanced_group_ = new QGroupBox("Advanced / Operator Details");
    advanced_group_->setCheckable(true);
    advanced_group_->setChecked(false);
    auto* advanced_outer = new QVBoxLayout(advanced_group_);
    auto* advanced_content = new QWidget(advanced_group_);
    auto* log_layout = new QVBoxLayout(advanced_content);
    auto* raw_metrics = new QLabel();
    raw_metrics->setText(QStringLiteral("Ledger sequence and daemon-wide account/loss metrics are available in Developer status."));
    raw_metrics->setWordWrap(true);
    log_layout->addWidget(raw_metrics);
    log_layout->addWidget(new QLabel("Manual withdrawal status lookup:"));
    log_layout->addLayout(status_row);
    log_layout->addWidget(lbl_status_value_);
    log_layout->addWidget(new QLabel("Activity:"));
    event_log_ = new QTextEdit();
    event_log_->setReadOnly(true);
    event_log_->setMaximumHeight(140);
    log_layout->addWidget(event_log_);
    advanced_outer->addWidget(advanced_content);
    advanced_content->setVisible(false);
    connect(advanced_group_, &QGroupBox::toggled, advanced_content, &QWidget::setVisible);
    root->addWidget(advanced_group_);

    root->addStretch();

    connect(refresh_btn, &QPushButton::clicked, this, &VaultPanel::onRefresh);
}

void VaultPanel::setupConnections() {
    connect(&refresh_timer_, &QTimer::timeout, this, &VaultPanel::onRefresh);
    connect(account_id_input_, &QLineEdit::editingFinished,
            this, &VaultPanel::onAccountFieldChanged);
    connect(btn_withdraw_, &QPushButton::clicked, this, &VaultPanel::onWithdrawClicked);
    connect(btn_check_status_, &QPushButton::clicked,
            this, &VaultPanel::onCheckStatusClicked);
    connect(rpc_, &RpcClient::rpcResult, this, &VaultPanel::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &VaultPanel::onRpcError);
}

void VaultPanel::callRpc(const QString& method, const QJsonObject& params) {
    rpc_->callNamed(method, params);
}

void VaultPanel::callRpc(const QString& method, const QJsonArray& params) {
    rpc_->call(method, params);
}

void VaultPanel::onRefresh() {
    callRpc("vault.metrics", QJsonObject{});
    if (!operator_bind_inflight_ && !waiting_for_wallet_primary_) {
        operator_bind_attempted_ = true;
        callRpc("vault.getoperator", QJsonObject{});
    }
    onAccountFieldChanged();
    reconcileWithdrawalJournal();
}

void VaultPanel::refresh() {
    // Clear wallet-scoped labels so previous wallet values do not linger
    // while the new wallet/account RPC calls are in flight. Service-level
    // totals are daemon-scoped and can keep their values until refreshed.
    clearWalletScopedFields(wallet_scope_.isEmpty()
        ? QStringLiteral("<span style='color:#d8a37b;'>no wallet loaded</span>")
        : QStringLiteral("<span style='color:#d8a37b;'>refreshing active wallet vault…</span>"));
    onRefresh();
}

void VaultPanel::onAccountFieldChanged() {
    const QString account = account_id_input_->text().trimmed();
    if (account.isEmpty()) {
        return;
    }
    callRpc("vault.account.metrics", QJsonArray{account});
}

void VaultPanel::onWithdrawClicked() {
    if (withdrawal_submitting_ || withdrawal_journal_stage_ == "authorized" ||
        withdrawal_journal_stage_ == "submitting" || withdrawal_journal_stage_ == "accepted") {
        appendLog("A withdrawal is already awaiting reconciliation.");
        return;
    }
    const QString account = withdraw_account_input_->text().trimmed();
    const QString destination = withdraw_destination_input_->text().trimmed();
    qint64 amount = 0;
    if (!parseDinAmount(withdraw_amount_input_->text(), &amount)) {
        QMessageBox::warning(this, "Invalid Vault Amount",
                             "Enter a positive DIN amount with no more than 8 decimal places.");
        return;
    }
    if (!isTaprootAddress(destination)) {
        QMessageBox::warning(this, "Invalid Vault Destination",
                             "Enter a Taproot Dinero address beginning with din1p…");
        return;
    }
    if (account_spendable_una_ <= 0 || amount > account_spendable_una_) {
        QMessageBox::warning(this, "Insufficient Vault Balance",
                             QString("This vault can currently withdraw %1. Requested: %2.")
                                 .arg(formatAmountPlain(account_spendable_una_),
                                      formatAmountPlain(amount)));
        return;
    }
    const auto answer = QMessageBox::question(
        this, "Review Vault Withdrawal",
        QString("Vault account:\n%1\n\nDestination:\n%2\n\nAmount: %3\n\n"
                "The active wallet must be unlocked. Enqueuing reserves vault funds.")
            .arg(account, destination, formatAmountPlain(amount)),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    if (!saveWithdrawalJournal("authorized", account, amount, destination)) {
        appendLog("Authorization could not be persisted; nothing submitted.");
        return;
    }
    pending_withdrawal_account_ = account;
    pending_withdrawal_destination_ = destination;
    pending_withdrawal_amount_ = amount;
    withdrawal_waiting_wallet_status_ = true;
    callRpc("wallet.getinfo", QJsonObject{});
}

void VaultPanel::onCheckStatusClicked() {
    const QString req_id = status_request_id_input_->text().trimmed();
    if (req_id.isEmpty()) {
        return;
    }
    callRpc("vault.withdrawal.status", QJsonArray{req_id});
}

void VaultPanel::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method.startsWith("vault.")) {
        markVaultConnectionHealthy();
    }
    if (method == "vault.metrics") {
        if (!result.isObject()) {
            return;
        }
        const QJsonObject obj = result.toObject();
        lbl_runtime_status_->setText("<span style='color:#7bd88f;'>active</span>");
        lbl_total_credits_->setText(formatAmountRich(safeInt(obj.value("total_open_credits_una"))));
        lbl_total_loss_->setText(formatAmountRich(safeInt(obj.value("total_operator_loss_una"))));
        lbl_queue_depth_->setText(QString::number(safeInt(obj.value("withdrawal_queue_depth"))));
        lbl_ledger_seq_->setText(QString::number(safeInt(obj.value("ledger_next_seq"))));
        lbl_account_count_->setText(QString::number(safeInt(obj.value("account_count"))));
        updateDeveloperSummary();
        return;
    }
    if (method == "vault.getoperator") {
        if (waiting_for_wallet_primary_ || operator_bind_inflight_) {
            return;
        }
        if (!result.isObject()) {
            return;
        }
        const QJsonObject obj = result.toObject();
        const QString addr = safeString(obj.value("address"));
        const QString account = safeString(obj.value("account"));
        if (!addr.isEmpty()) {
            waiting_for_wallet_primary_ = false;
            operator_bind_inflight_ = false;
            lbl_operator_address_->setText(
                QString("<span style='color:#9fb3c8;'>%1</span>"
                        "<br/><span style='color:#7f8c9b; font-size:11px;'>Assigned account: %2</span>")
                    .arg(addr.toHtmlEscaped(), account.toHtmlEscaped()));
            // The daemon's binding is the source of truth. Legacy bindings
            // such as wallet:Charlie may already own credit and must never be
            // silently renamed or stranded by a UI migration.
            account_id_input_->setText(account);
            withdraw_account_input_->setText(account);
            onAccountFieldChanged();
        } else if (!wallet_scope_.isEmpty()) {
            requestWalletPrimaryBinding();
        } else if (!operator_bind_attempted_) {
            // First-time launch: no operator yet. Auto-bind by
            // querying the wallet's primary address; we bind it
            // when wallet.getinfo returns. Marking the flag here
            // avoids re-firing on every 6s refresh tick.
            operator_bind_attempted_ = true;
            waiting_for_wallet_primary_ = true;
            appendLog("No operator address bound — auto-binding to wallet primary…");
            callRpc("wallet.getinfo", QJsonObject{});
        } else {
            lbl_operator_address_->setText(
                "<span style='color:#d8a37b;'>not bound</span>");
        }
        return;
    }
    if (method == "wallet.getinfo") {
        if (withdrawal_waiting_wallet_status_) {
            withdrawal_waiting_wallet_status_ = false;
            const QJsonObject info = result.toObject();
            const bool encrypted = info.value("encrypted").toBool(false);
            if (!info.value("unlocked").toBool(!encrypted)) {
                saveWithdrawalJournal("rejected", pending_withdrawal_account_,
                                      pending_withdrawal_amount_,
                                      pending_withdrawal_destination_);
                appendLog("Withdrawal rejected locally: unlock the active wallet first.");
                return;
            }
            const QString account = pending_withdrawal_account_;
            const QString destination = pending_withdrawal_destination_;
            const qint64 amount = pending_withdrawal_amount_;
            if (!saveWithdrawalJournal("submitting", account, amount, destination)) {
                appendLog("Submission state could not be persisted; nothing submitted.");
                return;
            }
            setWithdrawalSubmitting(true);
            callRpc("vault.withdraw", QJsonArray{QJsonObject{
                {"account_id", account}, {"amount_una", amount},
                {"destination_address", destination},
            }});
            return;
        }
        if (!waiting_for_wallet_primary_ || operator_bind_inflight_) {
            return;
        }
        if (!result.isObject()) {
            return;
        }
        const QString primary = safeString(result.toObject().value("primary_address"));
        if (primary.isEmpty()) {
            waiting_for_wallet_primary_ = false;
            appendLog("Auto-bind failed: wallet has no primary address yet.");
            return;
        }
        QJsonObject params;
        params["address"] = primary;
        params["account"] = activeAccountKey();
        waiting_for_wallet_primary_ = false;
        operator_bind_inflight_ = true;
        callRpc("vault.setoperator", QJsonArray{params});
        appendLog(QString("→ vault.setoperator %1 account=%2").arg(primary, activeAccountKey()));
        return;
    }
    if (method == "vault.setoperator") {
        operator_bind_inflight_ = false;
        if (result.isObject()) {
            const QString addr = safeString(result.toObject().value("address"));
            appendLog(addr.isEmpty()
                          ? "Operator unbound."
                          : QString("Operator bound to %1").arg(addr));
        }
        // Force an immediate refresh so the UI reflects the new binding.
        callRpc("vault.getoperator", QJsonObject{});
        callRpc("vault.metrics", QJsonObject{});
        return;
    }
    if (method == "vault.account.metrics") {
        if (!result.isObject()) {
            return;
        }
        const QJsonObject obj = result.toObject();
        lbl_account_confirmed_->setText(formatAmountRich(safeInt(obj.value("confirmed_una"))));
        lbl_account_pending_->setText(formatAmountRich(safeInt(obj.value("pending_una"))));
        lbl_account_locked_->setText(formatAmountRich(safeInt(obj.value("locked_una"))));
        account_spendable_una_ = safeInt(obj.value("spendable_una"));
        lbl_account_spendable_->setText(formatAmountRich(account_spendable_una_));
        lbl_account_loss_->setText(formatAmountRich(safeInt(obj.value("operator_loss_una"))));
        return;
    }
    if (method == "vault.withdraw") {
        setWithdrawalSubmitting(false);
        const QString id = result.isObject()
                               ? safeString(result.toObject().value("request_id"))
                               : safeString(result);
        if (!id.isEmpty()) {
            lbl_last_request_id_->setText("Last request: " + id);
            status_request_id_input_->setText(id);
            appendLog("← vault.withdraw enqueued: " + id);
            saveWithdrawalJournal("accepted", pending_withdrawal_account_,
                                  pending_withdrawal_amount_, pending_withdrawal_destination_, id);
            reconcileWithdrawalJournal();
        }
        return;
    }
    if (method == "vault.withdrawal.status") {
        if (result.isObject()) {
            const QJsonObject obj = result.toObject();
            const QString state = safeString(obj.value("state"));
            lbl_status_value_->setText(QString(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
            if (state == "settled" || state == "failed" || state == "reverted") {
                const QString requestId = safeString(obj.value("request_id"));
                saveWithdrawalJournal(state, pending_withdrawal_account_,
                                      pending_withdrawal_amount_, pending_withdrawal_destination_,
                                      requestId);
                setWithdrawalSubmitting(false);
                appendLog(state == "settled"
                              ? QString("Withdrawal settled on chain: %1").arg(requestId)
                              : QString("Withdrawal %1: %2").arg(state, requestId));
                onAccountFieldChanged();
            }
        } else {
            lbl_status_value_->setText(safeString(result));
        }
        return;
    }
}

void VaultPanel::onRpcError(const QString& method, int code, const QString& message) {
    if (method == "wallet.getinfo" && withdrawal_waiting_wallet_status_) {
        withdrawal_waiting_wallet_status_ = false;
        appendLog("Could not verify wallet authorization; nothing submitted.");
        return;
    }
    if (!method.startsWith("vault.")) {
        return;
    }
    if (isTransientConnectionError(code, message)) {
        transient_connection_error_ = true;
        lbl_connection_status_->setText(
            "<span style='color:#d8a37b;'>waiting for daemon…</span>");
        if (method == "vault.metrics") {
            lbl_runtime_status_->setText("<span style='color:#d8a37b;'>starting…</span>");
        }
        return;
    }
    if (method == "vault.metrics") {
        // Most common: runtime not initialised. Surface as a calmer
        // status rather than an error log spam every refresh tick.
        lbl_runtime_status_->setText(
            "<span style='color:#d8a37b;'>disabled</span> (set vault=1)");
        developer_summary_ = QStringLiteral("Vault raw metrics: unavailable (vault service disabled)");
        Q_EMIT developerSummaryChanged(developer_summary_);
        return;
    }
    appendLog(QString("✗ %1 [%2]: %3").arg(method).arg(code).arg(message));
    if (method == "vault.withdraw") {
        setWithdrawalSubmitting(true);
        appendLog("Outcome uncertain; automatic retry is disabled. Check status and history first.");
    }
}

void VaultPanel::setWithdrawalSubmitting(bool submitting) {
    withdrawal_submitting_ = submitting;
    if (btn_withdraw_) btn_withdraw_->setEnabled(!submitting);
}

bool VaultPanel::saveWithdrawalJournal(const QString& stage, const QString& account,
                                       qint64 amount, const QString& destination,
                                       const QString& requestId) {
    QSettings s;
    const QString base = "vault/withdrawalJournal/" + journalKeyForWalletScope(wallet_scope_);
    s.setValue(base + "/stage", stage);
    s.setValue(base + "/account", account);
    s.setValue(base + "/amount", amount);
    s.setValue(base + "/destination", destination);
    s.setValue(base + "/requestId", requestId);
    s.setValue(base + "/updatedAt", QDateTime::currentDateTimeUtc());
    s.sync();
    withdrawal_journal_stage_ = stage;
    return s.status() == QSettings::NoError && s.value(base + "/stage").toString() == stage;
}

void VaultPanel::loadWithdrawalJournal() {
    QSettings s;
    const QString base = "vault/withdrawalJournal/" + journalKeyForWalletScope(wallet_scope_);
    withdrawal_journal_stage_ = s.value(base + "/stage").toString();
    if (withdrawal_journal_stage_.isEmpty()) return;
    withdraw_account_input_->setText(s.value(base + "/account").toString());
    pending_withdrawal_account_ = s.value(base + "/account").toString();
    pending_withdrawal_amount_ = s.value(base + "/amount").toLongLong();
    pending_withdrawal_destination_ = s.value(base + "/destination").toString();
    withdraw_amount_input_->setText(formatDinAmount(pending_withdrawal_amount_));
    withdraw_destination_input_->setText(s.value(base + "/destination").toString());
    const QString requestId = s.value(base + "/requestId").toString();
    if (withdrawal_journal_stage_ == "submitting") {
        setWithdrawalSubmitting(true);
        appendLog("Previous withdrawal outcome is uncertain; automatic retry is disabled.");
    } else if (withdrawal_journal_stage_ == "accepted" && !requestId.isEmpty()) {
        status_request_id_input_->setText(requestId);
        lbl_last_request_id_->setText("Last request: " + requestId);
        setWithdrawalSubmitting(true);
        reconcileWithdrawalJournal();
    }
}

void VaultPanel::reconcileWithdrawalJournal() {
    if (withdrawal_journal_stage_ != "accepted") return;
    QSettings s;
    const QString base = "vault/withdrawalJournal/" + journalKeyForWalletScope(wallet_scope_);
    const QString requestId = s.value(base + "/requestId").toString().trimmed();
    if (requestId.isEmpty()) return;
    status_request_id_input_->setText(requestId);
    callRpc("vault.withdrawal.status", QJsonArray{requestId});
}

bool VaultPanel::isTransientConnectionError(int code, const QString& message) const {
    const QString lower = message.toLower();
    return code == -1 && (lower.contains("connection refused") ||
                          lower.contains("socket") || lower.contains("not connected") ||
                          lower.contains("daemon") || lower.contains("temporarily unavailable"));
}

void VaultPanel::markVaultConnectionHealthy() {
    lbl_connection_status_->setText("<span style='color:#7bd88f;'>connected</span>");
    if (!transient_connection_error_) return;
    transient_connection_error_ = false;
    // Transient startup failures are transport noise, not vault activity.
    // Remove the stale visual pollution once the daemon answers again.
    event_log_->clear();
    appendLog("Vault connection restored.");
}

void VaultPanel::appendLog(const QString& message) {
    const QString stamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    event_log_->append(QString("[%1] %2").arg(stamp, message));
}
