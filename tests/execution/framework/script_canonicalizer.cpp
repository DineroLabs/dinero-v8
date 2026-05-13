#include "script_canonicalizer.h"

namespace dinero {
namespace execution {
namespace test {

// Opcode constants (from Bitcoin script)
static const uint8_t OP_0 = 0x00;
static const uint8_t OP_1 = 0x51;
static const uint8_t OP_2 = 0x52;
static const uint8_t OP_3 = 0x53;
static const uint8_t OP_4 = 0x54;
static const uint8_t OP_5 = 0x55;
static const uint8_t OP_DUP = 0x76;
static const uint8_t OP_DROP = 0x75;
static const uint8_t OP_ADD = 0x93;
static const uint8_t OP_SUB = 0x94;

std::vector<uint8_t> ScriptCanonicalizer::generateEquivalent(
    const std::vector<uint8_t>& script,
    size_t variant_type
) {
    // Apply different transformations based on variant_type
    switch (variant_type % 3) {
        case 0:
            // Expand arithmetic: OP_2 → OP_1 OP_1 OP_ADD
            return expandArithmetic(script);

        case 1:
            // Add identity operations: OP_X → OP_X OP_DUP OP_DROP
            return expandIdentity(script);

        case 2:
            // Return canonical form (simplified)
            return getCanonicalForm(script);

        default:
            return script;
    }
}

std::vector<uint8_t> ScriptCanonicalizer::getCanonicalForm(
    const std::vector<uint8_t>& script
) {
    // Simplify to canonical form
    auto result = foldConstants(script);
    result = removeNoOps(result);
    return result;
}

bool ScriptCanonicalizer::areEquivalent(
    const std::vector<uint8_t>& script1,
    const std::vector<uint8_t>& script2
) {
    // Heuristic check: compare canonical forms
    auto canonical1 = getCanonicalForm(script1);
    auto canonical2 = getCanonicalForm(script2);

    return canonical1 == canonical2;
}

std::vector<uint8_t> ScriptCanonicalizer::expandArithmetic(
    const std::vector<uint8_t>& script
) {
    std::vector<uint8_t> result;
    result.reserve(script.size() * 2);

    for (size_t i = 0; i < script.size(); ++i) {
        uint8_t op = script[i];

        // Expand OP_2 → OP_1 OP_1 OP_ADD
        if (op == OP_2) {
            result.push_back(OP_1);
            result.push_back(OP_1);
            result.push_back(OP_ADD);
        }
        // Expand OP_3 → OP_1 OP_2 OP_ADD
        else if (op == OP_3) {
            result.push_back(OP_1);
            result.push_back(OP_2);
            result.push_back(OP_ADD);
        }
        // Expand OP_4 → OP_2 OP_2 OP_ADD
        else if (op == OP_4) {
            result.push_back(OP_2);
            result.push_back(OP_2);
            result.push_back(OP_ADD);
        }
        // Expand OP_5 → OP_2 OP_3 OP_ADD
        else if (op == OP_5) {
            result.push_back(OP_2);
            result.push_back(OP_3);
            result.push_back(OP_ADD);
        }
        else {
            result.push_back(op);
        }
    }

    return result;
}

std::vector<uint8_t> ScriptCanonicalizer::expandIdentity(
    const std::vector<uint8_t>& script
) {
    std::vector<uint8_t> result;
    result.reserve(script.size() * 2);

    for (size_t i = 0; i < script.size(); ++i) {
        uint8_t op = script[i];
        result.push_back(op);

        // After each push opcode, add OP_DUP OP_DROP (identity)
        // But only for simple push opcodes (OP_1 through OP_5)
        if (op >= OP_1 && op <= OP_5) {
            result.push_back(OP_DUP);
            result.push_back(OP_DROP);
        }
    }

    return result;
}

std::vector<uint8_t> ScriptCanonicalizer::foldConstants(
    const std::vector<uint8_t>& script
) {
    std::vector<uint8_t> result;
    result.reserve(script.size());

    for (size_t i = 0; i < script.size(); ++i) {
        // Look for OP_1 OP_1 OP_ADD → OP_2
        if (i + 2 < script.size() &&
            script[i] == OP_1 &&
            script[i+1] == OP_1 &&
            script[i+2] == OP_ADD) {
            result.push_back(OP_2);
            i += 2; // Skip next 2 opcodes
        }
        // Look for OP_1 OP_2 OP_ADD → OP_3
        else if (i + 2 < script.size() &&
                 script[i] == OP_1 &&
                 script[i+1] == OP_2 &&
                 script[i+2] == OP_ADD) {
            result.push_back(OP_3);
            i += 2;
        }
        // Look for OP_2 OP_2 OP_ADD → OP_4
        else if (i + 2 < script.size() &&
                 script[i] == OP_2 &&
                 script[i+1] == OP_2 &&
                 script[i+2] == OP_ADD) {
            result.push_back(OP_4);
            i += 2;
        }
        else {
            result.push_back(script[i]);
        }
    }

    return result;
}

std::vector<uint8_t> ScriptCanonicalizer::removeNoOps(
    const std::vector<uint8_t>& script
) {
    std::vector<uint8_t> result;
    result.reserve(script.size());

    for (size_t i = 0; i < script.size(); ++i) {
        // Look for OP_DUP OP_DROP (identity, can be removed)
        if (i + 1 < script.size() &&
            script[i] == OP_DUP &&
            script[i+1] == OP_DROP) {
            // Skip both
            i += 1;
        }
        else {
            result.push_back(script[i]);
        }
    }

    return result;
}

} // namespace test
} // namespace execution
} // namespace dinero
