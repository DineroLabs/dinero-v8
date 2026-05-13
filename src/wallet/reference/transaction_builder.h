#ifndef DINEROCOIN_WALLET_REFERENCE_TRANSACTION_BUILDER_H
#define DINEROCOIN_WALLET_REFERENCE_TRANSACTION_BUILDER_H

#include "wallet.h"
#include <vector>
#include <string>
#include <cstdint>

namespace dinero {
namespace wallet {
namespace reference {

/**
 * Transaction Builder
 *
 * Responsibilities:
 * 1. Build deterministic transactions
 * 2. Sign transactions with private key
 * 3. Ensure byte-identical output for same inputs
 *
 * Guarantees:
 * - Same inputs + same fee = byte-identical transaction
 * - Output order: [recipient, change] (never shuffled)
 * - No RBF, no replace-by-fee
 * - Explicit fees only
 */
class TransactionBuilder {
public:
    /**
     * Constructor
     * @param private_key Wallet's private key (32 bytes)
     * @param public_key Wallet's public key (33 bytes compressed)
     * @param address Wallet's address (for change output)
     */
    TransactionBuilder(
        const std::vector<uint8_t>& private_key,
        const std::vector<uint8_t>& public_key,
        const std::string& address
    );

    ~TransactionBuilder();

    /**
     * Build and sign transaction
     *
     * Transaction structure (deterministic):
     * - Version: 2
     * - Inputs: Sorted by (txid, vout) - as provided by UTXO manager
     * - Outputs:
     *   1. Recipient output (index 0)
     *   2. Change output (index 1) - only if change > dust threshold
     * - Locktime: 0
     * - Witness: SegWit v0 signatures
     *
     * @param inputs Selected UTXOs (already sorted)
     * @param to_address Recipient address
     * @param amount Amount to send (una)
     * @param fee Transaction fee (una)
     * @return Signed transaction hex and txid
     */
    struct BuildResult {
        std::string txid;          // Transaction ID (hex)
        std::string hex;           // Signed transaction (hex)
        uint64_t total_input;      // Sum of input amounts
        uint64_t total_output;     // Sum of output amounts
        uint64_t change_amount;    // Change amount (0 if no change)
    };

    BuildResult BuildTransaction(
        const std::vector<UTXO>& inputs,
        const std::string& to_address,
        uint64_t amount,
        uint64_t fee
    );

    /**
     * Estimate transaction size (vbytes)
     * @param num_inputs Number of inputs
     * @param num_outputs Number of outputs (1 or 2)
     * @return Estimated virtual bytes
     */
    static uint64_t EstimateSize(size_t num_inputs, size_t num_outputs);

    /**
     * Dust threshold (minimum output value)
     * @return Minimum una for a valid output
     */
    static uint64_t GetDustThreshold() {
        return 546;  // Standard Bitcoin dust threshold
    }

private:
    std::vector<uint8_t> private_key_;
    std::vector<uint8_t> public_key_;
    std::string change_address_;

    /**
     * Create script pubkey for P2WPKH address
     * @param address Bech32 address
     * @return Script pubkey (hex)
     */
    std::string CreateScriptPubKey(const std::string& address);

    /**
     * Sign input with SegWit v0 signature
     * @param tx_hex Unsigned transaction hex
     * @param input_index Input to sign
     * @param utxo UTXO being spent
     * @return Witness stack (signature + pubkey)
     */
    std::vector<std::vector<uint8_t>> SignInput(
        const std::string& tx_hex,
        size_t input_index,
        const UTXO& utxo
    );

    /**
     * Calculate transaction hash (txid)
     * @param tx_hex Signed transaction hex
     * @return Transaction ID (hex, reversed)
     */
    std::string CalculateTxid(const std::string& tx_hex);

    /**
     * Serialize transaction (deterministic)
     * @param inputs Input UTXOs
     * @param outputs Output amounts and addresses
     * @return Unsigned transaction hex
     */
    std::string SerializeTransaction(
        const std::vector<UTXO>& inputs,
        const std::vector<std::pair<std::string, uint64_t>>& outputs
    );

    /**
     * Add witnesses to transaction
     * @param tx_hex Unsigned transaction hex
     * @param witnesses Witness stacks for each input
     * @return Signed transaction hex (with witness data)
     */
    std::string AddWitnesses(
        const std::string& tx_hex,
        const std::vector<std::vector<std::vector<uint8_t>>>& witnesses
    );
};

} // namespace reference
} // namespace wallet
} // namespace dinero

#endif // DINEROCOIN_WALLET_REFERENCE_TRANSACTION_BUILDER_H
