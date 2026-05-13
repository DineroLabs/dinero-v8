#include "execution_trace.h"
#include <functional>
#include <algorithm>

namespace dinero {
namespace execution {
namespace test {

//=============================================================================
// Hash Computation (Determinism Verification)
//=============================================================================

namespace {

// Simple FNV-1a hash for deterministic trace hashing
uint64_t fnv1a_hash(const void* data, size_t len, uint64_t hash = 14695981039346656037ULL) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t hashBytes(const std::vector<uint8_t>& data) {
    return fnv1a_hash(data.data(), data.size());
}

uint64_t hashUint64(uint64_t value) {
    return fnv1a_hash(&value, sizeof(value));
}

uint64_t hashBool(bool value) {
    uint8_t byte = value ? 1 : 0;
    return fnv1a_hash(&byte, 1);
}

} // anonymous namespace

uint64_t ExecutionTrace::computeHash() const {
    uint64_t hash = hashUint64(rng_seed);

    // Hash input
    hash = fnv1a_hash(script.data(), script.size(), hash);
    for (const auto& elem : witness.elements) {
        hash = fnv1a_hash(elem.data(), elem.size(), hash);
    }

    // Hash Taproot path (if used)
    if (taproot_path.has_value()) {
        hash = fnv1a_hash(&taproot_path->is_key_path, 1, hash);
        hash = fnv1a_hash(taproot_path->internal_key.data(),
                          taproot_path->internal_key.size(), hash);
    }

    // Hash operations
    for (const auto& op : operations) {
        hash = fnv1a_hash(&op.type, sizeof(op.type), hash);
        hash = fnv1a_hash(&op.step, sizeof(op.step), hash);
        hash = fnv1a_hash(&op.success, sizeof(op.success), hash);
    }

    // Hash stack states
    for (const auto& snapshot : stack_states) {
        hash = fnv1a_hash(&snapshot.step, sizeof(snapshot.step), hash);
        hash = fnv1a_hash(&snapshot.depth, sizeof(snapshot.depth), hash);
    }

    // Hash path reveals
    for (const auto& reveal : path_reveals) {
        hash = fnv1a_hash(&reveal.step, sizeof(reveal.step), hash);
        hash = fnv1a_hash(&reveal.is_key_path, sizeof(reveal.is_key_path), hash);
    }

    // Hash result
    hash = fnv1a_hash(&success, sizeof(success), hash);
    hash = fnv1a_hash(&operation_count, sizeof(operation_count), hash);
    hash = fnv1a_hash(&stack_depth_max, sizeof(stack_depth_max), hash);

    return hash;
}

//=============================================================================
// Well-Formedness Check
//=============================================================================

bool ExecutionTrace::isWellFormed() const {
    // Basic sanity checks

    // 1. Operation count matches recorded operations
    if (operation_count != operations.size()) {
        return false;
    }

    // 2. Operations are monotonically increasing
    for (size_t i = 1; i < operations.size(); i++) {
        if (operations[i].step <= operations[i-1].step) {
            return false;
        }
    }

    // 3. Stack snapshots are monotonic
    for (size_t i = 1; i < stack_states.size(); i++) {
        if (stack_states[i].step <= stack_states[i-1].step) {
            return false;
        }
    }

    // 4. Max stack depth is consistent
    size_t computed_max = 0;
    for (const auto& snapshot : stack_states) {
        computed_max = std::max(computed_max, snapshot.depth);
    }
    if (computed_max != stack_depth_max) {
        return false;
    }

    // 5. If failed, error message should be present
    if (!success && !error.has_value()) {
        return false;
    }

    // 6. Final hash should be non-zero (computed)
    if (final_hash == 0 && !operations.empty()) {
        return false;
    }

    return true;
}

//=============================================================================
// Trace Comparison
//=============================================================================

bool ExecutionTrace::isEquivalentTo(const ExecutionTrace& other) const {
    // Two traces are equivalent if they represent the same execution
    // (Same seed → same inputs → same operations → same result)

    // 1. Same seed (determinism)
    if (rng_seed != other.rng_seed) {
        return false;
    }

    // 2. Same input script
    if (script != other.script) {
        return false;
    }

    // 3. Same witness
    if (witness.elements.size() != other.witness.elements.size()) {
        return false;
    }
    for (size_t i = 0; i < witness.elements.size(); i++) {
        if (witness.elements[i] != other.witness.elements[i]) {
            return false;
        }
    }

    // 4. Same operations executed
    if (operations.size() != other.operations.size()) {
        return false;
    }
    for (size_t i = 0; i < operations.size(); i++) {
        if (operations[i].type != other.operations[i].type ||
            operations[i].success != other.operations[i].success) {
            return false;
        }
    }

    // 5. Same result
    if (success != other.success) {
        return false;
    }

    // 6. Same final hash (strongest check)
    if (final_hash != other.final_hash) {
        return false;
    }

    return true;
}

bool ExecutionTrace::hasDifferentPathFrom(const ExecutionTrace& other) const {
    // Two traces have different paths if they executed different operations
    // even if they reached the same result

    // Same number of operations but different types → different path
    if (operations.size() != other.operations.size()) {
        return true;
    }

    for (size_t i = 0; i < operations.size(); i++) {
        if (operations[i].type != other.operations[i].type) {
            return true; // Different execution path
        }
    }

    // Different Taproot path selection
    if (taproot_path.has_value() != other.taproot_path.has_value()) {
        return true;
    }

    if (taproot_path.has_value() && other.taproot_path.has_value()) {
        if (taproot_path->is_key_path != other.taproot_path->is_key_path) {
            return true; // Key-path vs script-path
        }
    }

    // Same operations, same path
    return false;
}

} // namespace test
} // namespace execution
} // namespace dinero
