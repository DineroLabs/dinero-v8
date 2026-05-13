#pragma once

#include "lightning/lightning_db_types.h"
#include "common/status.h"
#include <vector>
#include <string>
#include <optional>
#include <memory>

namespace dinero {
namespace lightning {

// Forward declarations
class IDBTransaction;

// Record types defined in lightning_db_types.h

// Result type for operations that can fail
template<typename T>
struct Result {
    bool success;
    T value;
    std::string error;

    static Result<T> Ok(T val) {
        return Result<T>{true, std::move(val), ""};
    }

    static Result<T> Err(std::string err) {
        return Result<T>{false, T{}, std::move(err)};
    }

    bool isOk() const { return success; }
    bool isErr() const { return !success; }
    const T& unwrap() const { return value; }
    T& unwrap() { return value; }
    const std::string& err() const { return error; }
};

// Specialization for void results
template<>
struct Result<void> {
    bool success;
    std::string error;

    static Result<void> Ok() {
        return Result<void>{true, ""};
    }

    static Result<void> Err(std::string err) {
        return Result<void>{false, std::move(err)};
    }

    bool isOk() const { return success; }
    bool isErr() const { return !success; }
    const std::string& err() const { return error; }
};

/**
 * @class ILightningDB
 * @brief Abstract interface for Lightning Network state persistence
 *
 * Allows multiple backend implementations (RocksDB, SQLite, etc.)
 * while maintaining a common interface for Lightning components.
 */
class ILightningDB {
public:
    virtual ~ILightningDB() = default;

    // Database status
    virtual bool isOpen() const = 0;

    // Channel operations
    virtual Status putChannel(const ChannelRecord& rec) = 0;
    virtual std::optional<ChannelRecord> getChannel(const std::string& id) = 0;
    virtual Status deleteChannel(const std::string& id) = 0;
    virtual std::vector<ChannelRecord> listChannels() = 0;

    // HTLC operations
    virtual Status putHTLC(const HTLCRecord& rec) = 0;
    virtual std::optional<HTLCRecord> getHTLC(const std::string& id) = 0;
    virtual Status deleteHTLC(const std::string& id) = 0;
    virtual std::vector<HTLCRecord> listHTLCsForChannel(const std::string& channel_id) = 0;

    // Commitment operations
    virtual Status putCommitment(const CommitmentRecord& rec) = 0;
    virtual std::optional<CommitmentRecord> getCommitment(const std::string& id) = 0;
    virtual std::vector<CommitmentRecord> listCommitmentsForChannel(const std::string& channel_id) = 0;

    // Peer operations
    virtual Status putPeer(const PeerRecord& rec) = 0;
    virtual std::optional<PeerRecord> getPeer(const std::string& node_id) = 0;
    virtual std::vector<PeerRecord> listPeers() = 0;

    // Invoice operations
    virtual Status putInvoice(const InvoiceRecord& rec) = 0;
    virtual std::optional<InvoiceRecord> getInvoice(const std::string& payment_hash) = 0;
    virtual Status deleteInvoice(const std::string& payment_hash) = 0;
    virtual std::vector<InvoiceRecord> listInvoices() = 0;

    // Payment operations
    virtual Status putPayment(const PaymentRecord& rec) = 0;
    virtual std::optional<PaymentRecord> getPayment(const std::string& payment_hash) = 0;
    virtual Status deletePayment(const std::string& payment_hash) = 0;
    virtual std::vector<PaymentRecord> listPayments() = 0;

    // HTLC Sweep operations (Phase 7B: Recovery After Force-Close)
    virtual Status putHTLCSweep(const HTLCSweepRecord& rec) = 0;
    virtual std::optional<HTLCSweepRecord> getHTLCSweep(const std::string& sweep_id) = 0;
    virtual Status deleteHTLCSweep(const std::string& sweep_id) = 0;
    virtual std::vector<HTLCSweepRecord> listHTLCSweepsForChannel(const std::string& channel_id) = 0;
    virtual std::vector<HTLCSweepRecord> listPendingHTLCSweeps() = 0; // All sweeps not yet confirmed

    // Justice operations (Phase 7C: Breach Detection & Punishment)
    virtual Status putJustice(const JusticeRecord& rec) = 0;
    virtual std::optional<JusticeRecord> getJustice(const std::string& justice_id) = 0;
    virtual Status deleteJustice(const std::string& justice_id) = 0;
    virtual std::vector<JusticeRecord> listJusticeForChannel(const std::string& channel_id) = 0;
    virtual std::vector<JusticeRecord> listPendingJustice() = 0; // All justice not yet confirmed

    // Atomic batch operations
    virtual Status atomicUpdateChannelHTLC(const ChannelRecord& chan, const HTLCRecord& htlc) = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 9: Watchtower & Transaction Confirmation Tracking
    // ═══════════════════════════════════════════════════════════════════════

    // Watchtower operations
    virtual Status putRevokedCommitment(const RevokedCommitmentRecord& rec) = 0;
    virtual std::optional<RevokedCommitmentRecord> getRevokedCommitment(const std::string& commitment_txid) = 0;
    virtual std::optional<RevokedCommitmentRecord> getRevokedCommitmentByTxId(const std::string& txid) = 0;
    virtual std::vector<RevokedCommitmentRecord> listRevokedCommitmentsForChannel(const std::string& channel_id) = 0;
    virtual Status deleteRevokedCommitment(const std::string& commitment_txid) = 0;
    virtual size_t pruneOldRevokedCommitments(uint64_t cutoff_timestamp) = 0;

    // Transaction confirmation tracking (idempotency)
    virtual bool isTxConfirmed(const std::string& txid) = 0;
    virtual Status markTxConfirmed(const std::string& txid, uint64_t height) = 0;

    // Fast lookups for onTransactionConfirmed
    virtual std::optional<HTLCSweepRecord> getSweepByTxId(const std::string& txid) = 0;
    virtual std::optional<ChannelRecord> getChannelByFundingTx(const std::string& txid) = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Transaction Support (Phase 6: Crash Safety)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Begin atomic transaction for multi-operation writes
     *
     * Usage:
     * @code
     *   auto tx = db->beginTransaction();
     *   if (!tx) { return Status::Internal; }
     *
     *   tx->putChannel(channel);
     *   tx->putHTLC(htlc);
     *   tx->commit();  // Atomic: both or neither
     * @endcode
     *
     * Transaction creation failure is an L1 concern (resource exhaustion, etc.)
     * L2 logic simply checks for nullptr and surfaces failure.
     *
     * @return Transaction handle or nullptr on failure
     */
    virtual std::unique_ptr<IDBTransaction> beginTransaction() = 0;

    /**
     * @brief Flush all pending writes to disk
     *
     * Forces fsync to ensure durability. Call before shutdown or
     * at periodic checkpoints.
     *
     * @return Status::Ok if flush succeeded
     */
    virtual Status flush() = 0;
};

} // namespace lightning
} // namespace dinero
