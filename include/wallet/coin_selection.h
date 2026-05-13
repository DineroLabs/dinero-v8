#pragma once

#include "wallet/transaction.h"
#include "wallet/canonical_wallet_utxo.h"  // Phase M.3: Use canonical UTXO type
#include <vector>
#include <string>

namespace dinero {

// Coin selection algorithm for building transactions

struct CoinSelectionResult {
    std::vector<CanonicalWalletUTXO> selected_coins;  // Phase M.3: Canonical UTXO type
    uint64_t total_value;
    uint64_t change_amount;
    uint64_t fee;
    bool success;
    std::string error;

    CoinSelectionResult() : total_value(0), change_amount(0), fee(0), success(false) {}
};

class CoinSelector {
public:
    // Select coins to cover target amount + fee
    // Returns selected UTXOs and change amount
    // Milestone 12.3: Now tries BnB first for exact match, then least-waste
    // best-fit selection to minimize unnecessary value locked in change.
    // Phase M.3: Uses CanonicalWalletUTXO
    static CoinSelectionResult SelectCoins(
        const std::vector<CanonicalWalletUTXO>& available_utxos,
        uint64_t target_amount,
        uint64_t fee_rate,  // una per vbyte
        size_t num_outputs
    );

    // Estimate transaction size (for fee calculation)
    //
    // num_p2mr_inputs counts inputs whose witness is a canonical ML-DSA-65
    // P2MR spend — each adds ~5273 bytes of witness (1952 pubkey + 3309 sig
    // + framing) on top of the 41-byte non-witness part, vs the 66-byte
    // witness a plain P2TR input contributes. Without this, the fee
    // estimator undercounts a P2MR-spending tx by >5 KB and the mempool
    // rejects the submission as below min-feerate.
    static size_t EstimateTransactionSize(
        size_t num_inputs,
        size_t num_outputs,
        size_t num_p2mr_inputs = 0
    );

    // Calculate fee for a transaction
    static uint64_t CalculateFee(size_t tx_size, uint64_t fee_rate);

    // Dust threshold (546 una for P2WPKH)
    static constexpr uint64_t DUST_THRESHOLD = 546;

    // Milestone 12.3: Branch-and-Bound max iterations (prevent exponential blowup)
    static constexpr size_t BNB_MAX_TRIES = 100000;

private:
    // Milestone 12.3: Branch-and-Bound algorithm for exact match (no change output)
    // Finds combination where sum(selected) == target + fee exactly
    // Returns empty result if no exact match found
    // Phase M.3: Uses CanonicalWalletUTXO
    static CoinSelectionResult SelectCoinsBnB(
        const std::vector<CanonicalWalletUTXO>& available_utxos,
        uint64_t target_amount,
        uint64_t estimated_fee
    );

    // Least-waste best-fit algorithm: minimize overage above target + fee,
    // then minimize number of inputs for the same total.
    // Phase M.3: Uses CanonicalWalletUTXO
    static CoinSelectionResult SelectCoinsLeastWaste(
        const std::vector<CanonicalWalletUTXO>& available_utxos,
        uint64_t target_amount,
        uint64_t estimated_fee
    );

    // Branch-and-Bound search cap for least-waste search.
    static constexpr size_t LEAST_WASTE_MAX_TRIES = 100000;

    // Milestone 12.3: Privacy-aware coin selection
    // Prefers UTXOs from same address to avoid linking addresses
    // Phase M.3: Uses CanonicalWalletUTXO (path field instead of address)
    static std::vector<CanonicalWalletUTXO> ApplyPrivacyHeuristics(
        const std::vector<CanonicalWalletUTXO>& utxos
    );
};

} // namespace dinero
