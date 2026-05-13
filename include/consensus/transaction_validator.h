#pragma once

#include "primitives/transaction.h"
#include "primitives/amount.h"  // Phase M.6.1: Monetary type safety
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace consensus {

// Forward declaration
class IUTXOProvider;

} // namespace consensus

// Bitcoin-style transaction validation
class TransactionValidator {
public:
    // Phase M.6.1: total_fee converted to AmountUna for type safety
    struct ValidationResult {
        bool valid;
        std::string error;
        AmountUna total_fee;
    };

    // Validate a transaction before accepting into mempool
    // Takes IUTXOProvider interface - works with consensus or wallet adapters
    static ValidationResult ValidateTransaction(
        const Transaction& tx,
        consensus::IUTXOProvider* utxo_provider,
        uint32_t current_height
    );

    // F.8.5: Per-input signature verification (for parallel validation)
    // Canonical verification engine - used by both serial and parallel paths
    // Integrates with caches (F.8.3 + F.8.4)
    struct InputVerificationResult {
        bool valid;
        std::string error;
    };

    static InputVerificationResult VerifyInput(
        const Transaction& tx,
        size_t input_index,
        const std::vector<uint8_t>& scriptPubKey,
        uint64_t value,
        uint32_t script_flags,
        uint32_t height,
        uint64_t median_time_past,  // BIP113: For time-based CHECKLOCKTIMEVERIFY
        // BIP341: Full UTXO context required for Taproot sighash (all inputs' amounts and scripts)
        const std::vector<uint64_t>& all_input_amounts = {},
        const std::vector<std::vector<uint8_t>>& all_input_scriptpubkeys = {},
        const std::vector<uint8_t>& all_input_confidential_flags = {},
        const std::vector<std::vector<uint8_t>>& all_input_commitments = {}
    );

private:
    // Individual validation checks
    static bool CheckStructure(const Transaction& tx, std::string& error);
    static bool CheckInputsExist(const Transaction& tx, consensus::IUTXOProvider* utxo_provider, uint32_t current_height, std::string& error);
    static bool CheckNoDoubleSpend(const Transaction& tx, consensus::IUTXOProvider* utxo_provider, std::string& error);
    static bool VerifySignatures(const Transaction& tx, consensus::IUTXOProvider* utxo_provider, std::string& error);
    // Phase M.6.1: fee parameter changed to AmountUna&
    static bool CheckFees(const Transaction& tx, consensus::IUTXOProvider* utxo_provider, AmountUna& fee, std::string& error);

    // Constants
    static constexpr uint64_t MIN_TX_FEE = 100;  // 100 una minimum fee
    static constexpr uint64_t MAX_TX_SIZE = 100000;  // 100KB max
    static constexpr size_t MAX_TX_SIGOPS = 4000;
};

} // namespace dinero
