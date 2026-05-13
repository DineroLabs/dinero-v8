#ifndef TRANSACTIONTRACKER_H
#define TRANSACTIONTRACKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <vector>

class RpcClient;

enum class TxStatus {
    Pending,     // Broadcast but not yet seen in mempool
    Mempool,     // Seen in mempool
    Confirmed,   // 1-5 confirmations
    Finalized,   // 6+ confirmations
    Replaced,    // Outpoint-overlap replacement detected
    Evicted,     // Disappeared from mempool without confirmation
    Failed       // Rejected by node
};

struct TerminalAdvisoryEvent {
    QString txid;
    TxStatus status;
    QString reason;
    QString reservationId;
};

struct TrackedTransaction {
    QString txid;
    bool isIncoming = false;
    qint64 amountUna = 0;
    QString address;           // recipient
    TxStatus status = TxStatus::Pending;
    int confirmations = 0;

    // Outpoint tracking (for replacement detection)
    QStringList selectedInputOutpoints;          // "txid:vout" format
    QMap<QString, qint64> selectedInputAmountsUna; // outpoint → amount

    // Change and fee
    QString changeAddress;
    qint64 changeAmountUna = 0;
    qint64 feePaidUna = 0;

    // Mempool lifecycle
    int mempoolMissingPolls = 0;
    QDateTime lastSeenInMempool;
    QDateTime createdAt;

    // Replacement
    QString replacedByTxid;
    QString statusReason;

    // Phase 5 notification state
    int notifiedConfirmations = 0;

    // Phase 6 link
    QString reservationId;
};

/**
 * Tracks local outgoing transactions through the mempool lifecycle.
 *
 * Authoritative only for: pending → mempool → evicted/replaced/failed.
 * Confirmed chain truth comes from wallet/chain data — tracker does
 * NOT become a second source of confirmed truth.
 *
 * Polling: 2s non-overlapping timer with generation counter to prevent
 * stale RPC replies from overwriting newer state.
 */
class TransactionTracker : public QObject {
    Q_OBJECT
public:
    explicit TransactionTracker(const QString& dataDir, RpcClient* rpc, QObject* parent = nullptr);

    void setDataDir(const QString& dataDir);
    void setWalletScope(const QString& walletScope);

    /// Add a newly broadcast transaction from the send flow.
    void trackSend(const TrackedTransaction& tx);

    /// Get all tracked transactions (for UI display merge).
    const std::vector<TrackedTransaction>& transactions() const { return transactions_; }

    /// Number of unfinalized (active) transactions.
    int pendingCount() const;

    /// Start/stop the polling timer.
    void startPolling();
    void stopPolling();

Q_SIGNALS:
    void transactionUpdated(const QString& txid, TxStatus newStatus);
    void terminalEvent(const TerminalAdvisoryEvent& event);
    void pendingCountChanged(int count);

public Q_SLOTS:
    void onPollRpcResult(const QString& method, const QJsonValue& result);

private Q_SLOTS:
    void onPollTick();

private:
    void load();
    void save();
    bool loadFromPath(const QString& path);
    void resetRuntimeState();
    void processBlockCount(int height);
    void processRawMempool(const QJsonArray& txids);
    void processRawTransaction(const QString& txid, const QJsonObject& txData);
    void detectReplacement(TrackedTransaction& tx);
    void transitionTo(TrackedTransaction& tx, TxStatus newStatus, const QString& reason = {});
    TrackedTransaction* findByTxid(const QString& txid);
    bool hasUnfinalizedTransactions() const;

    QString filePath() const;
    QString legacyFilePath() const;

    QString dataDir_;
    QString walletScope_;
    RpcClient* rpc_;
    QTimer* pollTimer_;

    std::vector<TrackedTransaction> transactions_;

    // Non-overlapping poll guard
    bool pollInFlight_ = false;
    quint64 pollGeneration_ = 0;
    quint64 activePollGeneration_ = 0;

    // Poll state machine
    enum PollPhase { Idle, WaitingBlockCount, WaitingMempool, WaitingTxDetails };
    PollPhase pollPhase_ = Idle;
    int currentHeight_ = 0;
    QSet<QString> mempoolTxids_;
    int pendingTxQueries_ = 0;

    static constexpr int POLL_INTERVAL_MS = 2000;
    static constexpr int MISSING_POLLS_BEFORE_EVICTION = 2;
    static constexpr int FINALIZATION_CONFIRMATIONS = 6;
    static constexpr int SCHEMA_VERSION = 1;
};

#endif // TRANSACTIONTRACKER_H
