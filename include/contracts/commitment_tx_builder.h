#pragma once

#include "contracts/commitment_builder.h"
#include "wallet/transaction.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace dinero {
namespace contracts {

// Commitment transaction builder
// Creates and broadcasts commitment transactions with OP_RETURN scripts
class CommitmentTransactionBuilder {
public:
    // Build commitment transaction
    // Creates a transaction with OP_RETURN output containing commitment data
    struct CommitmentTxResult {
        std::string txid;
        std::string raw_tx_hex;
        std::vector<uint8_t> commitment_script;
        uint64_t fee_amount;  // Estimated fee in una
        
        CommitmentTxResult() : fee_amount(0) {}
    };
    
    // Build commitment transaction
    // Parameters:
    //   commitment_script: OP_RETURN script bytes from CommitmentTransactionBuilder
    //   from_address: Address to fund the transaction from
    //   fee_per_byte: Fee rate in DIN per byte
    // Returns: Transaction ready for signing and broadcast
    static std::optional<Transaction> buildCommitmentTransaction(
        const std::vector<uint8_t>& commitment_script,
        const std::string& from_address,
        double fee_per_byte = 0.00001  // Default 0.00001 DIN per byte
    );
    
    // Estimate commitment transaction fee
    // Returns: Estimated fee in una
    static uint64_t estimateCommitmentFee(
        const std::vector<uint8_t>& commitment_script,
        double fee_per_byte = 0.00001
    );
    
    // Validate commitment script
    // Checks that script is valid OP_RETURN format
    static bool validateCommitmentScript(const std::vector<uint8_t>& script);
    
    // Convert transaction to hex for broadcast
    static std::string transactionToHex(const Transaction& tx);
    
private:
    // Helper: Calculate transaction size
    static size_t calculateTxSize(const std::vector<uint8_t>& commitment_script);
    
    // Helper: Build transaction outputs
    static std::vector<TxOutput> buildOutputs(
        const std::vector<uint8_t>& commitment_script
    );
};

}} // namespace dinero::contracts

