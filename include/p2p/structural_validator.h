/**
 * Phase G.3.2: Structural Validation (Cheap, Pure)
 *
 * Validates payload structure and internal consistency.
 *
 * Design Principles:
 * - Purely functional (no state, no side effects)
 * - Deserialize + check internal consistency
 * - NO script execution
 * - NO UTXO lookups
 * - NO consensus context
 * - NO chainstate access
 * - NO disk I/O
 *
 * What This IS:
 * - Deserialization validation
 * - Size limit enforcement
 * - Basic structural invariants
 * - Merkle root verification (blocks)
 * - Duplicate input detection (txs)
 *
 * What This Is NOT:
 * - Script verification
 * - UTXO validation
 * - Locktime semantics
 * - Coinbase subsidy checks
 * - State mutation
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dinero {
struct Transaction;
}

namespace dinero {
namespace p2p {

//=============================================================================
// Validation Result
//=============================================================================

struct StructuralValidationResult {
    bool ok;
    std::string error;

    StructuralValidationResult() : ok(true), error("") {}

    static StructuralValidationResult Ok() {
        return StructuralValidationResult();
    }

    static StructuralValidationResult Fail(const std::string& err) {
        StructuralValidationResult result;
        result.ok = false;
        result.error = err;
        return result;
    }
};

//=============================================================================
// StructuralValidator: Pure Structural Validation
//=============================================================================

class StructuralValidator {
public:
    StructuralValidator() = default;

    // Validate block structure
    StructuralValidationResult validateBlock(const std::vector<uint8_t>& raw);

    // Validate transaction structure
    StructuralValidationResult validateTx(const std::vector<uint8_t>& raw);

private:
    StructuralValidationResult validateParsedTx(const dinero::Transaction& tx) const;

    // Size limits (Bitcoin-compatible)
    static constexpr size_t MAX_BLOCK_SIZE = 4 * 1024 * 1024;  // 4MB
    static constexpr size_t MAX_TX_SIZE = 100000;               // 100KB (consensus/limits.h)
    static constexpr size_t MIN_BLOCK_HEADER_SIZE = 128;        // Dinero BlockHeader v1
};

} // namespace p2p
} // namespace dinero
