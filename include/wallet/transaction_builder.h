#pragma once

#include "wallet/utxo_index.h"
#include "wallet/canonical_wallet_utxo.h"  // Phase M.3: Canonical UTXO type
// #include "wallet/coin_selection.h"  // TODO: Reintegrate with new interface system
#include "wallet/bip143_signer.h"
#include "wallet/taproot_tx_signer.h"
#include "wallet/transaction.h"
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace dinero {

/**
 * Transaction builder for creating and signing P2WPKH transactions
 */
class TransactionBuilder {
public:
    struct Recipient {
        std::string address;
        int64_t amount;  // una
    };

    struct BuildOptions {
        double fee_rate = 1.0;  // sat/vB
        bool subtract_fee_from_amount = false;
        std::optional<std::string> change_address;
        int64_t dust_threshold = 546;  // una

        // B1a: RBF control
        bool enable_rbf = true;  // Default true (nSequence = 0xfffffffe)

        // B1b: Absolute timelock (sets tx.lockTime to this block height)
        std::optional<uint32_t> absolute_locktime;

        // B1b: Per-input relative timelocks (BIP68 nSequence encoding)
        // Map: input_index -> relative_locktime_blocks
        std::map<uint32_t, uint32_t> relative_locktime_blocks;

        // Explicit candidate UTXOs: when non-empty, bypass UTXOIndex lookup and use
        // only these UTXOs for coin selection (already filtered by caller).
        std::vector<CanonicalWalletUTXO> candidate_utxos;
    };

    struct BuildResult {
        bool success = false;
        std::string error;
        Transaction transaction;
        int64_t fee = 0;
        int64_t change_amount = 0;
        std::string change_address;
        std::vector<CanonicalWalletUTXO> selected_utxos;  // Phase M.3: Canonical UTXO type
        std::vector<std::string> required_private_keys;

        // B1a+B1b: Timelock/RBF metadata
        bool is_rbf_enabled = false;
        bool has_absolute_timelock = false;
        bool has_relative_timelocks = false;
    };

    explicit TransactionBuilder(UTXOIndex* utxo_index);

    /**
     * Preview a transaction without signing
     * @param recipients List of recipients
     * @param options Build options
     * @return Build result with transaction details
     */
    BuildResult PreviewTransaction(
        const std::vector<Recipient>& recipients,
        const BuildOptions& options
    );

    /**
     * Preview a transaction without signing (with default options)
     * @param recipients List of recipients
     * @return Build result with transaction details
     */
    BuildResult PreviewTransaction(
        const std::vector<Recipient>& recipients
    );

    /**
     * Build and sign a complete transaction
     * @param recipients List of recipients
     * @param private_keys Map of address -> private key (hex)
     * @param options Build options
     * @return Build result with signed transaction
     */
    BuildResult BuildTransaction(
        const std::vector<Recipient>& recipients,
        const std::map<std::string, std::string>& private_keys,
        const BuildOptions& options
    );

    /**
     * Build and sign a complete transaction (with default options)
     * @param recipients List of recipients
     * @param private_keys Map of address -> private key (hex)
     * @return Build result with signed transaction
     */
    BuildResult BuildTransaction(
        const std::vector<Recipient>& recipients,
        const std::map<std::string, std::string>& private_keys
    );

    /**
     * Estimate transaction fee
     * @param num_inputs Number of inputs
     * @param num_outputs Number of outputs
     * @param fee_rate Fee rate in sat/vB
     * @return Estimated fee in una
     */
    static int64_t EstimateFee(int num_inputs, int num_outputs, double fee_rate);

    /**
     * Validate a Bech32 address
     * @param address Address to validate
     * @param expected_hrp Expected HRP (e.g., "din")
     * @return true if valid P2WPKH address
     */
    static bool ValidateAddress(const std::string& address, const std::string& expected_hrp = "din");

    /**
     * Convert address to scriptPubKey
     * @param address Bech32 address
     * @return scriptPubKey bytes, empty if invalid
     */
    static std::vector<uint8_t> AddressToScriptPubKey(const std::string& address);

    /**
     * Generate a change address (placeholder - should integrate with HD wallet)
     * @return Bech32 change address
     */
    std::string GenerateChangeAddress();

private:
    /**
     * Build the transaction structure without signing
     * @param recipients List of recipients
     * @param selected_utxos Selected UTXOs for inputs
     * @param change_amount Change amount
     * @param change_address Change address
     * @return Unsigned transaction
     */
    Transaction BuildUnsignedTransaction(
        const std::vector<Recipient>& recipients,
        const std::vector<CanonicalWalletUTXO>& selected_utxos,  // Phase M.3: Canonical UTXO type
        int64_t change_amount,
        const std::string& change_address,
        const BuildOptions& options
    );

    /**
     * Get private key for a UTXO (placeholder - should integrate with HD wallet)
     * @param utxo UTXO to get key for
     * @param private_keys Available private keys
     * @return Private key hex, empty if not found
     */
    std::string GetPrivateKeyForUTXO(
        const CanonicalWalletUTXO& utxo,  // Phase M.3: Canonical UTXO type
        const std::map<std::string, std::string>& private_keys
    );

    UTXOIndex* utxo_index_;
    // TODO: Integrate with new coin selection interfaces
    // CoinSelector coin_selector_;
    // FeeEstimator fee_estimator_;
};

} // namespace dinero
