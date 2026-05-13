#pragma once

#include "lightning/lightning_db_interface.h"
#include "lightning/db_transaction.h"
#include "lightning/lightning_db_types.h"  // For record struct definitions
#include "common/status.h"
#include <sqlite3.h>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <mutex>

namespace dinero {
namespace lightning {

// Note: ChannelRecord, HTLCRecord, CommitmentRecord, PeerRecord, InvoiceRecord
// are defined in lightning_db_types.h and shared between both DB implementations

/**
 * @class SQLiteLightningDB
 * @brief SQLite-backed Lightning Network state storage (per-wallet)
 *
 * Architecture:
 * - Uses separate lightning.db file per wallet
 * - Stores all Lightning state in wallets/{name}/lightning.db
 * - Enables true per-wallet Lightning isolation
 * - Separate from on-chain wallet.db (clear separation of concerns)
 *
 * Tables Used (from lightning_schema.sql):
 * - ln_channels: Channel records
 * - ln_htlcs: HTLC records
 * - ln_commitments: Commitment transaction records
 * - ln_peers: Peer information
 * - ln_invoices: Invoice records
 * - ln_payments: Payment history
 * - ln_secrets: Encrypted revocation secrets
 * - ln_watchtower: Breach detection metadata
 *
 * Thread Safety: All public methods are thread-safe via internal mutex
 */
class SQLiteLightningDB : public ILightningDB {
public:
    /**
     * @brief Construct SQLiteLightningDB with path to lightning.db
     * @param db_path Path to lightning.db file (e.g., "~/.dinero/wallets/default/lightning.db")
     */
    explicit SQLiteLightningDB(const std::string& db_path);

    /**
     * @brief Destructor - closes lightning.db
     */
    ~SQLiteLightningDB();

    // Disable copy and move
    SQLiteLightningDB(const SQLiteLightningDB&) = delete;
    SQLiteLightningDB& operator=(const SQLiteLightningDB&) = delete;
    SQLiteLightningDB(SQLiteLightningDB&&) noexcept = delete;
    SQLiteLightningDB& operator=(SQLiteLightningDB&&) noexcept = delete;

    // ═══════════════════════════════════════════════════════════════════════════
    // Database Lifecycle
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Initialize lightning.db (create schema if needed)
     * @return Status::Ok if initialization successful
     */
    Status initialize();

    /**
     * @brief Verify Lightning tables exist in lightning.db
     * @return Status::Ok if all ln_* tables are present
     */
    Status verifySchema();

    /**
     * @brief Check if database is valid
     */
    bool isOpen() const override { return db_ != nullptr; }

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Store or update a channel record
     */
    Status putChannel(const ChannelRecord& rec) override;

    /**
     * @brief Retrieve a channel record by ID
     */
    std::optional<ChannelRecord> getChannel(const std::string& id) override;

    /**
     * @brief Delete a channel record
     */
    Status deleteChannel(const std::string& id) override;

    /**
     * @brief List all channels
     */
    std::vector<ChannelRecord> listChannels() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // HTLC Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Store or update an HTLC record
     */
    Status putHTLC(const HTLCRecord& rec) override;

    /**
     * @brief Retrieve an HTLC record by ID
     */
    std::optional<HTLCRecord> getHTLC(const std::string& id) override;

    /**
     * @brief Delete an HTLC record
     */
    Status deleteHTLC(const std::string& id) override;

    /**
     * @brief List all HTLCs for a specific channel
     */
    std::vector<HTLCRecord> listHTLCsForChannel(const std::string& channel_id) override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Commitment Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Store a commitment transaction record
     */
    Status putCommitment(const CommitmentRecord& rec) override;

    /**
     * @brief Retrieve a commitment record by ID
     */
    std::optional<CommitmentRecord> getCommitment(const std::string& id) override;

    /**
     * @brief List all commitments for a specific channel
     */
    std::vector<CommitmentRecord> listCommitmentsForChannel(const std::string& channel_id) override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Peer Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Store or update a peer record
     */
    Status putPeer(const PeerRecord& rec) override;

    /**
     * @brief Retrieve a peer record by node ID
     */
    std::optional<PeerRecord> getPeer(const std::string& node_id) override;

    /**
     * @brief List all peers
     */
    std::vector<PeerRecord> listPeers() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Invoice Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Store or update an invoice record
     */
    Status putInvoice(const InvoiceRecord& rec) override;

    /**
     * @brief Retrieve an invoice record by payment hash
     */
    std::optional<InvoiceRecord> getInvoice(const std::string& payment_hash) override;

    /**
     * @brief Delete an invoice record
     */
    Status deleteInvoice(const std::string& payment_hash) override;

    /**
     * @brief List all invoices
     */
    std::vector<InvoiceRecord> listInvoices() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Payment Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Store or update a payment record
     */
    Status putPayment(const PaymentRecord& rec) override;

    /**
     * @brief Retrieve a payment record by payment hash
     */
    std::optional<PaymentRecord> getPayment(const std::string& payment_hash) override;

    /**
     * @brief Delete a payment record
     */
    Status deletePayment(const std::string& payment_hash) override;

    /**
     * @brief List all payments
     */
    std::vector<PaymentRecord> listPayments() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 7B: HTLC Sweep Operations (Stubs - not yet implemented)
    // ═══════════════════════════════════════════════════════════════════════════

    Status putHTLCSweep(const HTLCSweepRecord& rec) override;
    std::optional<HTLCSweepRecord> getHTLCSweep(const std::string& sweep_id) override;
    Status deleteHTLCSweep(const std::string& sweep_id) override;
    std::vector<HTLCSweepRecord> listHTLCSweepsForChannel(const std::string& channel_id) override;
    std::vector<HTLCSweepRecord> listPendingHTLCSweeps() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 7C/9: Revoked Commitment Operations (Stubs - not yet implemented)
    // ═══════════════════════════════════════════════════════════════════════════

    Status putRevokedCommitment(const RevokedCommitmentRecord& rec) override;
    std::optional<RevokedCommitmentRecord> getRevokedCommitment(const std::string& commitment_txid) override;
    std::optional<RevokedCommitmentRecord> getRevokedCommitmentByTxId(const std::string& txid) override;
    std::vector<RevokedCommitmentRecord> listRevokedCommitmentsForChannel(const std::string& channel_id) override;
    Status deleteRevokedCommitment(const std::string& commitment_txid) override;
    size_t pruneOldRevokedCommitments(uint64_t cutoff_timestamp) override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 9: Transaction Confirmation Tracking (Stubs - not yet implemented)
    // ═══════════════════════════════════════════════════════════════════════════

    bool isTxConfirmed(const std::string& txid) override;
    Status markTxConfirmed(const std::string& txid, uint64_t height) override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 9: Additional Query Methods (Stubs - not yet implemented)
    // ═══════════════════════════════════════════════════════════════════════════

    std::optional<HTLCSweepRecord> getSweepByTxId(const std::string& txid) override;
    std::optional<ChannelRecord> getChannelByFundingTx(const std::string& txid) override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Transactions (Stubs - not yet implemented)
    // ═══════════════════════════════════════════════════════════════════════════

    std::unique_ptr<IDBTransaction> beginTransaction() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Database Lifecycle (Part 2)
    // ═══════════════════════════════════════════════════════════════════════════

    Status flush() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Atomic Batch Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Atomically update channel and add HTLC
     *
     * Example use case: Adding new HTLC requires updating channel balance.
     * Both operations must succeed or fail together.
     *
     * @param chan Updated channel record
     * @param htlc New HTLC record
     * @return Status::Ok if both operations succeeded atomically
     */
    Status atomicUpdateChannelHTLC(const ChannelRecord& chan, const HTLCRecord& htlc) override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 7C: Justice Operations (Stubs - not yet implemented)
    // ═══════════════════════════════════════════════════════════════════════════

    Status putJustice(const JusticeRecord& rec) override;
    std::optional<JusticeRecord> getJustice(const std::string& justice_id) override;
    Status deleteJustice(const std::string& justice_id) override;
    std::vector<JusticeRecord> listJusticeForChannel(const std::string& channel_id) override;
    std::vector<JusticeRecord> listPendingJustice() override;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    std::string db_path_;          // Path to lightning.db
    sqlite3* db_;                  // Lightning database pointer (owned by this class)
    mutable std::mutex db_mutex_;  // Thread safety

    // ═══════════════════════════════════════════════════════════════════════════
    // Helper Methods
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Execute SQL statement without result
     */
    Status execSQL(const std::string& sql);

    /**
     * @brief Prepare SQL statement
     */
    Status prepareStatement(const std::string& sql, sqlite3_stmt** stmt);
};

} // namespace lightning
} // namespace dinero
