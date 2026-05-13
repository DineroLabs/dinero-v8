#include "contracts/commitment_tx_builder.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace dinero {
namespace contracts {

std::optional<Transaction> CommitmentTransactionBuilder::buildCommitmentTransaction(
    const std::vector<uint8_t>& commitment_script,
    const std::string& from_address,
    double fee_per_byte) {
    
    if (!validateCommitmentScript(commitment_script)) {
        g_logger.error("[CommitmentTxBuilder] Invalid commitment script");
        return std::nullopt;
    }
    
    // Create transaction
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;  // SegWit v0 (P2WPKH)
    
    // Build outputs (OP_RETURN output only)
    tx.vout = buildOutputs(commitment_script);
    
    // Note: Inputs would need to be added by wallet system
    // This function creates the transaction structure with OP_RETURN output
    // The wallet system should add inputs and sign the transaction
    
    return tx;
}

uint64_t CommitmentTransactionBuilder::estimateCommitmentFee(
    const std::vector<uint8_t>& commitment_script,
    double fee_per_byte) {
    
    size_t tx_size = calculateTxSize(commitment_script);
    double fee_din = tx_size * fee_per_byte;
    
    // Convert DIN to una (1 DIN = 100,000,000 una)
    constexpr uint64_t DIN_TO_UNA = 100000000ULL;
    uint64_t fee_una = static_cast<uint64_t>(std::ceil(fee_din * DIN_TO_UNA));
    
    return fee_una;
}

bool CommitmentTransactionBuilder::validateCommitmentScript(const std::vector<uint8_t>& script) {
    if (script.empty()) {
        return false;
    }
    
    // Check OP_RETURN opcode
    if (script[0] != 0x6a) {  // OP_RETURN
        return false;
    }
    
    // Check length encoding
    if (script.size() < 2) {
        return false;
    }
    
    uint8_t len_byte = script[1];
    size_t expected_size = 0;
    
    if (len_byte <= 75) {
        expected_size = 2 + len_byte;  // OP_RETURN + length + data
    } else if (len_byte == 0x4c) {  // OP_PUSHDATA1
        if (script.size() < 3) return false;
        expected_size = 3 + script[2];
    } else if (len_byte == 0x4d) {  // OP_PUSHDATA2
        if (script.size() < 4) return false;
        expected_size = 4 + (script[2] | (script[3] << 8));
    } else {
        return false;
    }
    
    return script.size() == expected_size;
}

std::string CommitmentTransactionBuilder::transactionToHex(const Transaction& tx) {
    std::vector<uint8_t> serialized = tx.Serialize();
    
    std::ostringstream hex_oss;
    for (uint8_t byte : serialized) {
        hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    
    return hex_oss.str();
}

size_t CommitmentTransactionBuilder::calculateTxSize(const std::vector<uint8_t>& commitment_script) {
    // Base transaction overhead
    size_t base_size = 10;  // version (4) + locktime (4) + segwit flag (2)
    
    // Input overhead (minimal - 1 input assumed)
    size_t input_size = 40;  // prevout (36) + sequence (4)
    
    // Output size
    size_t output_size = 8 + commitment_script.size();  // value (8) + script length + script
    
    // Witness overhead (empty witness for OP_RETURN)
    size_t witness_size = 2;  // witness count
    
    // Total size (base + inputs + outputs + witness)
    return base_size + input_size + output_size + witness_size;
}

std::vector<TxOutput> CommitmentTransactionBuilder::buildOutputs(
    const std::vector<uint8_t>& commitment_script) {
    
    std::vector<TxOutput> outputs;
    
    // OP_RETURN output (value = 0)
    TxOutput op_return_output;
    op_return_output.value = 0;
    op_return_output.scriptPubKey = commitment_script;
    
    outputs.push_back(op_return_output);
    
    return outputs;
}

} // namespace contracts
} // namespace dinero

