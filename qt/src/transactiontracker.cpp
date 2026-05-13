#include "transactiontracker.h"
#include "rpcclient.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

// ── Serialization helpers ──────────────────────────────────────────

namespace {

QString walletScopeFileComponent(const QString& walletScope) {
    QString scope = walletScope.trimmed();
    if (scope.isEmpty()) {
        return QString();
    }

    scope.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return scope;
}

} // namespace

static QString statusToString(TxStatus s) {
    switch (s) {
    case TxStatus::Pending:   return "pending";
    case TxStatus::Mempool:   return "mempool";
    case TxStatus::Confirmed: return "confirmed";
    case TxStatus::Finalized: return "finalized";
    case TxStatus::Replaced:  return "replaced";
    case TxStatus::Evicted:   return "evicted";
    case TxStatus::Failed:    return "failed";
    }
    return "pending";
}

static TxStatus statusFromString(const QString& s) {
    if (s == "pending")   return TxStatus::Pending;
    if (s == "mempool")   return TxStatus::Mempool;
    if (s == "confirmed") return TxStatus::Confirmed;
    if (s == "finalized") return TxStatus::Finalized;
    if (s == "replaced")  return TxStatus::Replaced;
    if (s == "evicted")   return TxStatus::Evicted;
    if (s == "failed")    return TxStatus::Failed;
    return TxStatus::Pending;
}

static bool isTerminal(TxStatus s) {
    return s == TxStatus::Finalized || s == TxStatus::Replaced ||
           s == TxStatus::Evicted || s == TxStatus::Failed;
}

// ── Constructor ────────────────────────────────────────────────────

TransactionTracker::TransactionTracker(const QString& dataDir, RpcClient* rpc, QObject* parent)
    : QObject(parent), dataDir_(dataDir), rpc_(rpc)
{
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(POLL_INTERVAL_MS);
    connect(pollTimer_, &QTimer::timeout, this, &TransactionTracker::onPollTick);
}

void TransactionTracker::setDataDir(const QString& dataDir) {
    dataDir_ = dataDir;
    load();
}

void TransactionTracker::setWalletScope(const QString& walletScope) {
    walletScope_ = walletScope.trimmed();
    load();
}

// ── Persistence ────────────────────────────────────────────────────

QString TransactionTracker::filePath() const {
    const QString scopedComponent = walletScopeFileComponent(walletScope_);
    if (dataDir_.isEmpty() || scopedComponent.isEmpty()) {
        return QString();
    }
    return dataDir_ + QDir::separator() +
           QStringLiteral("tracked_transactions_%1.json").arg(scopedComponent);
}

QString TransactionTracker::legacyFilePath() const {
    if (dataDir_.isEmpty()) {
        return QString();
    }
    return dataDir_ + QDir::separator() + QStringLiteral("tracked_transactions.json");
}

void TransactionTracker::resetRuntimeState() {
    stopPolling();
    pollInFlight_ = false;
    pollGeneration_++;
    activePollGeneration_ = 0;
    pollPhase_ = Idle;
    currentHeight_ = 0;
    mempoolTxids_.clear();
    pendingTxQueries_ = 0;
}

void TransactionTracker::load() {
    resetRuntimeState();
    transactions_.clear();

    const QString scopedPath = filePath();
    if (scopedPath.isEmpty()) {
        Q_EMIT pendingCountChanged(0);
        return;
    }

    bool loaded = loadFromPath(scopedPath);
    const QString legacyPath = legacyFilePath();
    if (!loaded && !walletScope_.isEmpty() && QFile::exists(legacyPath)) {
        loaded = loadFromPath(legacyPath);
        if (loaded) {
            save();
            QFile::remove(legacyPath);
        }
    }

    if (loaded && hasUnfinalizedTransactions()) {
        startPolling();
    }

    Q_EMIT pendingCountChanged(pendingCount());
}

bool TransactionTracker::loadFromPath(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) return false;

    QJsonObject root = doc.object();
    if (root["schema_version"].toInt() != SCHEMA_VERSION) return false;

    QJsonArray arr = root["transactions"].toArray();
    for (const auto& val : arr) {
        QJsonObject o = val.toObject();
        TrackedTransaction tx;
        tx.txid                = o["txid"].toString();
        tx.isIncoming          = o["is_incoming"].toBool();
        tx.amountUna           = o["amount_una"].toVariant().toLongLong();
        tx.address             = o["address"].toString();
        tx.status              = statusFromString(o["status"].toString());
        tx.confirmations       = o["confirmations"].toInt();
        tx.changeAddress       = o["change_address"].toString();
        tx.changeAmountUna     = o["change_amount_una"].toVariant().toLongLong();
        tx.feePaidUna          = o["fee_paid_una"].toVariant().toLongLong();
        tx.mempoolMissingPolls = o["mempool_missing_polls"].toInt();
        tx.lastSeenInMempool   = QDateTime::fromString(o["last_seen_in_mempool"].toString(), Qt::ISODate);
        tx.createdAt           = QDateTime::fromString(o["created_at"].toString(), Qt::ISODate);
        tx.replacedByTxid      = o["replaced_by_txid"].toString();
        tx.statusReason        = o["status_reason"].toString();
        tx.notifiedConfirmations = o["notified_confirmations"].toInt();
        tx.reservationId       = o["reservation_id"].toString();

        for (const auto& v : o["selected_input_outpoints"].toArray())
            tx.selectedInputOutpoints.append(v.toString());

        QJsonObject amountsObj = o["selected_input_amounts_una"].toObject();
        for (auto it = amountsObj.begin(); it != amountsObj.end(); ++it)
            tx.selectedInputAmountsUna[it.key()] = it.value().toVariant().toLongLong();

        transactions_.push_back(tx);
    }

    return true;
}

void TransactionTracker::save() {
    const QString path = filePath();
    if (path.isEmpty()) {
        return;
    }

    QJsonArray arr;
    for (const auto& tx : transactions_) {
        QJsonObject o;
        o["txid"]                  = tx.txid;
        o["is_incoming"]           = tx.isIncoming;
        o["amount_una"]            = tx.amountUna;
        o["address"]               = tx.address;
        o["status"]                = statusToString(tx.status);
        o["confirmations"]         = tx.confirmations;
        o["change_address"]        = tx.changeAddress;
        o["change_amount_una"]     = tx.changeAmountUna;
        o["fee_paid_una"]          = tx.feePaidUna;
        o["mempool_missing_polls"] = tx.mempoolMissingPolls;
        o["last_seen_in_mempool"]  = tx.lastSeenInMempool.toString(Qt::ISODate);
        o["created_at"]            = tx.createdAt.toString(Qt::ISODate);
        o["replaced_by_txid"]      = tx.replacedByTxid;
        o["status_reason"]         = tx.statusReason;
        o["notified_confirmations"] = tx.notifiedConfirmations;
        o["reservation_id"]        = tx.reservationId;

        QJsonArray outpoints;
        for (const auto& op : tx.selectedInputOutpoints)
            outpoints.append(op);
        o["selected_input_outpoints"] = outpoints;

        QJsonObject amounts;
        for (auto it = tx.selectedInputAmountsUna.begin(); it != tx.selectedInputAmountsUna.end(); ++it)
            amounts[it.key()] = it.value();
        o["selected_input_amounts_una"] = amounts;

        arr.append(o);
    }

    QJsonObject root;
    root["schema_version"] = SCHEMA_VERSION;
    root["transactions"] = arr;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();
    }
}

// ── Public API ─────────────────────────────────────────────────────

void TransactionTracker::trackSend(const TrackedTransaction& tx) {
    // Avoid duplicate tracking
    if (findByTxid(tx.txid)) return;

    transactions_.push_back(tx);
    save();
    Q_EMIT transactionUpdated(tx.txid, tx.status);
    Q_EMIT pendingCountChanged(pendingCount());

    // Start polling if we have unfinalized transactions
    if (!pollTimer_->isActive())
        startPolling();
}

int TransactionTracker::pendingCount() const {
    int count = 0;
    for (const auto& tx : transactions_)
        if (!isTerminal(tx.status)) ++count;
    return count;
}

void TransactionTracker::startPolling() {
    if (hasUnfinalizedTransactions())
        pollTimer_->start();
}

void TransactionTracker::stopPolling() {
    pollTimer_->stop();
}

bool TransactionTracker::hasUnfinalizedTransactions() const {
    for (const auto& tx : transactions_)
        if (!isTerminal(tx.status)) return true;
    return false;
}

TrackedTransaction* TransactionTracker::findByTxid(const QString& txid) {
    for (auto& tx : transactions_)
        if (tx.txid == txid) return &tx;
    return nullptr;
}

// ── Polling state machine ──────────────────────────────────────────

void TransactionTracker::onPollTick() {
    if (pollInFlight_) return;  // Non-overlapping guard
    if (!hasUnfinalizedTransactions()) {
        stopPolling();
        return;
    }

    pollInFlight_ = true;
    pollGeneration_++;
    activePollGeneration_ = pollGeneration_;
    pollPhase_ = WaitingBlockCount;

    // Step 1: get current block height
    rpc_->call("getblockcount", QJsonArray());
}

void TransactionTracker::onPollRpcResult(const QString& method, const QJsonValue& result) {
    // Stale generation — ignore
    if (activePollGeneration_ != pollGeneration_) return;

    if (method == "getblockcount" && pollPhase_ == WaitingBlockCount) {
        int height = result.toInt(currentHeight_);
        processBlockCount(height);
    }
    else if (method == "getrawmempool" && pollPhase_ == WaitingMempool) {
        QJsonArray txids;
        if (result.isArray()) {
            txids = result.toArray();
        } else if (result.isObject()) {
            // Some responses wrap in {result: [...]}
            txids = result.toObject()["result"].toArray();
        }
        processRawMempool(txids);
    }
    else if (method == "getrawtransaction" && pollPhase_ == WaitingTxDetails) {
        if (result.isObject()) {
            QJsonObject txData = result.toObject();
            QString txid = txData["txid"].toString();
            if (!txid.isEmpty())
                processRawTransaction(txid, txData);
        }
        pendingTxQueries_--;
        if (pendingTxQueries_ <= 0) {
            // All tx queries complete — poll cycle done
            pollInFlight_ = false;
            pollPhase_ = Idle;
            save();

            if (!hasUnfinalizedTransactions())
                stopPolling();
        }
    }
}

void TransactionTracker::processBlockCount(int height) {
    currentHeight_ = height;
    pollPhase_ = WaitingMempool;

    // Step 2: get raw mempool
    rpc_->call("getrawmempool", QJsonArray());
}

void TransactionTracker::processRawMempool(const QJsonArray& txids) {
    mempoolTxids_.clear();
    for (const auto& v : txids)
        mempoolTxids_.insert(v.toString());

    // Step 3: query each unfinalized tx
    pendingTxQueries_ = 0;
    for (auto& tx : transactions_) {
        if (isTerminal(tx.status)) continue;

        if (mempoolTxids_.contains(tx.txid)) {
            // Tx is in mempool
            if (tx.status == TxStatus::Pending) {
                transitionTo(tx, TxStatus::Mempool);
            }
            tx.mempoolMissingPolls = 0;
            tx.lastSeenInMempool = QDateTime::currentDateTimeUtc();
        } else {
            // Tx not in mempool — check if confirmed or missing
            tx.mempoolMissingPolls++;
        }

        // Query getrawtransaction for confirmation count
        pendingTxQueries_++;
        QJsonArray params;
        params.append(tx.txid);
        params.append(true); // verbose
        rpc_->call("getrawtransaction", params);
    }

    if (pendingTxQueries_ == 0) {
        // No unfinalized transactions needed querying
        pollInFlight_ = false;
        pollPhase_ = Idle;
    } else {
        pollPhase_ = WaitingTxDetails;
    }
}

void TransactionTracker::processRawTransaction(const QString& txid, const QJsonObject& txData) {
    auto* tx = findByTxid(txid);
    if (!tx) return;

    int confirmations = txData["confirmations"].toInt(0);
    tx->confirmations = confirmations;

    if (confirmations > 0) {
        tx->mempoolMissingPolls = 0;
        if (confirmations >= FINALIZATION_CONFIRMATIONS) {
            transitionTo(*tx, TxStatus::Finalized);
        } else if (tx->status != TxStatus::Confirmed) {
            transitionTo(*tx, TxStatus::Confirmed);
        } else {
            // Already Confirmed, just update count — emit for UI
            Q_EMIT transactionUpdated(tx->txid, tx->status);
        }
    } else if (tx->mempoolMissingPolls >= MISSING_POLLS_BEFORE_EVICTION &&
               tx->status != TxStatus::Pending) {
        // Was in mempool, now gone for 2+ polls — check replacement, then evict
        detectReplacement(*tx);
    }
}

// ── Replacement detection (outpoint-first) ─────────────────────────

void TransactionTracker::detectReplacement(TrackedTransaction& tx) {
    if (tx.selectedInputOutpoints.isEmpty()) {
        // No outpoint data — can't detect replacement, just evict
        transitionTo(tx, TxStatus::Evicted, "Disappeared from mempool");
        return;
    }

    // Build set of our outpoints for fast lookup
    QSet<QString> ourOutpoints;
    for (const auto& op : tx.selectedInputOutpoints)
        ourOutpoints.insert(op);

    // Check each mempool txid for outpoint overlap
    // This is O(mempool * inputs) but runs rarely (only on eviction)
    for (const QString& candidateTxid : mempoolTxids_) {
        if (candidateTxid == tx.txid) continue;

        // Check if this candidate is already tracked as the replacement
        if (auto* existing = findByTxid(candidateTxid)) {
            // Already tracking this tx — check outpoint overlap
            for (const auto& op : existing->selectedInputOutpoints) {
                if (ourOutpoints.contains(op)) {
                    tx.replacedByTxid = candidateTxid;
                    transitionTo(tx, TxStatus::Replaced,
                        QString("Replaced by %1 (outpoint overlap)").arg(candidateTxid));
                    return;
                }
            }
        }
        // For non-tracked mempool txids, we'd need getrawtransaction to check
        // inputs. This is deferred — the simple case (our own RBF) is caught
        // above because we track both the original and replacement.
    }

    // No replacement found — evict
    transitionTo(tx, TxStatus::Evicted, "Disappeared from mempool (no replacement found)");
}

// ── State transitions ──────────────────────────────────────────────

void TransactionTracker::transitionTo(TrackedTransaction& tx, TxStatus newStatus, const QString& reason) {
    TxStatus oldStatus = tx.status;
    if (oldStatus == newStatus && reason.isEmpty()) return;

    tx.status = newStatus;
    if (!reason.isEmpty())
        tx.statusReason = reason;

    Q_EMIT transactionUpdated(tx.txid, newStatus);

    // Terminal events feed Phase 5 advisory banners
    if (newStatus == TxStatus::Replaced || newStatus == TxStatus::Evicted ||
        newStatus == TxStatus::Failed) {
        TerminalAdvisoryEvent event;
        event.txid = tx.txid;
        event.status = newStatus;
        event.reason = tx.statusReason;
        event.reservationId = tx.reservationId;
        Q_EMIT terminalEvent(event);
    }

    // Finalization — stop tracking actively
    if (newStatus == TxStatus::Finalized) {
        Q_EMIT pendingCountChanged(pendingCount());
    }

    // Any terminal state — update pending count
    if (isTerminal(newStatus) && !isTerminal(oldStatus)) {
        Q_EMIT pendingCountChanged(pendingCount());
    }
}
