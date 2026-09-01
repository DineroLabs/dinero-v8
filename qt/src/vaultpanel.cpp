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
#include <QRegularExpression>
#include <QVBoxLayout>

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
    QString scope = walletName.trimmed();
    if (scope.isEmpty()) {
        return QStringLiteral("wallet");
    }
    scope.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.:-]+")), QStringLiteral("_"));
    if (scope.size() > 96) {
        scope = scope.left(96);
    }
    return QStringLiteral("wallet:%1").arg(scope);
}

QString VaultPanel::activeAccountKey() const {
    return accountKeyForWalletScope(wallet_scope_);
}

void VaultPanel::clearWalletScopedFields(const QString& operatorText) {
    if (lbl_operator_address_) lbl_operator_address_->setText(operatorText);
    if (lbl_account_confirmed_) lbl_account_confirmed_->setText("—");
    if (lbl_account_pending_)   lbl_account_pending_->setText("—");
    if (lbl_account_locked_)    lbl_account_locked_->setText("—");
    if (lbl_account_spendable_) lbl_account_spendable_->setText("—");
    if (lbl_account_transferable_) lbl_account_transferable_->setText("—");
    if (lbl_account_loss_)      lbl_account_loss_->setText("—");
    if (lbl_transfer_status_)   lbl_transfer_status_->setText("—");
    if (transfer_to_input_)     transfer_to_input_->clear();
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
        if (transfer_from_input_) transfer_from_input_->clear();
        return;
    }

    const QString account = activeAccountKey();
    clearWalletScopedFields("<span style='color:#d8a37b;'>rebinding to active wallet…</span>");
    if (account_id_input_) account_id_input_->setText(account);
    if (withdraw_account_input_) withdraw_account_input_->setText(account);
    if (transfer_from_input_) transfer_from_input_->setText(account);
    appendLog(QString("Wallet switched — vault scope is now %1").arg(wallet_scope_));
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
        "credits open at K=10 confirmations, settle at K=20.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #9fb3c8; padding: 4px;");
    root->addWidget(hint);

    // Service-level metrics.
    service_group_ = new QGroupBox("Vault Summary");
    auto* svc_grid = new QGridLayout(service_group_);
    lbl_runtime_status_ = new QLabel("unknown");
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
    svc_grid->addWidget(new QLabel("Withdrawal queue:"), 0, 2);
    svc_grid->addWidget(lbl_queue_depth_, 0, 3);
    svc_grid->addWidget(new QLabel("Settling:"), 1, 0);
    svc_grid->addWidget(lbl_total_credits_, 1, 1);
    svc_grid->addWidget(new QLabel("Operator loss:"), 2, 0);
    svc_grid->addWidget(lbl_total_loss_, 2, 1);
    svc_grid->addWidget(new QLabel("Accounts:"), 2, 2);
    svc_grid->addWidget(lbl_account_count_, 2, 3);
    svc_grid->addWidget(new QLabel("Vault Deposit Address:"), 3, 0);
    svc_grid->addWidget(lbl_operator_address_, 3, 1, 1, 3);
    lbl_reconcile_warning_ = new QLabel();
    lbl_reconcile_warning_->setTextFormat(Qt::RichText);
    lbl_reconcile_warning_->setWordWrap(true);
    lbl_reconcile_warning_->setStyleSheet(
        "QLabel { background: #3a2a12; border: 1px solid #d8a37b; border-radius: 4px; padding: 6px; }");
    lbl_reconcile_warning_->hide();
    svc_grid->addWidget(lbl_reconcile_warning_, 4, 0, 1, 4);
    root->addWidget(service_group_);

    // Per-account inspector.
    account_group_ = new QGroupBox("Vault Balance");
    auto* acc_layout = new QVBoxLayout(account_group_);
    auto* acc_input_row = new QHBoxLayout();
    acc_input_row->addWidget(new QLabel("Vault account:"));
    account_id_input_ = new QLineEdit();
    account_id_input_->setPlaceholderText("e.g. user-42 or wallet/account-id");
    acc_input_row->addWidget(account_id_input_);
    acc_layout->addLayout(acc_input_row);

    auto* acc_grid = new QGridLayout();
    lbl_account_confirmed_ = new QLabel("\xE2\x80\x93");
    lbl_account_pending_ = new QLabel("\xE2\x80\x93");
    lbl_account_locked_ = new QLabel("\xE2\x80\x93");
    lbl_account_spendable_ = new QLabel("\xE2\x80\x93");
    lbl_account_transferable_ = new QLabel("\xE2\x80\x93");
    lbl_account_loss_ = new QLabel("\xE2\x80\x93");
    for (QLabel* label : {lbl_total_credits_, lbl_total_loss_, lbl_account_confirmed_,
                          lbl_account_pending_, lbl_account_locked_, lbl_account_spendable_,
                          lbl_account_transferable_, lbl_account_loss_}) {
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
    acc_grid->addWidget(new QLabel(formatMetricTitle(
        "Sendable In-Vault", "Settled funds this account can send to another vault account.")), 2, 0);
    acc_grid->addWidget(lbl_account_transferable_, 2, 1);
    acc_grid->addWidget(new QLabel(formatMetricTitle(
        "Operator Loss", "Only used if a credited deposit later has to be reversed.")), 2, 2);
    acc_grid->addWidget(lbl_account_loss_, 2, 3);
    acc_layout->addLayout(acc_grid);
    root->addWidget(account_group_);

    // Internal transfer form. Off-chain move between two accounts of
    // this same vault — no transaction, no fee, no confirmations.
    transfer_group_ = new QGroupBox("Send Inside Vault");
    auto* tf_layout = new QVBoxLayout(transfer_group_);
    auto* tf_hint = new QLabel(
        "Move settled balance to another account in this vault. Instant and "
        "feeless — nothing touches the chain. Only “Sendable In-Vault” funds "
        "can move; deposits still settling stay put until they finish.");
    tf_hint->setWordWrap(true);
    tf_hint->setStyleSheet("color: #9fb3c8; padding: 2px;");
    tf_layout->addWidget(tf_hint);

    auto* tf_form = new QFormLayout();
    transfer_from_input_ = new QLineEdit();
    transfer_from_input_->setPlaceholderText("Account ID");
    tf_form->addRow("From:", transfer_from_input_);
    transfer_to_input_ = new QLineEdit();
    transfer_to_input_->setPlaceholderText("Destination vault account ID");
    tf_form->addRow("To:", transfer_to_input_);
    transfer_amount_input_ = new QSpinBox();
    transfer_amount_input_->setRange(1, 1000000000);
    transfer_amount_input_->setSuffix(" una");
    transfer_amount_input_->setValue(100000);  // 0.001 DIN
    tf_form->addRow("Amount:", transfer_amount_input_);
    tf_layout->addLayout(tf_form);

    auto* tf_action_row = new QHBoxLayout();
    btn_transfer_ = new QPushButton("Send In-Vault");
    btn_transfer_->setStyleSheet("font-weight: bold; padding: 6px 14px;");
    tf_action_row->addStretch();
    tf_action_row->addWidget(btn_transfer_);
    tf_layout->addLayout(tf_action_row);

    lbl_transfer_status_ = new QLabel("\xE2\x80\x93");
    lbl_transfer_status_->setStyleSheet("color: #9fb3c8;");
    lbl_transfer_status_->setTextFormat(Qt::RichText);
    lbl_transfer_status_->setWordWrap(true);
    lbl_transfer_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tf_layout->addWidget(lbl_transfer_status_);
    root->addWidget(transfer_group_);

    // Withdrawal form + status check.
    withdraw_group_ = new QGroupBox("Withdrawal");
    auto* wd_layout = new QVBoxLayout(withdraw_group_);
    auto* wd_form = new QFormLayout();
    withdraw_account_input_ = new QLineEdit();
    withdraw_account_input_->setPlaceholderText("Account ID");
    wd_form->addRow("Account:", withdraw_account_input_);
    withdraw_amount_input_ = new QSpinBox();
    withdraw_amount_input_->setRange(1, 1000000000);
    withdraw_amount_input_->setSuffix(" una");
    withdraw_amount_input_->setValue(100000);  // 0.001 DIN
    wd_form->addRow("Amount:", withdraw_amount_input_);
    withdraw_destination_input_ = new QLineEdit();
    withdraw_destination_input_->setPlaceholderText(
        "scriptPubKey hex (e.g. 5120<32-byte x-only>)");
    wd_form->addRow("Destination:", withdraw_destination_input_);
    wd_layout->addLayout(wd_form);

    auto* wd_action_row = new QHBoxLayout();
    btn_withdraw_ = new QPushButton("Enqueue Withdrawal");
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
    wd_layout->addLayout(status_row);

    lbl_status_value_ = new QLabel("\xE2\x80\x93");
    lbl_status_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wd_layout->addWidget(lbl_status_value_);
    root->addWidget(withdraw_group_);

    // Activity log.
    auto* log_group = new QGroupBox("Activity");
    auto* log_layout = new QVBoxLayout(log_group);
    event_log_ = new QTextEdit();
    event_log_->setReadOnly(true);
    event_log_->setMaximumHeight(140);
    log_layout->addWidget(event_log_);
    root->addWidget(log_group);

    root->addStretch();

    connect(refresh_btn, &QPushButton::clicked, this, &VaultPanel::onRefresh);
}

void VaultPanel::setupConnections() {
    connect(&refresh_timer_, &QTimer::timeout, this, &VaultPanel::onRefresh);
    connect(account_id_input_, &QLineEdit::editingFinished,
            this, &VaultPanel::onAccountFieldChanged);
    connect(btn_withdraw_, &QPushButton::clicked, this, &VaultPanel::onWithdrawClicked);
    connect(btn_transfer_, &QPushButton::clicked, this, &VaultPanel::onTransferClicked);
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
    if (!wallet_scope_.isEmpty() && !operator_bind_attempted_) {
        requestWalletPrimaryBinding();
    } else if (!operator_bind_inflight_ && !waiting_for_wallet_primary_) {
        callRpc("vault.getoperator", QJsonObject{});
    }
    onAccountFieldChanged();
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
    const QString account = withdraw_account_input_->text().trimmed();
    const QString destination = withdraw_destination_input_->text().trimmed();
    const qint64 amount = withdraw_amount_input_->value();
    if (account.isEmpty() || destination.isEmpty()) {
        appendLog("Withdrawal: account and destination are required.");
        return;
    }
    callRpc("vault.withdraw", QJsonArray{QJsonObject{
        {"account_id", account},
        {"amount_una", static_cast<qint64>(amount)},
        {"destination_address", destination},
    }});
    appendLog(QString("→ vault.withdraw account=%1 amount=%2")
                  .arg(account, formatAmountPlain(amount)));
}

void VaultPanel::onTransferClicked() {
    const QString from = transfer_from_input_->text().trimmed();
    const QString to = transfer_to_input_->text().trimmed();
    const qint64 amount = transfer_amount_input_->value();
    if (from.isEmpty() || to.isEmpty()) {
        lbl_transfer_status_->setText(
            "<span style='color:#d8a37b;'>Both accounts are required.</span>");
        return;
    }
    if (from == to) {
        lbl_transfer_status_->setText(
            "<span style='color:#d8a37b;'>Source and destination are the same account.</span>");
        return;
    }
    lbl_transfer_status_->setText("sending…");
    callRpc("vault.transfer", QJsonArray{QJsonObject{
        {"from_account_id", from},
        {"to_account_id", to},
        {"amount_una", static_cast<qint64>(amount)},
    }});
    appendLog(QString("→ vault.transfer %1 → %2 amount=%3")
                  .arg(from, to, formatAmountPlain(amount)));
}

void VaultPanel::onCheckStatusClicked() {
    const QString req_id = status_request_id_input_->text().trimmed();
    if (req_id.isEmpty()) {
        return;
    }
    callRpc("vault.withdrawal.status", QJsonArray{req_id});
}

void VaultPanel::onRpcResult(const QString& method, const QJsonValue& result) {
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
        // Only the ledger survives a daemon restart; deposits and
        // withdrawals that were mid-lifecycle come back with correct
        // balances but no state machine to finish them.
        const qint64 stuck_deposits = safeInt(obj.value("unreconciled_deposits"));
        const qint64 stuck_withdrawals = safeInt(obj.value("unreconciled_withdrawals"));
        if (stuck_deposits > 0 || stuck_withdrawals > 0) {
            lbl_reconcile_warning_->setText(
                QString("<b style='color:#d8a37b;'>Needs reconciliation after restart</b>"
                        "<br/><span style='color:#c8b8a0; font-size:11px;'>%1 deposit(s) credited "
                        "but not settled, %2 withdrawal(s) started but not finished. These will not "
                        "complete on their own, and the withdrawals keep holding their reserved "
                        "balance until an operator resolves them.</span>")
                    .arg(stuck_deposits)
                    .arg(stuck_withdrawals));
            lbl_reconcile_warning_->show();
        } else {
            lbl_reconcile_warning_->hide();
        }
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
        if (!wallet_scope_.isEmpty() && account != activeAccountKey()) {
            appendLog(QString("Vault operator belongs to '%1'; rebinding for active wallet '%2'.")
                          .arg(account.isEmpty() ? QStringLiteral("(none)") : account,
                               wallet_scope_));
            requestWalletPrimaryBinding();
            return;
        }
        if (!addr.isEmpty()) {
            waiting_for_wallet_primary_ = false;
            operator_bind_inflight_ = false;
            lbl_operator_address_->setText(
                QString("<span style='color:#9fb3c8;'>%1</span>"
                        "<br/><span style='color:#7f8c9b; font-size:11px;'>Assigned account: %2</span>")
                    .arg(addr.toHtmlEscaped(), account.toHtmlEscaped()));
            // Set the inspector + withdrawal account fields if blank.
            if (account_id_input_->text().isEmpty()) {
                account_id_input_->setText(account);
                onAccountFieldChanged();
            }
            if (withdraw_account_input_->text().isEmpty()) {
                withdraw_account_input_->setText(account);
            }
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
        lbl_account_spendable_->setText(formatAmountRich(safeInt(obj.value("spendable_una"))));
        lbl_account_transferable_->setText(formatAmountRich(safeInt(obj.value("transferable_una"))));
        lbl_account_loss_->setText(formatAmountRich(safeInt(obj.value("operator_loss_una"))));
        return;
    }
    if (method == "vault.withdraw") {
        const QString id = result.isObject()
                               ? safeString(result.toObject().value("request_id"))
                               : safeString(result);
        if (!id.isEmpty()) {
            lbl_last_request_id_->setText("Last request: " + id);
            status_request_id_input_->setText(id);
            appendLog("← vault.withdraw enqueued: " + id);
        }
        return;
    }
    if (method == "vault.transfer") {
        if (!result.isObject()) {
            return;
        }
        const QJsonObject obj = result.toObject();
        const QString to = safeString(obj.value("to_account_id"));
        const qint64 amount = safeInt(obj.value("amount_una"));
        const QString seq = QString::number(safeInt(obj.value("seq")));
        lbl_transfer_status_->setText(
            QString("<span style='color:#7bd88f;'>Sent %1 to %2</span>"
                    "<br/><span style='color:#7f8c9b; font-size:11px;'>ledger entry #%3</span>")
                .arg(formatAmountPlain(amount).toHtmlEscaped(), to.toHtmlEscaped(), seq));
        appendLog(QString("← vault.transfer sent %1 to %2 (seq %3)")
                      .arg(formatAmountPlain(amount), to, seq));
        // Both sides moved; pull fresh per-account and service metrics.
        onAccountFieldChanged();
        callRpc("vault.metrics", QJsonObject{});
        return;
    }
    if (method == "vault.withdrawal.status") {
        if (result.isObject()) {
            lbl_status_value_->setText(QString(QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact)));
        } else {
            lbl_status_value_->setText(safeString(result));
        }
        return;
    }
}

void VaultPanel::onRpcError(const QString& method, int code, const QString& message) {
    if (!method.startsWith("vault.")) {
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
    if (method == "vault.transfer") {
        lbl_transfer_status_->setText(
            QString("<span style='color:#e06c75;'>%1</span>").arg(message.toHtmlEscaped()));
    }
    appendLog(QString("✗ %1 [%2]: %3").arg(method).arg(code).arg(message));
}

void VaultPanel::appendLog(const QString& message) {
    const QString stamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    event_log_->append(QString("[%1] %2").arg(stamp, message));
}
