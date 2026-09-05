// Copyright (c) 2026 Dinero Labs.
//
// Pool operator dashboard.
//
// Two audiences in one panel:
//   * someone who does NOT run a pool — the top section exists to tell
//     them the option exists and why it is unusually safe here;
//   * someone who DOES — live status from their pool's read-only ops
//     endpoint, plus fee earnings read from the chain.
//
// Deliberate design points:
//
//   * Earnings come from the CHAIN, never from the pool. The operator
//     fee is an output in every block the pool finds, so the node can
//     prove it. A number the pool reports about itself cannot be
//     verified, and would stop updating exactly when the pool is down —
//     which is when an operator most wants to look.
//   * This panel does NOT touch dinerod's `pool.*` RPCs. Those belong to
//     a separate, off-by-default C++ pool-accounting subsystem that has
//     nothing to do with a running SV2 pool; reading them would silently
//     report the wrong pool's numbers.

#pragma once

#include <QGroupBox>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

class RpcClient;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

class PoolPanel : public QWidget {
    Q_OBJECT

public:
    explicit PoolPanel(RpcClient* rpc, QWidget* parent = nullptr);
    ~PoolPanel() override;

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void onFetchStatusClicked();
    void onChangePayoutClicked();
    void onChangeFeeClicked();
    void onCheckEarningsClicked();
    void onOpsReplyFinished(QNetworkReply* reply);
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

private:
    void setupUi();
    void setupConnections();
    void applyStatus(const QJsonObject& status);
    bool validateStatus(const QJsonObject& status, QString* error) const;
    void markStatusStale(const QString& reason);
    void updateHealth(const QJsonObject& status, bool legacy);
    void recordHistory(const QJsonObject& status);
    void renderHistory();
    void restorePayoutJournal();
    bool persistPayoutJournal(const QString& stage, const QString& from, const QString& to);
    void reconcilePayoutJournal(const QString& observed);
    QString endpointKey() const;
    void handlePayoutReply(QNetworkReply* reply, int http);
    void handleFeeReply(QNetworkReply* reply, int http);
    bool persistFeeJournal(const QString& stage, qint64 from, qint64 to);
    void restoreFeeJournal();
    void reconcileFeeJournal(qint64 observed);
    void setPayoutMessage(const QString& html);
    void setStatusMessage(const QString& html);
    bool validateOpsUrl(const QUrl& url, QLabel* error_target) const;
    static QString formatDin(qint64 una);

    RpcClient* rpc_;
    QNetworkAccessManager* net_;
    QTimer refresh_timer_;
    /// Suppresses the "no pool configured" nag until the operator has
    /// actually asked for a connection once.
    bool ops_attempted_ = false;
    bool status_in_flight_ = false;
    bool payout_in_flight_ = false;
    bool fee_in_flight_ = false;
    bool earnings_in_flight_ = false;
    bool has_valid_status_ = false;
    QDateTime last_valid_status_;
    QGroupBox* why_group_ = nullptr;
    QPushButton* btn_about_ = nullptr;

    // Connection settings.
    QLineEdit* ops_url_input_;
    QLineEdit* ops_token_input_;
    QPushButton* btn_fetch_status_;
    QLabel* lbl_status_message_;

    // Live status.
    QGroupBox* status_group_;
    QGroupBox* earnings_group_ = nullptr;
    QLabel* lbl_connected_miners_;
    QLabel* lbl_fee_;
    QLabel* lbl_window_;
    QLabel* lbl_producer_;
    QLabel* lbl_shares_;
    QLabel* lbl_blocks_;
    QLabel* lbl_health_;
    QLabel* lbl_health_detail_;
    QLabel* lbl_daemon_;
    QLabel* lbl_stratum_;
    QLabel* lbl_last_share_;
    QLabel* lbl_last_block_;
    QLabel* lbl_rejections_;
    QLabel* lbl_history_5m_;
    QLabel* lbl_history_1h_;
    QLabel* lbl_history_24h_;
    QTableWidget* miners_table_;

    // Fee address, as reported and as changed.
    //
    // Changing it is opt-in ON THE POOL (--ops-allow-payout-change); a pool
    // that has not enabled it answers 403 and we say so plainly rather than
    // hiding the control, so an operator learns the setting exists.
    QLabel* lbl_payout_current_;
    QLineEdit* payout_input_;
    QPushButton* btn_change_payout_;
    QLabel* lbl_payout_message_;
    /// Last address /status reported. Used to avoid clobbering what the
    /// operator is mid-way through typing on a background refresh.
    QString live_payout_address_;
    QString journal_stage_;
    QString journal_from_;
    QString journal_to_;

    QDoubleSpinBox* fee_percent_input_;
    QPushButton* btn_change_fee_;
    QLabel* lbl_fee_message_;
    qint64 live_fee_bps_ = -1;
    QString fee_journal_stage_;
    qint64 fee_journal_from_ = -1;
    qint64 fee_journal_to_ = -1;

    // Chain-verified earnings.
    QLineEdit* fee_address_input_;
    QPushButton* btn_check_earnings_;
    QLabel* lbl_earnings_;

    static constexpr int REFRESH_INTERVAL_MS = 15000;
};
