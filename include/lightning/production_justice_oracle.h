#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Production Justice Oracle (L1→L2 Adapter - Phase 7C)
// ═══════════════════════════════════════════════════════════════════════════
// Builds and broadcasts justice (penalty) transactions after breach detection.
//
// ARCHITECTURE:
// - L2 (ChannelManagerCore) decides WHEN justice is required (breach detection)
// - L1 (ProductionJusticeOracle) decides HOW to punish (TX building, signing)
// - Wraps wallet API, commitment builder, and mempool for justice operations
//
// JUSTICE PATH:
// 1. Counterparty broadcasts revoked commitment
// 2. ChannelManagerCore detects breach (txid != latest commitment)
// 3. Creates JusticeRecord with revocation_secret
// 4. Waits for CSV delay (to_self_delay blocks)
// 5. buildJusticeTransaction() - claims all outputs with revocation key
// 6. broadcastJusticeTransaction() - time-critical mempool submission
//
// KEY DIFFERENCE FROM HTLC SWEEP (Phase 7B):
// - Justice transactions are TIME-CRITICAL (race against counterparty)
// - Therefore: build + sign integrated (atomic operation)
// - HTLC sweeps have timelock protection → can separate build/sign
//
// USAGE:
//   auto justice_oracle = std::make_shared<ProductionJusticeOracle>(
//       wallet_api, daemon_ctx, commitment_builder
//   );
//   auto core = std::make_unique<ChannelManagerCore>(
//       chain_oracle, wallet_oracle, funding_service, sweep_oracle, justice_oracle, ...
//   );
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/justice_oracle.h"
#include "lightning/lightning_db_types.h"
#include "lightning/commitment_builder.h"
#include "daemon/daemon_context.h"
#include "wallet/dinero_wallet_api.h"
#include "primitives/transaction.h"

namespace dinero {
namespace lightning {

/**
 * Production implementation of IJusticeOracle.
 * Builds justice transactions using revocation keys and broadcasts to mempool.
 */
class ProductionJusticeOracle : public ::lightning::IJusticeOracle {
public:
    /**
     * Constructor
     * @param wallet_api Pointer to IWalletAPI implementation (NOT owned)
     * @param daemon_ctx Reference to DaemonContext for mempool/chainstate access
     * @param commitment_builder CommitmentBuilder for revocation key derivation
     */
    ProductionJusticeOracle(
        wallet::IWalletAPI* wallet_api,
        DaemonContext& daemon_ctx,
        CommitmentBuilder& commitment_builder
    );

    ~ProductionJusticeOracle() override = default;

    // Disable copy and move
    ProductionJusticeOracle(const ProductionJusticeOracle&) = delete;
    ProductionJusticeOracle& operator=(const ProductionJusticeOracle&) = delete;
    ProductionJusticeOracle(ProductionJusticeOracle&&) = delete;
    ProductionJusticeOracle& operator=(ProductionJusticeOracle&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IJusticeOracle Implementation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Build and sign a justice transaction to punish breach.
     *
     * Process:
     * 1. Parse revocation secret from JusticeRecord
     * 2. Query revoked commitment transaction from chain
     * 3. Identify claimable outputs (to_local, to_remote, HTLCs)
     * 4. Build transaction inputs with CSV sequence numbers
     * 5. Build single output sweeping all funds to wallet
     * 6. Derive revocation private key
     * 7. Sign transaction (script-path for revocation branch)
     * 8. Return fully signed justice transaction
     *
     * @param justice Justice record with revocation secret and timing
     * @param channel Channel record with commitment parameters
     * @return JusticeTx if build succeeded, error otherwise
     */
    Result<::lightning::JusticeTx> buildJusticeTransaction(
        const dinero::lightning::JusticeRecord& justice,
        const dinero::lightning::ChannelRecord& channel
    ) override;

    /**
     * Broadcast justice transaction to mempool.
     *
     * Time-critical operation - must confirm before counterparty claims funds.
     *
     * @param tx Justice transaction to broadcast
     * @return Status::Ok if broadcast succeeded, error otherwise
     */
    dinero::Status broadcastJusticeTransaction(
        const ::lightning::JusticeTx& tx
    ) override;

    /**
     * Check if a justice transaction has been confirmed.
     *
     * Queries chainstate to check if justice txid is in a confirmed block.
     *
     * @param justice_txid Transaction ID from buildJusticeTransaction()
     * @return Block height if confirmed, std::nullopt if unconfirmed
     */
    std::optional<uint64_t> getJusticeConfirmationHeight(
        const std::string& justice_txid
    ) const override;

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Claimable output descriptor
     */
    struct ClaimableOutput {
        std::string txid;                  // Transaction ID (hex)
        uint32_t vout;                     // Output index
        uint64_t amount;                   // Amount in muna
        bool needs_revocation_key;         // true = revocation branch, false = our key
        uint32_t csv_delay;                // CSV delay blocks (0 = no delay)
        std::vector<uint8_t> scriptPubKey; // Output script
    };

    /**
     * Calculate fee for justice transaction (aggressive for fast confirmation)
     * @param total_input_value Total input amount in muna
     * @return uint64_t Fee in muna (1% of input, min 10000 muna)
     */
    uint64_t calculateJusticeFee(uint64_t total_input_value) const;

    /**
     * Get wallet address for justice output
     * @return std::string Bech32 address or empty if failed
     */
    std::string getJusticeDestinationAddress() const;

    /**
     * Query revoked commitment transaction from chain
     * @param commitment_txid Transaction ID (hex)
     * @return Transaction or nullopt if not found
     */
    std::optional<Transaction> getCommitmentTransaction(
        const std::string& commitment_txid
    ) const;

    /**
     * Identify claimable outputs from revoked commitment
     * @param revoked_commit Revoked commitment transaction
     * @param channel Channel record with output metadata
     * @return Vector of claimable outputs
     */
    std::vector<ClaimableOutput> identifyClaimableOutputs(
        const Transaction& revoked_commit,
        const dinero::lightning::ChannelRecord& channel,
        const std::string& commitment_txid
    ) const;

    /**
     * Build justice transaction from claimable outputs
     * @param claimable_outputs Outputs to claim
     * @param destination_address Wallet address for sweep
     * @param to_self_delay CSV delay for outputs
     * @return Transaction or nullopt if failed
     */
    std::optional<Transaction> buildJusticeTransactionFromOutputs(
        const std::vector<ClaimableOutput>& claimable_outputs,
        const std::string& destination_address,
        uint32_t to_self_delay
    ) const;

    /**
     * Sign justice transaction with revocation and wallet keys
     * @param tx Transaction to sign (modified in place)
     * @param claimable_outputs Output descriptors
     * @param revocation_privkey Revocation private key (32 bytes)
     * @param channel Channel record with key metadata
     * @return bool true if signing succeeded
     */
    bool signJusticeTransaction(
        Transaction& tx,
        const std::vector<ClaimableOutput>& claimable_outputs,
        const std::vector<uint8_t>& revocation_privkey,
        const dinero::lightning::ChannelRecord& channel
    ) const;

    /**
     * Sign input with revocation key (script-path spend)
     * @param tx Transaction being signed
     * @param input_index Index of input to sign
     * @param output Output descriptor
     * @param revocation_privkey Revocation private key (32 bytes)
     * @param channel Channel record
     * @return bool true if signing succeeded
     */
    bool signRevocationInput(
        Transaction& tx,
        size_t input_index,
        const ClaimableOutput& output,
        const std::vector<uint8_t>& revocation_privkey,
        const dinero::lightning::ChannelRecord& channel
    ) const;

    /**
     * Sign input with wallet key (key-path spend)
     * @param tx Transaction being signed
     * @param input_index Index of input to sign
     * @param output Output descriptor
     * @return bool true if signing succeeded
     */
    bool signKeyPathInput(
        Transaction& tx,
        size_t input_index,
        const ClaimableOutput& output
    ) const;

    /**
     * Build witness stack for revocation branch
     * @param signature BIP340 Schnorr signature (64 bytes)
     * @param revocation_script Revocation script
     * @param control_block Taproot control block
     * @return Witness stack elements
     */
    std::vector<std::vector<uint8_t>> buildRevocationWitnessStack(
        const std::vector<uint8_t>& signature,
        const std::vector<uint8_t>& revocation_script,
        const std::vector<uint8_t>& control_block
    ) const;

    /**
     * Parse hex string to bytes
     * @param hex Hex string
     * @return Byte vector
     */
    std::vector<uint8_t> hexToBytes(const std::string& hex) const;

    /**
     * Encode CSV delay as BIP68 sequence number
     * @param blocks Number of blocks
     * @return uint32_t BIP68 sequence number
     */
    uint32_t encodeCSV(uint32_t blocks) const;

    // Dependencies (NOT owned)
    wallet::IWalletAPI* m_wallet_api;
    DaemonContext& m_daemon_ctx;
    CommitmentBuilder& m_commitment_builder;
};

} // namespace lightning
} // namespace dinero
