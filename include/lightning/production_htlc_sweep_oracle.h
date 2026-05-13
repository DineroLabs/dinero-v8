#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Production HTLC Sweep Oracle (L1→L2 Adapter - Phase 7B)
// ═══════════════════════════════════════════════════════════════════════════
// Builds and broadcasts HTLC sweep transactions for ChannelManagerCore.
//
// ARCHITECTURE:
// - L2 (ChannelManagerCore) decides WHAT and WHEN to sweep (policy)
// - L1 (ProductionHTLCSweepOracle) decides HOW to sweep (execution)
// - Wraps wallet API, transaction builder, and mempool for sweep operations
//
// SWEEP TYPES:
// - TIMEOUT: Outgoing HTLCs after CLTV expiry (claim back our funds)
// - SUCCESS: Incoming HTLCs with preimage (claim their funds)
//
// CSV/CLTV CONSTRAINTS:
// - CSV delay: Relative timelock (wait N blocks after commitment confirmation)
// - CLTV expiry: Absolute timelock (wait until specific block height)
// - Sweep transaction sequence must satisfy CSV requirements (BIP68)
//
// USAGE:
//   auto sweep_oracle = std::make_shared<ProductionHTLCSweepOracle>(
//       wallet_api, daemon_ctx
//   );
//   auto core = std::make_unique<ChannelManagerCore>(
//       chain_oracle, wallet_oracle, funding_service, sweep_oracle, ...
//   );
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/htlc_sweep_oracle.h"
#include "lightning/lightning_db_types.h"
#include "daemon/daemon_context.h"
#include "wallet/dinero_wallet_api.h"
#include "primitives/transaction.h"  // Transaction class

namespace dinero {
namespace lightning {

/**
 * Production implementation of IHTLCSweepOracle.
 * Builds HTLC sweep transactions using wallet API and broadcasts to mempool.
 */
class ProductionHTLCSweepOracle : public ::lightning::IHTLCSweepOracle {
public:
    /**
     * Constructor
     * @param wallet_api Pointer to IWalletAPI implementation (NOT owned)
     * @param daemon_ctx Reference to DaemonContext for mempool/chainstate access
     */
    ProductionHTLCSweepOracle(
        wallet::IWalletAPI* wallet_api,
        DaemonContext& daemon_ctx
    );

    ~ProductionHTLCSweepOracle() override = default;

    // Disable copy and move
    ProductionHTLCSweepOracle(const ProductionHTLCSweepOracle&) = delete;
    ProductionHTLCSweepOracle& operator=(const ProductionHTLCSweepOracle&) = delete;
    ProductionHTLCSweepOracle(ProductionHTLCSweepOracle&&) = delete;
    ProductionHTLCSweepOracle& operator=(ProductionHTLCSweepOracle&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IHTLCSweepOracle Implementation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Build and broadcast an HTLC sweep transaction.
     *
     * Process:
     * 1. Extract sweep parameters from HTLCSweepRecord
     * 2. Build transaction input (HTLC output from commitment tx)
     * 3. Build transaction output (wallet address - sweep fee)
     * 4. Set sequence number to satisfy CSV constraint (BIP68)
     * 5. Build witness script (HTLC timeout/success path)
     * 6. Sign transaction with appropriate keys
     * 7. Broadcast to mempool
     *
     * @param sweep Sweep record with all parameters
     * @return Transaction ID if broadcast succeeded, empty string otherwise
     */
    std::string broadcastSweep(
        const dinero::lightning::HTLCSweepRecord& sweep
    ) override;

    /**
     * Check if a sweep transaction has been confirmed.
     *
     * Queries chainstate to check if sweep txid is in a confirmed block.
     *
     * @param sweep_txid Transaction ID from broadcastSweep()
     * @return Block height if confirmed, std::nullopt if unconfirmed
     */
    std::optional<uint64_t> getSweepConfirmationHeight(
        const std::string& sweep_txid
    ) const override;

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Calculate fee for sweep transaction
     * @param input_amount Input amount in muna
     * @return uint64_t Fee in muna (0.1% of input, min 1000 muna)
     */
    uint64_t calculateSweepFee(uint64_t input_amount) const;

    /**
     * Get wallet address for sweep output
     * @return std::string Bech32 address or empty if failed
     */
    std::string getSweepDestinationAddress() const;

    /**
     * Build sweep transaction from sweep record
     * @param sweep Sweep record
     * @param destination_address Wallet address for output
     * @param current_height Current block height for eligibility checks
     * @return Transaction or nullopt if failed
     */
    std::optional<Transaction> buildSweepTransaction(
        const dinero::lightning::HTLCSweepRecord& sweep,
        const std::string& destination_address,
        uint64_t current_height
    ) const;

    /**
     * Sign sweep transaction with HTLC witness
     * @param tx Transaction to sign (modified in place)
     * @param sweep Sweep record with HTLC details
     * @param is_timeout true for timeout path, false for success path
     * @return bool true if signing succeeded
     */
    bool signSweepTransaction(
        Transaction& tx,
        const dinero::lightning::HTLCSweepRecord& sweep,
        bool is_timeout
    ) const;

    /**
     * Build HTLC witness stack for sweep transaction
     * @param sweep Sweep record with HTLC details
     * @param signature BIP340 Schnorr signature (64 bytes)
     * @param is_timeout true for timeout path, false for success path
     * @return Witness stack elements
     */
    std::vector<std::vector<uint8_t>> buildHTLCWitnessStack(
        const dinero::lightning::HTLCSweepRecord& sweep,
        const std::vector<uint8_t>& signature,
        bool is_timeout
    ) const;

    /**
     * Parse hex string to bytes
     * @param hex Hex string
     * @return Byte vector
     */
    std::vector<uint8_t> hexToBytes(const std::string& hex) const;

    // Dependencies (NOT owned)
    wallet::IWalletAPI* m_wallet_api;
    DaemonContext& m_daemon_ctx;
};

} // namespace lightning
} // namespace dinero
