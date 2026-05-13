#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Funding Service Interface (L1↔L2 Boundary)
// ═══════════════════════════════════════════════════════════════════════════
// Defines the interface through which Lightning (L2) requests funding transactions.
//
// ARCHITECTURE:
// - Lightning MUST NOT include wallet/transaction/daemon headers  
// - Lightning requests funding ONLY through this interface
// - Production implementation uses wallet + transaction builder
// - Test implementation uses mocks
//
// This enforces architectural boundary: Lightning depends on interface, not implementation.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <optional>
#include <string>

namespace lightning {

/**
 * Funding transaction result from L1
 */
struct FundingTxResult {
    std::string funding_txid;           // Funding transaction ID (hex)
    uint32_t funding_vout = 0;          // Output index of the funding output
    std::string funding_tx_hex;         // Serialized funding transaction (hex)
    uint64_t funding_amount_sats = 0;   // Actual funding amount in una
    
    // Transaction details
    std::string funding_script_pubkey;  // ScriptPubKey of the funding output (hex)
    uint32_t funding_tx_weight = 0;     // Transaction weight for fee calculation
    uint64_t fee_paid_sats = 0;         // Actual fee paid

    FundingTxResult() = default;
};

/**
 * Interface for Lightning to request funding transactions.
 *
 * Production implementation: Uses wallet + transaction builder
 * Test implementation: MockFundingService with deterministic responses
 *
 * Replaces direct access to:
 * - Wallet::createTransaction()
 * - TransactionBuilder
 * - UTXO selection logic
 */
class IFundingService {
public:
    virtual ~IFundingService() = default;

    // ═══════════════════════════════════════════════════════════════════════
    // Funding Transaction Creation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Create a funding transaction for a Lightning channel.
     *
     * Creates a 2-of-2 multisig output (Taproot or P2WSH depending on protocol version)
     * and returns the transaction details.
     *
     * @param amount_sats Channel funding amount in una
     * @param remote_pubkey Remote peer's funding pubkey (33-byte compressed hex)
     * @param local_pubkey Our funding pubkey (33-byte compressed hex)
     * @param csv_delay CheckSequenceVerify delay for unilateral close (blocks)
     * @param feerate_sat_per_kvb Fee rate in una per kilobyte
     * @return FundingTxResult if successful, std::nullopt if insufficient funds or error
     */
    virtual std::optional<FundingTxResult> createFunding(
        uint64_t amount_sats,
        const std::string& remote_pubkey,
        const std::string& local_pubkey,
        uint32_t csv_delay,
        uint64_t feerate_sat_per_kvb
    ) = 0;

    /**
     * Broadcast a funding transaction to the network.
     *
     * @param tx_hex Serialized transaction (hex)
     * @return true if broadcast succeeded, false otherwise
     */
    virtual bool broadcastFunding(const std::string& tx_hex) = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Fee Estimation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Estimate feerate for funding transaction.
     * @param target_blocks Target confirmation time in blocks
     * @return Estimated feerate in una per kilobyte
     */
    virtual uint64_t estimateFundingFee(uint32_t target_blocks = 6) const = 0;
};

/**
 * Mock implementation for testing.
 * Allows tests to control funding outcomes without running full wallet.
 */
class MockFundingService : public IFundingService {
public:
    MockFundingService() = default;

    // Test configuration
    void setNextFundingResult(const FundingTxResult& result) {
        m_next_result = result;
        m_has_next_result = true;
    }

    void clearNextResult() {
        m_has_next_result = false;
    }

    void setFeeRate(uint64_t feerate) {
        m_feerate = feerate;
    }

    void setShouldFailBroadcast(bool should_fail) {
        m_broadcast_fails = should_fail;
    }

    // Test inspection
    const std::string& lastRemotePubkey() const { return m_last_remote_pubkey; }
    const std::string& lastLocalPubkey() const { return m_last_local_pubkey; }
    uint64_t lastAmount() const { return m_last_amount; }
    int callCount() const { return m_call_count; }

    // IFundingService implementation
    std::optional<FundingTxResult> createFunding(
        uint64_t amount_sats,
        const std::string& remote_pubkey,
        const std::string& local_pubkey,
        uint32_t csv_delay,
        uint64_t feerate_sat_per_kvb
    ) override {
        m_call_count++;
        m_last_amount = amount_sats;
        m_last_remote_pubkey = remote_pubkey;
        m_last_local_pubkey = local_pubkey;
        m_last_csv_delay = csv_delay;
        m_last_feerate = feerate_sat_per_kvb;

        if (m_has_next_result) {
            return m_next_result;
        }

        // Default: return deterministic mock result
        FundingTxResult result;
        result.funding_txid = "mock_funding_txid_" + std::to_string(m_call_count);
        result.funding_vout = 0;
        result.funding_tx_hex = "deadbeef";
        result.funding_amount_sats = amount_sats;
        result.funding_script_pubkey = "mock_script_pubkey";
        result.funding_tx_weight = 1000;
        result.fee_paid_sats = 500;
        return result;
    }

    bool broadcastFunding(const std::string& tx_hex) override {
        m_last_broadcast_tx = tx_hex;
        return !m_broadcast_fails;
    }

    uint64_t estimateFundingFee(uint32_t target_blocks = 6) const override {
        (void)target_blocks;
        return m_feerate;
    }

private:
    // Mock state
    bool m_has_next_result = false;
    FundingTxResult m_next_result;
    uint64_t m_feerate = 1000; // Default: 1000 sat/kvB
    bool m_broadcast_fails = false;

    // Last call tracking
    int m_call_count = 0;
    uint64_t m_last_amount = 0;
    std::string m_last_remote_pubkey;
    std::string m_last_local_pubkey;
    uint32_t m_last_csv_delay = 0;
    uint64_t m_last_feerate = 0;
    std::string m_last_broadcast_tx;
};

} // namespace lightning
