// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — daemon-side custodial vault UI.
//
// Distinct from the on-chain Tapscript covenant "Vault" template
// surfaced under the Contracts tab. This widget talks to the
// daemon's Track-C `vault.*` RPC family:
//   vault.metrics              — service-level totals
//   vault.account.spendable    — per-account spendable balance
//   vault.account.metrics      — per-account confirmed/pending/locked
//   vault.observe              — operator/debug deposit recording
//   vault.withdraw             — enqueue a withdrawal request
//   vault.transfer             — move settled balance to another vault
//                                account (off-chain, instant, no fee)
//   vault.withdrawal.status    — poll a withdrawal's lifecycle state

#pragma once

#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>

class RpcClient;

class VaultPanel : public QWidget {
    Q_OBJECT

public:
    explicit VaultPanel(RpcClient* rpc, QWidget* parent = nullptr);
    ~VaultPanel() override;
    QString developerSummary() const;

public Q_SLOTS:
    // Bind the panel to the active Qt wallet. Wallet switches are hard
    // boundaries: the previous wallet's operator address, account fields,
    // withdrawal state, and auto-bind flags must not leak into the next
    // active wallet session.
    void setWalletScope(const QString& walletName);
    // Forces an immediate re-fetch of vault metrics + operator binding.
    // MainWindow calls this on wallet switch so the panel doesn't display
    // stale per-account state from the previously active wallet.
    void refresh();

private Q_SLOTS:
    void onRefresh();
    void onAccountFieldChanged();
    void onWithdrawClicked();
    void onTransferClicked();
    void onCheckStatusClicked();
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

Q_SIGNALS:
    void developerSummaryChanged(const QString& summary);

private:
    void setupUi();
    void setupConnections();
    void callRpc(const QString& method, const QJsonObject& params);
    void callRpc(const QString& method, const QJsonArray& params);
    void appendLog(const QString& message);
    static QString formatAmountPlain(qint64 una);
    static QString formatAmountRich(qint64 una);
    static QString formatMetricTitle(const QString& title, const QString& helper);
    static QString accountKeyForWalletScope(const QString& walletName);
    QString activeAccountKey() const;
    void clearWalletScopedFields(const QString& operatorText);
    void requestWalletPrimaryBinding();
    void updateDeveloperSummary();

    RpcClient* rpc_;
    QTimer refresh_timer_;
    QString wallet_scope_;
    bool operator_bind_attempted_ = false;
    bool waiting_for_wallet_primary_ = false;
    bool operator_bind_inflight_ = false;
    QString developer_summary_ = QStringLiteral("Vault raw metrics: waiting for first refresh...");

    // Service-level metrics panel.
    QGroupBox* service_group_;
    QLabel* lbl_runtime_status_;
    QLabel* lbl_operator_address_;
    QLabel* lbl_total_credits_;
    QLabel* lbl_total_loss_;
    QLabel* lbl_queue_depth_;
    QLabel* lbl_ledger_seq_;
    QLabel* lbl_account_count_;
    /// Shown only when a restart replayed balances for deposits or
    /// withdrawals whose state machine could not be restored.
    QLabel* lbl_reconcile_warning_;

    // Account inspector.
    QGroupBox* account_group_;
    QLineEdit* account_id_input_;
    QLabel* lbl_account_confirmed_;
    QLabel* lbl_account_pending_;
    QLabel* lbl_account_locked_;
    QLabel* lbl_account_spendable_;
    QLabel* lbl_account_transferable_;
    QLabel* lbl_account_loss_;

    // Internal transfer form (account -> account, settled funds only).
    QGroupBox* transfer_group_;
    QLineEdit* transfer_from_input_;
    QLineEdit* transfer_to_input_;
    QSpinBox* transfer_amount_input_;
    QPushButton* btn_transfer_;
    QLabel* lbl_transfer_status_;

    // Withdrawal form.
    QGroupBox* withdraw_group_;
    QLineEdit* withdraw_account_input_;
    QSpinBox* withdraw_amount_input_;
    QLineEdit* withdraw_destination_input_;
    QPushButton* btn_withdraw_;
    QLabel* lbl_last_request_id_;
    QLineEdit* status_request_id_input_;
    QPushButton* btn_check_status_;
    QLabel* lbl_status_value_;

    // Activity log.
    QTextEdit* event_log_;

    static constexpr int REFRESH_INTERVAL_MS = 6000;
};
