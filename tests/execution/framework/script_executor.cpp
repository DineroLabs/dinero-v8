#include "script_executor.h"
#include <chrono>
#include <algorithm>

namespace dinero {
namespace execution {
namespace test {

//=============================================================================
// ScriptExecutor::Impl (Internal state)
//=============================================================================

struct ScriptExecutor::Impl {
    std::vector<StackElement> stack;
    uint64_t step_counter;
    bool execution_failed;
    std::string last_error;

    Impl() : step_counter(0), execution_failed(false) {}

    void reset() {
        stack.clear();
        step_counter = 0;
        execution_failed = false;
        last_error.clear();
    }
};

//=============================================================================
// Constructor / Destructor
//=============================================================================

ScriptExecutor::ScriptExecutor() : pimpl(std::make_unique<Impl>()) {}

ScriptExecutor::~ScriptExecutor() = default;

void ScriptExecutor::reset() {
    pimpl->reset();
}

//=============================================================================
// Basic Script Execution (Phase 7a)
//=============================================================================

bool ScriptExecutor::execute(
    const std::vector<uint8_t>& script,
    const WitnessStack& witness,
    ExecutionTrace& trace
) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Reset state
    pimpl->reset();

    // Initialize trace
    trace.script = script;
    trace.witness = witness;
    trace.success = false;

    // Record start event
    trace.recordEvent(ExecutionEvent(
        ExecutionEventType::SCRIPT_START,
        0,
        pimpl->step_counter
    ));

    // Push witness elements onto stack (in order)
    for (const auto& elem : witness.elements) {
        pimpl->stack.push_back(elem);

        Operation op(OperationType::OP_PUSH, pimpl->step_counter++, true);
        op.data = elem;
        trace.recordOperation(op);

        recordStackSnapshot(pimpl->stack, trace, pimpl->step_counter);
    }

    // Execute script opcodes
    for (size_t pc = 0; pc < script.size() && !pimpl->execution_failed; ) {
        uint8_t opcode = script[pc];

        if (!executeOpcode(opcode, pimpl->stack, trace, pimpl->step_counter++)) {
            pimpl->execution_failed = true;
            trace.error = pimpl->last_error;
            break;
        }

        recordStackSnapshot(pimpl->stack, trace, pimpl->step_counter);
        pc++;
    }

    // Script succeeds if:
    // 1. No execution failure
    // 2. Stack has at least one element
    // 3. Top element is non-zero (true)
    bool success = !pimpl->execution_failed &&
                   !pimpl->stack.empty() &&
                   !pimpl->stack.back().empty() &&
                   pimpl->stack.back()[0] != 0;

    trace.success = success;

    // Record final event
    trace.recordEvent(ExecutionEvent(
        success ? ExecutionEventType::SCRIPT_SUCCESS : ExecutionEventType::SCRIPT_FAIL,
        0,
        pimpl->step_counter
    ));

    // Finalize trace
    trace.final_state.step = pimpl->step_counter;
    trace.final_state.stack = pimpl->stack;
    trace.final_state.script_valid = success;
    trace.final_hash = trace.computeHash();

    auto end_time = std::chrono::high_resolution_clock::now();
    trace.execution_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time
    ).count();

    return success;
}

//=============================================================================
// Opcode Execution (Phase 7a - Basic opcodes only)
//=============================================================================

bool ScriptExecutor::executeOpcode(
    uint8_t opcode,
    std::vector<StackElement>& stack,
    ExecutionTrace& trace,
    uint64_t step
) {
    // Phase 7a: Implement basic opcodes for testing
    // Full opcode set in later phases

    switch (opcode) {
        //=====================================================================
        // Stack operations
        //=====================================================================

        case 0x4f: // OP_1NEGATE (-1)
        {
            std::vector<uint8_t> elem = {0x81}; // -1 in script number encoding
            stack.push_back(elem);
            trace.recordOperation(Operation(OperationType::OP_PUSH, step, true));
            return true;
        }

        case 0x51: // OP_1 (true)
        {
            std::vector<uint8_t> elem = {0x01};
            stack.push_back(elem);
            trace.recordOperation(Operation(OperationType::OP_PUSH, step, true));
            return true;
        }

        case 0x52: // OP_2
        case 0x53: // OP_3
        case 0x54: // OP_4
        case 0x55: // OP_5
        {
            uint8_t value = opcode - 0x50;
            std::vector<uint8_t> elem = {value};
            stack.push_back(elem);
            trace.recordOperation(Operation(OperationType::OP_PUSH, step, true));
            return true;
        }

        case 0x75: // OP_DROP
        {
            if (stack.empty()) {
                pimpl->last_error = "OP_DROP: stack underflow";
                trace.recordOperation(Operation(OperationType::OP_DROP, step, false));
                return false;
            }
            stack.pop_back();
            trace.recordOperation(Operation(OperationType::OP_DROP, step, true));
            return true;
        }

        case 0x76: // OP_DUP
        {
            if (stack.empty()) {
                pimpl->last_error = "OP_DUP: stack underflow";
                trace.recordOperation(Operation(OperationType::OP_DUP, step, false));
                return false;
            }
            stack.push_back(stack.back());
            trace.recordOperation(Operation(OperationType::OP_DUP, step, true));
            return true;
        }

        case 0x7c: // OP_SWAP
        {
            if (stack.size() < 2) {
                pimpl->last_error = "OP_SWAP: insufficient stack elements";
                trace.recordOperation(Operation(OperationType::OP_SWAP, step, false));
                return false;
            }
            std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
            trace.recordOperation(Operation(OperationType::OP_SWAP, step, true));
            return true;
        }

        //=====================================================================
        // Arithmetic operations
        //=====================================================================

        case 0x93: // OP_ADD
        {
            if (stack.size() < 2) {
                pimpl->last_error = "OP_ADD: insufficient stack elements";
                trace.recordOperation(Operation(OperationType::OP_ADD, step, false));
                return false;
            }

            // Simple integer addition (simplified - real script uses script numbers)
            auto b = stack.back(); stack.pop_back();
            auto a = stack.back(); stack.pop_back();

            if (a.empty() || b.empty()) {
                pimpl->last_error = "OP_ADD: invalid operands";
                return false;
            }

            // Simplified: just add bytes (real implementation uses CScriptNum)
            uint8_t result = (a[0] + b[0]) & 0xFF;
            stack.push_back({result});

            trace.recordOperation(Operation(OperationType::OP_ADD, step, true));
            return true;
        }

        case 0x94: // OP_SUB
        {
            if (stack.size() < 2) {
                pimpl->last_error = "OP_SUB: insufficient stack elements";
                trace.recordOperation(Operation(OperationType::OP_SUB, step, false));
                return false;
            }

            auto b = stack.back(); stack.pop_back();
            auto a = stack.back(); stack.pop_back();

            if (a.empty() || b.empty()) {
                pimpl->last_error = "OP_SUB: invalid operands";
                return false;
            }

            uint8_t result = (a[0] - b[0]) & 0xFF;
            stack.push_back({result});

            trace.recordOperation(Operation(OperationType::OP_SUB, step, true));
            return true;
        }

        //=====================================================================
        // Unknown opcode
        //=====================================================================

        default:
        {
            pimpl->last_error = "Unknown opcode: 0x" +
                std::to_string(static_cast<int>(opcode));
            trace.recordOperation(Operation(OperationType::OP_PUSH, step, false));
            return false;
        }
    }
}

//=============================================================================
// Stack Snapshot Recording
//=============================================================================

void ScriptExecutor::recordStackSnapshot(
    const std::vector<StackElement>& stack,
    ExecutionTrace& trace,
    uint64_t step
) {
    StackSnapshot snapshot(step, stack);
    trace.recordStackState(snapshot);
}

//=============================================================================
// Taproot Execution (Phase 7b - Stub for now)
//=============================================================================

bool ScriptExecutor::executeWithTaproot(
    const std::vector<uint8_t>& script,
    const WitnessStack& witness,
    const TaprootPath& taproot_path,
    ExecutionTrace& trace
) {
    // Phase 7b: Implement Taproot path execution
    // For now, just execute as regular script
    trace.taproot_path = taproot_path;
    return execute(script, witness, trace);
}

//=============================================================================
// Covenant Execution (Phase 7c - Stub for now)
//=============================================================================

bool ScriptExecutor::executeWithCovenant(
    const std::vector<uint8_t>& script,
    const WitnessStack& witness,
    const CovenantSpec& covenant,
    ExecutionTrace& trace
) {
    // Phase 7c: Implement covenant constraint validation
    // For now, just execute as regular script
    trace.covenant = covenant;
    return execute(script, witness, trace);
}

} // namespace test
} // namespace execution
} // namespace dinero
