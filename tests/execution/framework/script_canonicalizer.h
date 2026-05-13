#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace dinero {
namespace execution {
namespace test {

/**
 * ScriptCanonicalizer
 *
 * Utility for Phase 7f - S24 (Canonical Equivalence)
 *
 * Generates syntactically different but semantically equivalent scripts
 * to test that execution semantics are independent of syntactic form.
 *
 * Transformations:
 * 1. Arithmetic equivalence: OP_1 OP_1 OP_ADD → OP_2
 * 2. Boolean equivalence: OP_1 OP_IF X OP_ENDIF → X
 * 3. Stack manipulation: OP_DUP OP_DROP → NOP (identity)
 * 4. Dead code: OP_1 <unreachable> → OP_1
 */
class ScriptCanonicalizer {
public:
    /**
     * Generate a semantically equivalent variant of a script
     *
     * @param script Original script
     * @param variant_type Type of transformation to apply
     * @return Equivalent but syntactically different script
     */
    static std::vector<uint8_t> generateEquivalent(
        const std::vector<uint8_t>& script,
        size_t variant_type = 0
    );

    /**
     * Get canonical (simplified) form of a script
     *
     * @param script Original script
     * @return Canonical form
     */
    static std::vector<uint8_t> getCanonicalForm(
        const std::vector<uint8_t>& script
    );

    /**
     * Check if two scripts are semantically equivalent
     * (This is a heuristic check based on known patterns)
     *
     * @param script1 First script
     * @param script2 Second script
     * @return true if likely equivalent
     */
    static bool areEquivalent(
        const std::vector<uint8_t>& script1,
        const std::vector<uint8_t>& script2
    );

private:
    // Transformation helpers

    /**
     * Apply arithmetic simplification
     * OP_1 OP_1 OP_ADD → OP_2
     */
    static std::vector<uint8_t> expandArithmetic(
        const std::vector<uint8_t>& script
    );

    /**
     * Apply identity expansion
     * OP_X → OP_X OP_DUP OP_DROP
     */
    static std::vector<uint8_t> expandIdentity(
        const std::vector<uint8_t>& script
    );

    /**
     * Apply constant folding (reverse of expansion)
     * OP_1 OP_1 OP_ADD → OP_2
     */
    static std::vector<uint8_t> foldConstants(
        const std::vector<uint8_t>& script
    );

    /**
     * Remove no-ops
     * OP_X OP_DUP OP_DROP → OP_X
     */
    static std::vector<uint8_t> removeNoOps(
        const std::vector<uint8_t>& script
    );
};

} // namespace test
} // namespace execution
} // namespace dinero
