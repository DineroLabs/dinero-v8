#pragma once

#include "lightning/lightning_db_types.h"
#include "common/status.h"
#include <string>

namespace dinero {
namespace lightning {

/**
 * @class IDBTransaction
 * @brief Interface for atomic multi-operation database transactions
 *
 * Provides ACID guarantees for Lightning state updates that span multiple
 * records. All operations are buffered until commit() is called.
 *
 * Usage Pattern:
 * @code
 *   auto tx_result = db->beginTransaction();
 *   if (tx_result.isErr()) { handle_error(); }
 *
 *   auto& tx = tx_result.unwrap();
 *   tx->putChannel(updated_channel);
 *   tx->deleteHTLC(htlc_id);
 *
 *   auto commit_result = tx->commit();
 *   if (commit_result != Status::Ok) {
 *       // Transaction failed - no changes applied
 *   }
 * @endcode
 *
 * Thread Safety: Each transaction is bound to one thread. Do NOT share
 *                across threads.
 *
 * Atomicity: ALL operations succeed or ALL fail. No partial writes.
 */
class IDBTransaction {
public:
    virtual ~IDBTransaction() = default;

    // ═══════════════════════════════════════════════════════════════════════
    // Write Operations (Buffered until commit)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Add channel write to transaction
     * @param rec Channel record to write
     * @return Status::Ok if buffered successfully
     */
    virtual Status putChannel(const ChannelRecord& rec) = 0;

    /**
     * @brief Add HTLC write to transaction
     * @param rec HTLC record to write
     * @return Status::Ok if buffered successfully
     */
    virtual Status putHTLC(const HTLCRecord& rec) = 0;

    /**
     * @brief Add commitment write to transaction
     * @param rec Commitment record to write
     * @return Status::Ok if buffered successfully
     */
    virtual Status putCommitment(const CommitmentRecord& rec) = 0;

    /**
     * @brief Add HTLC sweep write to transaction (Phase 7B)
     * @param rec HTLC sweep record to write
     * @return Status::Ok if buffered successfully
     */
    virtual Status putHTLCSweep(const HTLCSweepRecord& rec) = 0;

    /**
     * @brief Add justice record write to transaction (Phase 7C)
     * @param rec Justice record to write
     * @return Status::Ok if buffered successfully
     */
    virtual Status putJustice(const JusticeRecord& rec) = 0;

    /**
     * @brief Add channel deletion to transaction
     * @param channel_id Channel ID to delete
     * @return Status::Ok if buffered successfully
     */
    virtual Status deleteChannel(const std::string& channel_id) = 0;

    /**
     * @brief Add HTLC deletion to transaction
     * @param htlc_id HTLC ID to delete
     * @return Status::Ok if buffered successfully
     */
    virtual Status deleteHTLC(const std::string& htlc_id) = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Transaction Control
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Atomically commit all buffered operations
     *
     * This is the ONLY method that actually writes to disk.
     * Either ALL operations succeed or NONE do.
     *
     * RocksDB Implementation:
     * - Uses WriteBatch for atomicity
     * - Forces fsync with WriteOptions::sync = true
     * - Guarantees durability (survives crash after commit)
     *
     * @return Status::Ok if all operations committed successfully
     *         Status::Io if write failed (transaction rolled back)
     */
    virtual Status commit() = 0;

    /**
     * @brief Discard all buffered operations
     *
     * After rollback, the transaction cannot be reused.
     * Create a new transaction if needed.
     */
    virtual void rollback() = 0;

    /**
     * @brief Check if transaction has been committed or rolled back
     * @return true if transaction is still active (can accept operations)
     */
    virtual bool isActive() const = 0;
};

} // namespace lightning
} // namespace dinero
