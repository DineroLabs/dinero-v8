#pragma once
#include <cstdint>
#include <vector>

namespace din {

/**
 * @brief Deterministic transaction size estimator
 * 
 * Provides Bitcoin Core-accurate vbyte calculations for fee estimation.
 * All calculations use deterministic constants - no randomness or heuristics.
 * 
 * Virtual bytes (vbytes) calculation:
 * vbytes = (base_size * 3 + total_size) / 4
 * 
 * Where:
 * - base_size = size without witness data
 * - total_size = size with witness data
 */
class TxSizeEstimator {
public:
    // Input size constants (vbytes)
    static constexpr uint64_t P2WPKH_INPUT_VBYTES = 68;  // P2WPKH input
    static constexpr uint64_t P2PKH_INPUT_VBYTES = 148;  // P2PKH input
    static constexpr uint64_t P2SH_INPUT_VBYTES = 91;    // P2SH input (avg)
    static constexpr uint64_t P2TR_INPUT_VBYTES = 58;    // P2TR input (key path)
    static constexpr uint64_t P2TR_SCRIPT_INPUT_VBYTES = 58; // P2TR script path (min)
    
    // Output size constants (bytes, same as vbytes)
    static constexpr uint64_t P2WPKH_OUTPUT_BYTES = 31;  // P2WPKH output
    static constexpr uint64_t P2PKH_OUTPUT_BYTES = 34;    // P2PKH output
    static constexpr uint64_t P2SH_OUTPUT_BYTES = 32;     // P2SH output
    static constexpr uint64_t P2TR_OUTPUT_BYTES = 43;     // P2TR output
    
    // Transaction overhead (bytes)
    static constexpr uint64_t TX_OVERHEAD_BYTES = 10;    // version(4) + locktime(4) + in_count(1) + out_count(1)
    static constexpr uint64_t SEGWIT_OVERHEAD_BYTES = 2; // witness marker(1) + flag(1)

    /**
     * @brief Input type for size calculation
     */
    enum class InputType {
        P2WPKH,  // Pay-to-Witness-PubkeyHash
        P2PKH,   // Pay-to-PubkeyHash  
        P2SH,    // Pay-to-Script-Hash
        P2TR     // Pay-to-Taproot
    };
    
    /**
     * @brief Output type for size calculation
     */
    enum class OutputType {
        P2WPKH,  // Pay-to-Witness-PubkeyHash
        P2PKH,   // Pay-to-PubkeyHash
        P2SH,    // Pay-to-Script-Hash
        P2TR     // Pay-to-Taproot
    };
    
    /**
     * @brief Input specification for size estimation
     */
    struct InputSpec {
        InputType type;
        
        InputSpec(InputType t) : type(t) {}
    };
    
    /**
     * @brief Output specification for size estimation
     */
    struct OutputSpec {
        OutputType type;
        int64_t value_sats;  // Value in una (for dust calculations)
        
        OutputSpec(OutputType t, int64_t value) : type(t), value_sats(value) {}
    };

public:
    /**
     * @brief Estimate transaction size in virtual bytes
     * 
     * @param inputs Vector of input specifications
     * @param outputs Vector of output specifications
     * @return Estimated size in virtual bytes
     */
    static uint64_t estimateVBytes(
        const std::vector<InputSpec>& inputs,
        const std::vector<OutputSpec>& outputs
    );
    
    /**
     * @brief Calculate fee for given size and fee rate
     * 
     * @param vbytes Transaction size in virtual bytes
     * @param feerate_sat_per_vb Fee rate in una per virtual byte
     * @return Required fee in una (always rounded up)
     */
    static int64_t calculateFee(uint64_t vbytes, double feerate_sat_per_vb);
    
    /**
     * @brief Get input size for specific type
     * 
     * @param type Input type
     * @return Size in virtual bytes
     */
    static uint64_t getInputVBytes(InputType type);
    
    /**
     * @brief Get output size for specific type
     * 
     * @param type Output type  
     * @return Size in bytes (same as vbytes for outputs)
     */
    static uint64_t getOutputBytes(OutputType type);
    
    /**
     * @brief Check if transaction uses segwit inputs
     * 
     * @param inputs Vector of input specifications
     * @return True if any input is segwit type
     */
    static bool hasSegwitInputs(const std::vector<InputSpec>& inputs);
    
    /**
     * @brief Estimate size with change output consideration
     * 
     * Helps with coin selection by estimating size both with and without change.
     * 
     * @param inputs Vector of input specifications
     * @param outputs Vector of output specifications (without change)
     * @param change_type Type of change output if needed
     * @return Pair of (size_without_change, size_with_change) in vbytes
     */
    static std::pair<uint64_t, uint64_t> estimateWithChange(
        const std::vector<InputSpec>& inputs,
        const std::vector<OutputSpec>& outputs,
        OutputType change_type = OutputType::P2WPKH
    );

private:
    // Internal calculation helpers
    static uint64_t calculateBaseSize(
        const std::vector<InputSpec>& inputs,
        const std::vector<OutputSpec>& outputs,
        bool has_segwit
    );
    
    static uint64_t calculateTotalSize(
        const std::vector<InputSpec>& inputs,
        const std::vector<OutputSpec>& outputs,
        bool has_segwit
    );
};

} // namespace din
