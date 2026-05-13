#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace dinero {

/**
 * @brief Core Transaction structure for Dinero
 * 
 * This is the main transaction structure used throughout the codebase.
 * It provides a complete definition to avoid forward declaration issues.
 */
struct Transaction {
    // Transaction version (typically 2 for Dinero)
    uint32_t version = 2;
    
    // Inputs
    struct Input {
        std::string txid;           // Previous transaction ID
        uint32_t vout = 0;          // Output index
        std::string script_sig;     // Input script
        uint32_t sequence = 0xffffffff; // Sequence number
        
        Input() = default;
        Input(const std::string& tx, uint32_t v) : txid(tx), vout(v) {}
    };
    
    // Outputs
    struct Output {
        uint64_t value = 0;         // Value in una
        std::string script_pubkey;  // Output script
        
        Output() = default;
        Output(uint64_t v, const std::string& script) : value(v), script_pubkey(script) {}
    };
    
    // Transaction data
    std::vector<Input> inputs;
    std::vector<Output> outputs;
    uint32_t locktime = 0;          // Lock time
    
    // Computed fields
    std::string txid;               // Transaction ID (double SHA256)
    uint32_t size = 0;              // Serialized size in bytes
    uint32_t weight = 0;            // Transaction weight (for segwit)
    uint64_t fee = 0;               // Transaction fee
    
    // Constructor
    Transaction() = default;
    
    // Utility methods
    bool isEmpty() const {
        return inputs.empty() && outputs.empty();
    }
    
    uint64_t getTotalInputValue() const {
        uint64_t total = 0;
        for (const auto& input : inputs) {
            // Note: This would need UTXO lookup in real implementation
            total += 0; // Placeholder
        }
        return total;
    }
    
    uint64_t getTotalOutputValue() const {
        uint64_t total = 0;
        for (const auto& output : outputs) {
            total += output.value;
        }
        return total;
    }
    
    // Serialization helpers
    std::string toHex() const;
    static std::optional<Transaction> fromHex(const std::string& hex);
    
    // Validation
    bool isValid() const;
    bool isCoinbase() const {
        return inputs.size() == 1 && inputs[0].txid.empty();
    }
};

} // namespace dinero
