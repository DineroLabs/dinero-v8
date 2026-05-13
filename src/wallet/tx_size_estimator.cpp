#include "wallet/tx_size_estimator.h"
#include <algorithm>
#include <cmath>

namespace din {

uint64_t TxSizeEstimator::estimateVBytes(
    const std::vector<InputSpec>& inputs,
    const std::vector<OutputSpec>& outputs
) {
    if (inputs.empty()) {
        return 0; // Invalid transaction
    }
    
    bool has_segwit = hasSegwitInputs(inputs);
    
    uint64_t base_size = calculateBaseSize(inputs, outputs, has_segwit);
    uint64_t total_size = calculateTotalSize(inputs, outputs, has_segwit);
    
    // Virtual bytes calculation: (base_size * 3 + total_size) / 4
    return (base_size * 3 + total_size + 3) / 4; // +3 for ceiling division
}

int64_t TxSizeEstimator::calculateFee(uint64_t vbytes, double feerate_sat_per_vb) {
    if (vbytes == 0 || feerate_sat_per_vb <= 0.0) {
        return 0;
    }
    
    // Always round up to ensure sufficient fee
    double fee_exact = static_cast<double>(vbytes) * feerate_sat_per_vb;
    return static_cast<int64_t>(std::ceil(fee_exact));
}

uint64_t TxSizeEstimator::getInputVBytes(InputType type) {
    switch (type) {
        case InputType::P2WPKH:
            return P2WPKH_INPUT_VBYTES;
        case InputType::P2PKH:
            return P2PKH_INPUT_VBYTES;
        case InputType::P2SH:
            return P2SH_INPUT_VBYTES;
        case InputType::P2TR:
            return P2TR_INPUT_VBYTES;
    }
    return 0; // Should never reach here
}

uint64_t TxSizeEstimator::getOutputBytes(OutputType type) {
    switch (type) {
        case OutputType::P2WPKH:
            return P2WPKH_OUTPUT_BYTES;
        case OutputType::P2PKH:
            return P2PKH_OUTPUT_BYTES;
        case OutputType::P2SH:
            return P2SH_OUTPUT_BYTES;
        case OutputType::P2TR:
            return P2TR_OUTPUT_BYTES;
    }
    return 0; // Should never reach here
}

bool TxSizeEstimator::hasSegwitInputs(const std::vector<InputSpec>& inputs) {
    return std::any_of(inputs.begin(), inputs.end(), [](const InputSpec& input) {
        return input.type == InputType::P2WPKH || input.type == InputType::P2TR;
    });
}

std::pair<uint64_t, uint64_t> TxSizeEstimator::estimateWithChange(
    const std::vector<InputSpec>& inputs,
    const std::vector<OutputSpec>& outputs,
    OutputType change_type
) {
    uint64_t size_without_change = estimateVBytes(inputs, outputs);
    
    // Create outputs with change added
    std::vector<OutputSpec> outputs_with_change = outputs;
    outputs_with_change.emplace_back(change_type, 0); // Value doesn't affect size
    
    uint64_t size_with_change = estimateVBytes(inputs, outputs_with_change);
    
    return {size_without_change, size_with_change};
}

// Private implementation methods

uint64_t TxSizeEstimator::calculateBaseSize(
    const std::vector<InputSpec>& inputs,
    const std::vector<OutputSpec>& outputs,
    bool has_segwit
) {
    uint64_t size = TX_OVERHEAD_BYTES;
    
    // Add input sizes (without witness data)
    for (const auto& input : inputs) {
        switch (input.type) {
            case InputType::P2WPKH:
                // Base: outpoint(36) + scriptSigLen(1) + scriptSig(0) + sequence(4) = 41
                size += 41;
                break;
            case InputType::P2PKH:
                // Base: outpoint(36) + scriptSigLen(1) + scriptSig(107) + sequence(4) = 148
                size += 148;
                break;
            case InputType::P2SH:
                // Base: outpoint(36) + scriptSigLen(1) + scriptSig(~50) + sequence(4) = ~91
                size += 91;
                break;
            case InputType::P2TR:
                // Base: outpoint(36) + scriptSigLen(1) + scriptSig(0) + sequence(4) = 41
                size += 41;
                break;
        }
    }
    
    // Add output sizes (same for base and total)
    for (const auto& output : outputs) {
        size += getOutputBytes(output.type);
    }
    
    return size;
}

uint64_t TxSizeEstimator::calculateTotalSize(
    const std::vector<InputSpec>& inputs,
    const std::vector<OutputSpec>& outputs,
    bool has_segwit
) {
    uint64_t size = calculateBaseSize(inputs, outputs, has_segwit);
    
    if (has_segwit) {
        // Add segwit overhead
        size += SEGWIT_OVERHEAD_BYTES;
        
        // Add witness data for segwit inputs
        for (const auto& input : inputs) {
            if (input.type == InputType::P2WPKH) {
                // P2WPKH witness: items_count(1) + signature_len(1) + signature(~72) + pubkey_len(1) + pubkey(33) = ~108
                size += 108; // Full witness size for P2WPKH
            } else if (input.type == InputType::P2TR) {
                // P2TR witness: items_count(1) + signature(64) = 65 (key path spending)
                size += 65; // Key path spending witness
            }
        }
    }
    
    return size;
}

} // namespace din
