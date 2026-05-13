#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace dinero {
namespace execution {
namespace test {

//=============================================================================
// Operation Types (What happened during execution)
//=============================================================================

enum class OperationType {
    // Script operations
    OP_PUSH,
    OP_POP,
    OP_DUP,
    OP_SWAP,
    OP_DROP,
    OP_ADD,
    OP_SUB,
    OP_CHECKSIG,
    OP_CHECKTAPROOT,
    OP_COVENANT_CHECK,

    // Taproot operations
    PATH_SELECT,
    LEAF_REVEAL,
    MERKLE_VERIFY,
    KEY_PATH_EXECUTE,

    // Covenant operations
    OUTPUT_SHAPE_CHECK,
    STATE_TRANSITION_VERIFY,
    TIME_LOCK_VERIFY,

    // Control flow
    OP_IF,
    OP_ELSE,
    OP_ENDIF,
    OP_RETURN
};

struct Operation {
    OperationType type;
    uint64_t step;                              // Execution step number
    std::vector<uint8_t> data;                  // Operation data (if any)
    bool success;                               // Did operation succeed?
    std::optional<std::string> error;           // Error message (if failed)

    Operation(OperationType t, uint64_t s, bool ok = true)
        : type(t), step(s), success(ok) {}
};

//=============================================================================
// Stack State (Observable stack snapshots)
//=============================================================================

using StackElement = std::vector<uint8_t>;

struct StackSnapshot {
    uint64_t step;                              // Execution step
    std::vector<StackElement> stack;            // Stack contents
    size_t depth;                               // Stack depth

    StackSnapshot(uint64_t s, const std::vector<StackElement>& st)
        : step(s), stack(st), depth(st.size()) {}
};

//=============================================================================
// Taproot Path (Path selection and reveal)
//=============================================================================

struct TaprootLeaf {
    uint32_t leaf_version;
    std::vector<uint8_t> script;
    std::vector<uint8_t> merkle_proof;
};

struct TaprootPath {
    bool is_key_path;                           // True = key-path spend
    std::optional<TaprootLeaf> revealed_leaf;   // Script-path: revealed leaf
    std::vector<uint8_t> internal_key;          // Internal public key

    TaprootPath() : is_key_path(true) {}

    static TaprootPath keyPath(const std::vector<uint8_t>& key) {
        TaprootPath path;
        path.is_key_path = true;
        path.internal_key = key;
        return path;
    }

    static TaprootPath scriptPath(const TaprootLeaf& leaf, const std::vector<uint8_t>& key) {
        TaprootPath path;
        path.is_key_path = false;
        path.revealed_leaf = leaf;
        path.internal_key = key;
        return path;
    }
};

struct PathActivation {
    uint64_t step;                              // When path was revealed
    bool is_key_path;
    std::optional<uint32_t> leaf_index;         // Script-path: which leaf
    std::vector<uint8_t> merkle_proof;          // Proof of inclusion
};

//=============================================================================
// Covenant Specification (Output constraints)
//=============================================================================

struct OutputConstraint {
    uint64_t value_min;                         // Minimum output value
    uint64_t value_max;                         // Maximum output value
    std::optional<std::vector<uint8_t>> script_pubkey; // Required scriptPubKey
    bool allow_any_script;                      // If true, any script allowed

    OutputConstraint()
        : value_min(0), value_max(UINT64_MAX), allow_any_script(true) {}
};

struct StateTransition {
    std::vector<uint8_t> from_state;            // Required input state
    std::vector<uint8_t> to_state;              // Required output state
    bool monotonic;                             // State must progress forward
};

struct CovenantSpec {
    std::vector<OutputConstraint> required_outputs;
    std::optional<StateTransition> state_transition;
    std::optional<uint32_t> min_confirmations;  // Time-lock requirement

    CovenantSpec() = default;
};

//=============================================================================
// Execution Events (What the simulator observed)
//=============================================================================

enum class ExecutionEventType {
    // Lifecycle
    SCRIPT_START,
    SCRIPT_SUCCESS,
    SCRIPT_FAIL,

    // Opcodes
    OPCODE_EXECUTE,
    STACK_PUSH,
    STACK_POP,

    // Taproot
    PATH_REVEALED,
    LEAF_ACTIVATED,
    HIDDEN_PATH_INTACT,

    // Covenant
    CONSTRAINT_CHECKED,
    OUTPUT_MATCHED,
    STATE_UPDATED,

    // Composition
    MULTI_INPUT_START,
    INPUT_ISOLATED,
    COMBINED_SUCCESS
};

struct ExecutionEvent {
    ExecutionEventType type;
    uint64_t timestamp_ms;                      // When event occurred
    uint64_t step;                              // Execution step
    std::optional<std::string> details;         // Additional info

    ExecutionEvent(ExecutionEventType t, uint64_t ts, uint64_t s)
        : type(t), timestamp_ms(ts), step(s) {}
};

//=============================================================================
// Execution State (Per-execution snapshot)
//=============================================================================

struct ExecutionState {
    uint64_t step;
    std::vector<StackElement> stack;
    bool script_valid;
    std::optional<TaprootPath> active_path;
    std::vector<OutputConstraint> enforced_constraints;

    ExecutionState() : step(0), script_valid(true) {}
};

//=============================================================================
// Script Complexity (For test generation)
//=============================================================================

enum class ScriptComplexity {
    Trivial,    // OP_PUSH + OP_DROP
    Simple,     // 1-5 opcodes, no control flow
    Medium,     // 5-20 opcodes, basic control flow
    Complex,    // 20+ opcodes, nested control flow, Taproot
    Adversarial // Designed to test edge cases
};

//=============================================================================
// Witness Data
//=============================================================================

struct WitnessStack {
    std::vector<StackElement> elements;

    WitnessStack() = default;
    explicit WitnessStack(const std::vector<StackElement>& elems) : elements(elems) {}

    void push(const StackElement& elem) { elements.push_back(elem); }
    size_t size() const { return elements.size(); }
    bool empty() const { return elements.empty(); }
};

//=============================================================================
// Phase 7f: Semantic Determinism Support
//=============================================================================

/**
 * EvaluationOrder - For S21 (Evaluation Order Determinism)
 *
 * Controls how operations are evaluated within a script.
 * All orders must produce identical results (determinism property).
 */
enum class EvaluationOrder {
    DEFAULT,        // Standard left-to-right, depth-first
    LEFT_TO_RIGHT,  // Explicit left-to-right
    RIGHT_TO_LEFT,  // Right-to-left (for testing order independence)
    DEPTH_FIRST,    // Depth-first traversal
    BREADTH_FIRST   // Breadth-first traversal
};

/**
 * ExecutionStrategy - For S23 (Strategy Independence)
 *
 * Different execution implementation strategies.
 * All strategies must produce identical semantic results.
 */
enum class ExecutionStrategy {
    STANDARD,       // Default stack-based execution
    EAGER,          // Eager evaluation (evaluate immediately)
    LAZY,           // Lazy evaluation (defer when possible)
    OPTIMIZED       // Optimized execution (constant folding, etc.)
};

} // namespace test
} // namespace execution
} // namespace dinero
