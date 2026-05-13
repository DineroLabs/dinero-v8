#pragma once

/**
 * @file unsigned_tx_builder.h
 * @brief Milestone 12.4 - Unsigned Transaction Construction
 *
 * Pure, side-effect-free transaction building.
 * No wallet mutation, no mempool calls, no private keys.
 *
 * Design Rule: Unsigned transaction building must be deterministic
 * and have zero side effects. This enables:
 * - Hardware wallet integration (offline signing)
 * - PSBT export (Partially Signed Bitcoin Transactions)
 * - Transaction batching
 * - Clean separation from mempool layer
 */

#include "wallet/canonical_wallet_utxo.h"  // Phase M.3: CanonicalWalletUTXO
#include "wallet/transaction.h"
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Output Request (User Intent)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Requested transaction output (payment destination)
 */
struct TxOutputRequest {
    std::string address;      // Recipient address (Bech32)
    uint64_t amount;          // Amount to send (una)

    TxOutputRequest() : amount(0) {}
    TxOutputRequest(const std::string& addr, uint64_t amt)
        : address(addr), amount(amt) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Unsigned Transaction Result
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Unsigned transaction (no signatures, no witness data)
 *
 * Contains complete transaction structure ready for signing.
 * Explicitly separated from signed transactions to prevent confusion.
 */
struct UnsignedTransaction {
    Transaction tx;                                    // Unsigned tx (vin + vout only)
    uint64_t fee;                                      // Total fee (una)
    uint64_t change_amount;                            // Change output amount (0 if no change)
    std::string change_address;                        // Change address (empty if no change)
    std::vector<CanonicalWalletUTXO> selected_utxos;  // Phase M.3: CanonicalWalletUTXO
    bool signals_rbf;                                  // RBF enabled (sequence < 0xfffffffe)

    UnsignedTransaction()
        : fee(0), change_amount(0), signals_rbf(false) {}
};

/**
 * @brief Result of unsigned transaction building
 */
struct BuildResult {
    bool success;
    std::string error;
    UnsignedTransaction unsigned_tx;

    BuildResult() : success(false) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Build Options
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Options for transaction construction
 */
struct BuildOptions {
    uint64_t fee_rate;                      // Fee rate (una per vbyte)
    bool enable_rbf;                        // Signal RBF (BIP125)
    std::string change_address;             // Custom change address (optional)
    uint64_t dust_threshold;                // Minimum output value (default: 546)
    uint32_t locktime;                      // Transaction locktime (default: 0)

    BuildOptions()
        : fee_rate(1)
        , enable_rbf(true)   // RBF enabled by default (Bitcoin Core standard)
        , dust_threshold(546)
        , locktime(0)
    {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Unsigned Transaction Builder
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Pure transaction builder (no side effects)
 *
 * Milestone 12.4: Clean separation between building and signing.
 *
 * Responsibilities:
 * - Construct transaction structure (vin, vout)
 * - Calculate fees via size estimation
 * - Create change output if needed
 * - Set sequence numbers (RBF signaling)
 *
 * Does NOT:
 * - Sign transactions (see TransactionSigner - Milestone 12.5)
 * - Submit to mempool (see MempoolAdapter - Milestone 12.6)
 * - Select coins (see CoinSelector - Milestone 12.3)
 * - Mutate wallet state
 */
class UnsignedTxBuilder {
public:
    /**
     * @brief Build unsigned transaction from selected UTXOs
     *
     * Pure function: No wallet mutation, no mempool calls, no private keys.
     *
     * @param selected_utxos UTXOs selected for spending (from CoinSelector)
     * @param outputs Requested payment outputs
     * @param options Build options (fee rate, RBF, etc.)
     * @return Build result with unsigned transaction
     */
    static BuildResult Build(
        const std::vector<CanonicalWalletUTXO>& selected_utxos,  // Phase M.3: CanonicalWalletUTXO
        const std::vector<TxOutputRequest>& outputs,
        const BuildOptions& options
    );

    /**
     * @brief Estimate transaction size (vbytes)
     *
     * SegWit P2WPKH transaction size formula.
     *
     * @param num_inputs Number of inputs
     * @param num_outputs Number of outputs
     * @return Estimated vsize (virtual bytes)
     */
    static size_t EstimateTransactionSize(size_t num_inputs,
                                          size_t num_outputs,
                                          size_t num_p2mr_inputs = 0);

    /**
     * @brief Calculate fee for transaction size
     *
     * @param tx_size Transaction size (vbytes)
     * @param fee_rate Fee rate (una per vbyte)
     * @return Total fee (una)
     */
    static uint64_t CalculateFee(size_t tx_size, uint64_t fee_rate);

    // Dust threshold (546 una for P2WPKH)
    static constexpr uint64_t DUST_THRESHOLD = 546;

    // RBF sequence number (BIP125: < 0xfffffffe signals replaceability)
    static constexpr uint32_t RBF_SEQUENCE = 0xfffffffd;

    // Default sequence (no RBF)
    static constexpr uint32_t DEFAULT_SEQUENCE = 0xfffffffe;

private:
    /**
     * @brief Convert address to scriptPubKey
     *
     * @param address Bech32 address
     * @return scriptPubKey bytes (empty if invalid)
     */
    static std::vector<uint8_t> AddressToScriptPubKey(const std::string& address);

    /**
     * @brief Check if change output should be created
     *
     * Change is omitted if:
     * - Amount < dust threshold (546 una)
     * - Amount would create uneconomical output
     *
     * @param change_amount Potential change amount
     * @param dust_threshold Minimum output value
     * @return true if change output should be created
     */
    static bool ShouldCreateChangeOutput(uint64_t change_amount, uint64_t dust_threshold);
};

} // namespace dinero
