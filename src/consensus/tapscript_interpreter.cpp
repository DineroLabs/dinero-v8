#include "consensus/tapscript_interpreter.h"
#include "consensus/script_verify.h"
#include "consensus/script.h"             // Phase L0.3: For opcode definitions
#include "consensus/covenants.h"          // Phase L0.3: Covenant verification functions
#include "consensus/script_interpreter.h" // Phase L0.3: For SCRIPT_VERIFY_* flags
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <algorithm>
#include <limits>

namespace dinero {
namespace consensus {

namespace {

size_t CompactSizeLength(uint64_t value) {
    if (value < 0xfd) return 1;
    if (value <= 0xffff) return 3;
    if (value <= 0xffffffffULL) return 5;
    return 9;
}

size_t SerializedWitnessSize(const std::vector<std::vector<uint8_t>>& witness) {
    size_t size = CompactSizeLength(witness.size());
    for (const auto& element : witness) {
        size += CompactSizeLength(element.size()) + element.size();
    }
    return size;
}

bool IsActivatedCustomOpcode(uint8_t opcode, uint32_t flags) {
    return ((opcode == OP_CHECKSIGFROMSTACK ||
             opcode == OP_CHECKSIGFROMSTACKVERIFY) &&
            (flags & SCRIPT_VERIFY_CHECKSIGFROMSTACK)) ||
           (opcode == OP_TXHASH &&
            (flags & SCRIPT_VERIFY_TXHASH)) ||
           (opcode == OP_CHECKCONTRACTVERIFY &&
            (flags & SCRIPT_VERIFY_CHECKCONTRACT));
}

enum class TapscriptPreScanResult {
    CONTINUE,
    SUCCESS,
    INVALID,
};

enum class PushDecodeResult {
    NOT_PUSH,
    PUSH,
    INVALID,
};

PushDecodeResult DecodePushLength(
    const std::vector<uint8_t>& script,
    size_t& pc,
    uint8_t opcode,
    size_t& push_length,
    std::string& error
) {
    uint64_t decoded_length = 0;
    if (opcode == OP_0) {
        decoded_length = 0;
    } else if (opcode >= 0x01 && opcode <= 0x4b) {
        decoded_length = opcode;
    } else if (opcode == OP_PUSHDATA1) {
        if (pc >= script.size()) {
            error = "OP_PUSHDATA1: missing length byte";
            return PushDecodeResult::INVALID;
        }
        decoded_length = script[pc++];
    } else if (opcode == OP_PUSHDATA2) {
        if (script.size() - pc < 2) {
            error = "OP_PUSHDATA2: missing length bytes";
            return PushDecodeResult::INVALID;
        }
        decoded_length =
            static_cast<uint64_t>(script[pc]) |
            (static_cast<uint64_t>(script[pc + 1]) << 8);
        pc += 2;
    } else if (opcode == OP_PUSHDATA4) {
        if (script.size() - pc < 4) {
            error = "OP_PUSHDATA4: missing length bytes";
            return PushDecodeResult::INVALID;
        }
        decoded_length =
            static_cast<uint64_t>(script[pc]) |
            (static_cast<uint64_t>(script[pc + 1]) << 8) |
            (static_cast<uint64_t>(script[pc + 2]) << 16) |
            (static_cast<uint64_t>(script[pc + 3]) << 24);
        pc += 4;
    } else {
        return PushDecodeResult::NOT_PUSH;
    }

    if (decoded_length > script.size() - pc) {
        error = "Push operation exceeds script bounds";
        return PushDecodeResult::INVALID;
    }
    push_length = static_cast<size_t>(decoded_length);
    return PushDecodeResult::PUSH;
}

TapscriptPreScanResult PreScanTapscript(
    const std::vector<uint8_t>& script,
    uint32_t flags,
    std::string& error
) {
    size_t pc = 0;
    while (pc < script.size()) {
        const uint8_t opcode = script[pc++];
        if (TapscriptOpcodes::IsOpSuccess(opcode) &&
            !IsActivatedCustomOpcode(opcode, flags)) {
            return TapscriptPreScanResult::SUCCESS;
        }

        size_t push_length = 0;
        const PushDecodeResult push_result =
            DecodePushLength(script, pc, opcode, push_length, error);
        if (push_result == PushDecodeResult::INVALID) {
            return TapscriptPreScanResult::INVALID;
        }
        if (push_result == PushDecodeResult::PUSH) {
            pc += push_length;
        }
    }
    return TapscriptPreScanResult::CONTINUE;
}

ScriptExecutionContext MakeTaprootSighashContext(
    const Transaction* tx,
    size_t input_index,
    uint32_t flags,
    const std::vector<UTXOEntry>& input_utxos,
    const PrecomputedTransactionData* covenant_precomputed
) {
    if (covenant_precomputed != nullptr &&
        covenant_precomputed->HasTaprootDataFor(*tx)) {
        ScriptExecutionContext context(
            tx, static_cast<uint32_t>(input_index),
            input_utxos[input_index].value.GetUna(), flags);
        context.covenant_precomputed = covenant_precomputed;
        return context;
    }

    std::vector<uint64_t> amounts;
    std::vector<std::vector<uint8_t>> scripts;
    std::vector<uint8_t> confidential_flags;
    std::vector<std::vector<uint8_t>> commitments;
    amounts.reserve(input_utxos.size());
    scripts.reserve(input_utxos.size());
    confidential_flags.reserve(input_utxos.size());
    commitments.reserve(input_utxos.size());
    for (const auto& utxo : input_utxos) {
        amounts.push_back(utxo.value.GetUna());
        scripts.push_back(utxo.scriptPubKey);
        confidential_flags.push_back(utxo.is_confidential ? 1 : 0);
        commitments.push_back(utxo.commitment);
    }
    return ScriptExecutionContext(
        tx, static_cast<uint32_t>(input_index),
        amounts[input_index], flags,
        amounts, scripts, confidential_flags, commitments,
        covenant_precomputed);
}

bool ConsumeSignatureBudget(int64_t& validation_weight_left, std::string& error) {
    validation_weight_left -= 50;
    if (validation_weight_left < 0) {
        error = "Tapscript signature validation weight exceeded";
        return false;
    }
    return true;
}

bool DecodeScriptNum(
    const std::vector<uint8_t>& bytes,
    bool require_minimal,
    size_t max_size,
    int64_t& value
) {
    if (bytes.size() > max_size) return false;
    if (require_minimal && !bytes.empty() &&
        (bytes.back() & 0x7f) == 0 &&
        (bytes.size() == 1 || (bytes[bytes.size() - 2] & 0x80) == 0)) {
        return false;
    }

    uint64_t result = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        result |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    if (!bytes.empty() && (bytes.back() & 0x80)) {
        result &= ~(uint64_t{0x80} << (8 * (bytes.size() - 1)));
        value = -static_cast<int64_t>(result);
    } else {
        value = static_cast<int64_t>(result);
    }
    return true;
}

std::vector<uint8_t> EncodeScriptNum(int64_t value) {
    if (value == 0) return {};
    const bool negative = value < 0;
    uint64_t absolute = negative
        ? static_cast<uint64_t>(-(value + 1)) + 1
        : static_cast<uint64_t>(value);
    std::vector<uint8_t> result;
    while (absolute != 0) {
        result.push_back(static_cast<uint8_t>(absolute & 0xff));
        absolute >>= 8;
    }
    if (result.back() & 0x80) {
        result.push_back(negative ? 0x80 : 0x00);
    } else if (negative) {
        result.back() |= 0x80;
    }
    return result;
}

bool IsMinimalPush(uint8_t opcode, const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return opcode == OP_0;
    }
    if (data.size() == 1 && data[0] >= 1 && data[0] <= 16) {
        return opcode == OP_1 + (data[0] - 1);
    }
    if (data.size() == 1 && data[0] == 0x81) {
        return opcode == OP_1NEGATE;
    }
    if (data.size() <= 75) {
        return opcode == data.size();
    }
    if (data.size() <= 255) {
        return opcode == OP_PUSHDATA1;
    }
    if (data.size() <= 65535) {
        return opcode == OP_PUSHDATA2;
    }
    return true;
}

} // namespace

// Phase L0.3: Use opcodes from consensus namespace (script.h) instead of TapscriptOpcodes
// to avoid ambiguity with covenant opcodes

// ============================================================
// Main Execution Entry Point
// ============================================================

bool TapscriptInterpreter::ExecuteTapscript(
    const std::vector<uint8_t>& script,
    const std::vector<std::vector<uint8_t>>& witness_stack,
    const Transaction& tx,
    size_t input_index,
    const std::vector<UTXOEntry>& input_utxos,
    const std::vector<uint8_t>& tapleaf_hash,
    const std::array<uint8_t, 32>& internal_key,
    const std::array<uint8_t, 32>& merkle_root,
    uint8_t output_key_parity,
    uint32_t flags,
    std::string& error,
    const std::vector<uint8_t>& annex,
    const PrecomputedTransactionData* covenant_precomputed
) {
    // BIP342 decodes OP_SUCCESS before every resource and execution rule.
    // Pushed bytes are skipped and therefore cannot masquerade as opcodes.
    const TapscriptPreScanResult pre_scan =
        PreScanTapscript(script, flags, error);
    if (pre_scan == TapscriptPreScanResult::SUCCESS) {
        return true;
    }
    if (pre_scan == TapscriptPreScanResult::INVALID) {
        return false;
    }

    // BIP342 retains the 1,000-element and 520-byte element limits for the
    // initial stack, but removes the legacy 10,000-byte script-size limit.
    if (witness_stack.size() > 1000) {
        error = "Initial stack exceeds 1,000 elements";
        return false;
    }
    for (const auto& element : witness_stack) {
        if (element.size() > 520) {
            error = "Initial stack element exceeds 520 bytes";
            return false;
        }
    }

    // Initialize execution context
    ExecutionContext ctx;
    ctx.stack = witness_stack; // Initial stack from witness
    ctx.tx = &tx;
    ctx.input_index = input_index;
    ctx.input_utxos = &input_utxos;
    ctx.tapscript = &script;
    ctx.tapleaf_hash = &tapleaf_hash;
    ctx.internal_key = &internal_key;
    ctx.merkle_root = &merkle_root;
    ctx.output_key_parity = output_key_parity;
    ctx.annex = &annex;  // BIP341 annex for sighash computation
    ctx.covenant_precomputed = covenant_precomputed;
    ctx.flags = flags;  // Phase L0.3: Pass flags for covenant enforcement
    ctx.validation_weight_left =
        static_cast<int64_t>(SerializedWitnessSize(tx.vin[input_index].witness)) + 50;

    // Execute the script
    if (!Execute(script, ctx)) {
        error = ctx.error.empty() ? "Script execution failed" : ctx.error;
        return false;
    }
    if (ctx.op_success) {
        return true;
    }

    // Script must leave exactly one true value on stack (BIP342)
    if (ctx.stack.empty()) {
        error = "Stack empty after script execution";
        return false;
    }

    if (ctx.stack.size() > 1) {
        error = "Stack has more than one element after execution";
        return false;
    }

    if (!CastToBool(ctx.stack[0])) {
        error = "Script result is false";
        return false;
    }

    return true;
}

// ============================================================
// Core Execution Loop
// ============================================================

bool TapscriptInterpreter::Execute(const std::vector<uint8_t>& script, ExecutionContext& ctx) {
    size_t pc = 0;
    uint32_t opcode_position = 0;

    // O(1) conditional execution state, matching Bitcoin Core's ConditionStack
    // behavior without rescanning the nesting vector for every opcode.
    std::vector<bool> condition_stack;
    size_t inactive_conditions = 0;

    auto require_stack = [&](size_t count, const char* operation) {
        if (ctx.stack.size() >= count) {
            return true;
        }
        ctx.error = std::string(operation) + ": insufficient stack elements";
        return false;
    };
    auto stack_top = [&](size_t depth) -> std::vector<uint8_t>& {
        return ctx.stack[ctx.stack.size() - depth];
    };
    auto decode_number = [&](size_t depth, size_t max_size, int64_t& value) {
        if (!require_stack(depth, "Numeric opcode")) {
            return false;
        }
        if (!DecodeScriptNum(
                stack_top(depth),
                (ctx.flags & SCRIPT_VERIFY_MINIMALDATA) != 0,
                max_size,
                value)) {
            ctx.error = "Invalid script number";
            return false;
        }
        return true;
    };

    while (pc < script.size()) {
        // BIP342 counts decoded opcodes, not byte offsets. A multi-byte push is
        // one opcode, and opcodes in inactive branches still advance this
        // position even though they are not executed.
        const uint32_t current_opcode_position = opcode_position++;
        const uint8_t opcode = script[pc++];

        // OP_SUCCESS opcodes (BIP342 soft fork mechanism)
        if (TapscriptOpcodes::IsOpSuccess(opcode)) {
            if (!IsActivatedCustomOpcode(opcode, ctx.flags)) {
                ctx.op_success = true;
                return true;
            }
        }

        // Decode every push, including pushes in unexecuted branches. Bounds
        // and the 520-byte element limit remain consensus checks even when the
        // branch is inactive; only the stack mutation is conditional.
        size_t push_length = 0;
        const PushDecodeResult push_result =
            DecodePushLength(script, pc, opcode, push_length, ctx.error);
        if (push_result == PushDecodeResult::INVALID) {
            return false;
        }
        if (push_result == PushDecodeResult::PUSH) {
            if (push_length > 520) {
                ctx.error =
                    "Stack element size limit exceeded (BIP342: 520 bytes max)";
                return false;
            }
            if (inactive_conditions == 0) {
                std::vector<uint8_t> data(
                    script.begin() + pc,
                    script.begin() + pc + push_length);
                if ((ctx.flags & SCRIPT_VERIFY_MINIMALDATA) &&
                    !IsMinimalPush(opcode, data)) {
                    ctx.error = "Non-minimal data push";
                    return false;
                }
                if (!PushStack(ctx, data)) return false;
            }
            pc += push_length;
            continue;
        }

        // OP_VERIF and OP_VERNOTIF are permanently disabled and fail even in
        // an unexecuted branch.
        if (opcode == OP_VERIF || opcode == OP_VERNOTIF) {
            ctx.error = "Disabled conditional opcode";
            return false;
        }

        // IF/NOTIF/ELSE/ENDIF must be processed even inside an inactive branch
        // so nesting remains balanced. Tapscript enforces MINIMALIF as a
        // consensus rule: the consumed value is exactly {} or {0x01}.
        if (opcode == OP_IF || opcode == OP_NOTIF) {
            bool condition = false;
            if (inactive_conditions == 0) {
                std::vector<uint8_t> value;
                if (!PopStack(ctx, value)) {
                    ctx.error =
                        opcode == OP_IF
                            ? "OP_IF: insufficient stack elements"
                            : "OP_NOTIF: insufficient stack elements";
                    return false;
                }
                if (!value.empty() &&
                    !(value.size() == 1 && value[0] == 0x01)) {
                    ctx.error =
                        "OP_IF/OP_NOTIF requires a minimal boolean in tapscript";
                    return false;
                }
                condition = !value.empty();
                if (opcode == OP_NOTIF) {
                    condition = !condition;
                }
            }
            condition_stack.push_back(condition);
            if (!condition) {
                ++inactive_conditions;
            }
            continue;
        }

        if (opcode == OP_ELSE) {
            if (condition_stack.empty()) {
                ctx.error = "OP_ELSE without OP_IF/OP_NOTIF";
                return false;
            }
            if (!condition_stack.back()) {
                --inactive_conditions;
            }
            condition_stack.back() = !condition_stack.back();
            if (!condition_stack.back()) {
                ++inactive_conditions;
            }
            continue;
        }

        if (opcode == OP_ENDIF) {
            if (condition_stack.empty()) {
                ctx.error = "OP_ENDIF without OP_IF/OP_NOTIF";
                return false;
            }
            if (!condition_stack.back()) {
                --inactive_conditions;
            }
            condition_stack.pop_back();
            continue;
        }

        // Ordinary opcodes in an inactive branch are parsed but not executed.
        if (inactive_conditions != 0) {
            continue;
        }

        if (opcode == OP_1NEGATE ||
            (opcode >= OP_1 && opcode <= OP_16)) {
            const int64_t value =
                opcode == OP_1NEGATE
                    ? -1
                    : static_cast<int64_t>(opcode - (OP_1 - 1));
            if (!PushStack(ctx, EncodeScriptNum(value))) return false;
            continue;
        }

        // Handle opcodes
        switch (opcode) {
            // Stack operations
            case OP_TOALTSTACK:
                if (!require_stack(1, "OP_TOALTSTACK")) return false;
                ctx.altstack.push_back(std::move(ctx.stack.back()));
                ctx.stack.pop_back();
                break;

            case OP_FROMALTSTACK:
                if (ctx.altstack.empty()) {
                    ctx.error = "OP_FROMALTSTACK: altstack empty";
                    return false;
                }
                ctx.stack.push_back(std::move(ctx.altstack.back()));
                ctx.altstack.pop_back();
                break;

            case OP_2DROP:
                if (!require_stack(2, "OP_2DROP")) return false;
                ctx.stack.pop_back();
                ctx.stack.pop_back();
                break;

            case OP_2DUP: {
                if (!require_stack(2, "OP_2DUP")) return false;
                const auto first = stack_top(2);
                const auto second = stack_top(1);
                if (!PushStack(ctx, first) || !PushStack(ctx, second)) {
                    return false;
                }
                break;
            }

            case OP_3DUP: {
                if (!require_stack(3, "OP_3DUP")) return false;
                const auto first = stack_top(3);
                const auto second = stack_top(2);
                const auto third = stack_top(1);
                if (!PushStack(ctx, first) ||
                    !PushStack(ctx, second) ||
                    !PushStack(ctx, third)) {
                    return false;
                }
                break;
            }

            case OP_2OVER: {
                if (!require_stack(4, "OP_2OVER")) return false;
                const auto first = stack_top(4);
                const auto second = stack_top(3);
                if (!PushStack(ctx, first) || !PushStack(ctx, second)) {
                    return false;
                }
                break;
            }

            case OP_2ROT: {
                if (!require_stack(6, "OP_2ROT")) return false;
                const auto first = stack_top(6);
                const auto second = stack_top(5);
                ctx.stack.erase(ctx.stack.end() - 6);
                ctx.stack.erase(ctx.stack.end() - 5);
                if (!PushStack(ctx, first) || !PushStack(ctx, second)) {
                    return false;
                }
                break;
            }

            case OP_2SWAP:
                if (!require_stack(4, "OP_2SWAP")) return false;
                std::swap(stack_top(4), stack_top(2));
                std::swap(stack_top(3), stack_top(1));
                break;

            case OP_IFDUP:
                if (!require_stack(1, "OP_IFDUP")) return false;
                if (CastToBool(ctx.stack.back())) {
                    const auto value = ctx.stack.back();
                    if (!PushStack(ctx, value)) return false;
                }
                break;

            case OP_DEPTH:
                if (!PushStack(
                        ctx,
                        EncodeScriptNum(
                            static_cast<int64_t>(ctx.stack.size())))) {
                    return false;
                }
                break;

            case OP_DUP:
                if (!OpDup(ctx)) return false;
                break;

            case OP_DROP:
                if (!OpDrop(ctx)) return false;
                break;

            case OP_NIP:
                if (!require_stack(2, "OP_NIP")) return false;
                ctx.stack.erase(ctx.stack.end() - 2);
                break;

            case OP_OVER: {
                if (!require_stack(2, "OP_OVER")) return false;
                const auto value = stack_top(2);
                if (!PushStack(ctx, value)) return false;
                break;
            }

            case OP_PICK:
            case OP_ROLL: {
                int64_t depth = 0;
                if (!decode_number(1, 4, depth)) return false;
                ctx.stack.pop_back();
                if (depth < 0 ||
                    static_cast<size_t>(depth) >= ctx.stack.size()) {
                    ctx.error =
                        opcode == OP_PICK
                            ? "OP_PICK: invalid stack depth"
                            : "OP_ROLL: invalid stack depth";
                    return false;
                }
                const size_t index =
                    ctx.stack.size() - 1 - static_cast<size_t>(depth);
                const auto value = ctx.stack[index];
                if (opcode == OP_ROLL) {
                    ctx.stack.erase(ctx.stack.begin() + index);
                }
                if (!PushStack(ctx, value)) return false;
                break;
            }

            case OP_ROT:
                if (!require_stack(3, "OP_ROT")) return false;
                std::rotate(ctx.stack.end() - 3,
                            ctx.stack.end() - 2,
                            ctx.stack.end());
                break;

            case OP_SWAP:
                if (!require_stack(2, "OP_SWAP")) return false;
                std::swap(stack_top(2), stack_top(1));
                break;

            case OP_TUCK: {
                if (!require_stack(2, "OP_TUCK")) return false;
                const auto value = ctx.stack.back();
                ctx.stack.insert(ctx.stack.end() - 2, value);
                break;
            }

            case OP_SIZE:
                if (!require_stack(1, "OP_SIZE") ||
                    !PushStack(
                        ctx,
                        EncodeScriptNum(
                            static_cast<int64_t>(ctx.stack.back().size())))) {
                    return false;
                }
                break;

            // Equality
            case OP_EQUAL:
                if (!OpEqual(ctx)) return false;
                break;

            case OP_EQUALVERIFY:
                if (!OpEqualVerify(ctx)) return false;
                break;

            // Flow control
            case OP_VERIFY:
                if (!OpVerify(ctx)) return false;
                break;

            case OP_RETURN:
                if (!OpReturn(ctx)) return false;
                break;

            // Numeric operations inherited from P2WSH. Tapscript retains
            // four-byte minimally encoded operands when MINIMALDATA is set.
            case OP_1ADD:
            case OP_1SUB:
            case OP_NEGATE:
            case OP_ABS:
            case OP_NOT:
            case OP_0NOTEQUAL: {
                int64_t value = 0;
                if (!decode_number(1, 4, value)) return false;
                ctx.stack.pop_back();
                switch (opcode) {
                    case OP_1ADD: ++value; break;
                    case OP_1SUB: --value; break;
                    case OP_NEGATE: value = -value; break;
                    case OP_ABS:
                        if (value < 0) value = -value;
                        break;
                    case OP_NOT: value = value == 0 ? 1 : 0; break;
                    case OP_0NOTEQUAL: value = value != 0 ? 1 : 0; break;
                    default: break;
                }
                if (!PushStack(ctx, EncodeScriptNum(value))) return false;
                break;
            }

            case OP_ADD:
            case OP_SUB:
            case OP_BOOLAND:
            case OP_BOOLOR:
            case OP_NUMEQUAL:
            case OP_NUMEQUALVERIFY:
            case OP_NUMNOTEQUAL:
            case OP_LESSTHAN:
            case OP_GREATERTHAN:
            case OP_LESSTHANOREQUAL:
            case OP_GREATERTHANOREQUAL:
            case OP_MIN:
            case OP_MAX: {
                int64_t left = 0;
                int64_t right = 0;
                if (!decode_number(2, 4, left) ||
                    !decode_number(1, 4, right)) {
                    return false;
                }

                int64_t result = 0;
                switch (opcode) {
                    case OP_ADD: result = left + right; break;
                    case OP_SUB: result = left - right; break;
                    case OP_BOOLAND:
                        result = left != 0 && right != 0 ? 1 : 0;
                        break;
                    case OP_BOOLOR:
                        result = left != 0 || right != 0 ? 1 : 0;
                        break;
                    case OP_NUMEQUAL:
                    case OP_NUMEQUALVERIFY:
                        result = left == right ? 1 : 0;
                        break;
                    case OP_NUMNOTEQUAL:
                        result = left != right ? 1 : 0;
                        break;
                    case OP_LESSTHAN:
                        result = left < right ? 1 : 0;
                        break;
                    case OP_GREATERTHAN:
                        result = left > right ? 1 : 0;
                        break;
                    case OP_LESSTHANOREQUAL:
                        result = left <= right ? 1 : 0;
                        break;
                    case OP_GREATERTHANOREQUAL:
                        result = left >= right ? 1 : 0;
                        break;
                    case OP_MIN: result = std::min(left, right); break;
                    case OP_MAX: result = std::max(left, right); break;
                    default: break;
                }
                ctx.stack.pop_back();
                ctx.stack.pop_back();

                if (opcode == OP_NUMEQUALVERIFY) {
                    if (result == 0) {
                        ctx.error = "OP_NUMEQUALVERIFY failed";
                        return false;
                    }
                } else if (!PushStack(ctx, EncodeScriptNum(result))) {
                    return false;
                }
                break;
            }

            case OP_WITHIN: {
                int64_t value = 0;
                int64_t minimum = 0;
                int64_t maximum = 0;
                if (!decode_number(3, 4, value) ||
                    !decode_number(2, 4, minimum) ||
                    !decode_number(1, 4, maximum)) {
                    return false;
                }
                ctx.stack.pop_back();
                ctx.stack.pop_back();
                ctx.stack.pop_back();
                if (!PushStack(
                        ctx,
                        EncodeScriptNum(
                            minimum <= value && value < maximum ? 1 : 0))) {
                    return false;
                }
                break;
            }

            // Hash operations inherited unchanged from P2WSH.
            case OP_RIPEMD160:
            case OP_SHA1:
            case OP_SHA256:
            case OP_HASH160:
            case OP_HASH256: {
                if (!require_stack(1, "Hash opcode")) return false;
                const auto input = ctx.stack.back();
                ctx.stack.pop_back();
                std::vector<uint8_t> digest;
                switch (opcode) {
                    case OP_RIPEMD160: digest = RIPEMD160_Hash(input); break;
                    case OP_SHA1: digest = SHA1_Hash(input); break;
                    case OP_SHA256: digest = SHA256_Hash(input); break;
                    case OP_HASH160: digest = HASH160_Hash(input); break;
                    case OP_HASH256: digest = HASH256_Hash(input); break;
                    default: break;
                }
                if (!PushStack(ctx, digest)) return false;
                break;
            }

            case OP_CODESEPARATOR:
                ctx.code_separator_position = current_opcode_position;
                break;

            // Crypto (Schnorr signatures - BIP342)
            case OP_CHECKSIG:
                if (!OpCheckSig(ctx)) return false;
                break;

            case OP_CHECKSIGVERIFY:
                if (!OpCheckSigVerify(ctx)) return false;
                break;

            case OP_CHECKSIGADD:
                if (!OpCheckSigAdd(ctx)) return false;
                break;

            // BIP342 disables the legacy multisig opcodes. They fail only when
            // executed; the inactive-branch path above ignores them.
            case OP_CHECKMULTISIG:
            case OP_CHECKMULTISIGVERIFY:
                ctx.error =
                    "OP_CHECKMULTISIG(VERIFY) is disabled in tapscript";
                return false;

            case OP_NOP:
                break;

            case OP_NOP1:
            case OP_NOP5:
            case OP_NOP6:
            case OP_NOP7:
            case OP_NOP8:
            case OP_NOP9:
            case OP_NOP10:
                if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                    ctx.error = "Discouraged upgradable NOP";
                    return false;
                }
                break;

            case OP_CHECKLOCKTIMEVERIFY: {
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)) {
                    if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                        ctx.error = "Discouraged OP_CHECKLOCKTIMEVERIFY NOP";
                        return false;
                    }
                    break;
                }

                int64_t required_lock_time = 0;
                if (!decode_number(1, 5, required_lock_time)) return false;
                if (required_lock_time < 0) {
                    ctx.error = "OP_CHECKLOCKTIMEVERIFY: negative locktime";
                    return false;
                }
                if (!ctx.tx || ctx.input_index >= ctx.tx->vin.size()) {
                    ctx.error =
                        "OP_CHECKLOCKTIMEVERIFY: missing transaction context";
                    return false;
                }

                constexpr int64_t kLockTimeThreshold = 500'000'000;
                const int64_t transaction_lock_time = ctx.tx->lockTime;
                if ((required_lock_time < kLockTimeThreshold) !=
                        (transaction_lock_time < kLockTimeThreshold) ||
                    required_lock_time > transaction_lock_time ||
                    ctx.tx->vin[ctx.input_index].sequence ==
                        std::numeric_limits<uint32_t>::max()) {
                    ctx.error = "OP_CHECKLOCKTIMEVERIFY: unsatisfied locktime";
                    return false;
                }
                break;
            }

            case OP_CHECKSEQUENCEVERIFY: {
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKSEQUENCEVERIFY)) {
                    if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                        ctx.error = "Discouraged OP_CHECKSEQUENCEVERIFY NOP";
                        return false;
                    }
                    break;
                }

                int64_t required_sequence = 0;
                if (!decode_number(1, 5, required_sequence)) return false;
                if (required_sequence < 0) {
                    ctx.error =
                        "OP_CHECKSEQUENCEVERIFY: negative sequence";
                    return false;
                }

                constexpr int64_t kDisableFlag = int64_t{1} << 31;
                constexpr int64_t kTypeFlag = int64_t{1} << 22;
                constexpr int64_t kSequenceMask = 0x0000ffff;
                if (required_sequence & kDisableFlag) {
                    break;
                }
                if (!ctx.tx ||
                    ctx.tx->version < 2 ||
                    ctx.input_index >= ctx.tx->vin.size()) {
                    ctx.error =
                        "OP_CHECKSEQUENCEVERIFY: transaction is not eligible";
                    return false;
                }

                const int64_t transaction_sequence =
                    ctx.tx->vin[ctx.input_index].sequence;
                if (transaction_sequence & kDisableFlag) {
                    ctx.error =
                        "OP_CHECKSEQUENCEVERIFY: input sequence disabled";
                    return false;
                }
                const int64_t required_masked =
                    required_sequence & (kTypeFlag | kSequenceMask);
                const int64_t transaction_masked =
                    transaction_sequence & (kTypeFlag | kSequenceMask);
                if ((required_masked & kTypeFlag) !=
                        (transaction_masked & kTypeFlag) ||
                    required_masked > transaction_masked) {
                    ctx.error =
                        "OP_CHECKSEQUENCEVERIFY: unsatisfied sequence";
                    return false;
                }
                break;
            }

            // Phase L0.3: Covenant opcodes (consensus-critical)
            case OP_CHECKTEMPLATEVERIFY:
                // BIP119 assigns NOP4. Before activation it remains a NOP;
                // the 32-byte argument rule is part of the activated opcode.
                if (ctx.flags & SCRIPT_VERIFY_CHECKTEMPLATEVERIFY) {
                    if (!OpCheckTemplateVerify(ctx)) return false;
                } else if (
                    ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                    ctx.error =
                        "Discouraged inactive OP_CHECKTEMPLATEVERIFY";
                    return false;
                }
                break;

            case OP_CHECKSIGFROMSTACK:
                if (!OpCheckSigFromStack(ctx)) return false;
                break;

            // Phase 3 Fix: OP_CHECKSIGFROMSTACKVERIFY handler
            case OP_CHECKSIGFROMSTACKVERIFY:
                if (!OpCheckSigFromStack(ctx)) return false;
                if (!OpVerify(ctx)) return false;
                break;

            case OP_TXHASH:
                if (!OpTxHash(ctx)) return false;
                break;

            case OP_CHECKCONTRACTVERIFY:
                if (!OpCheckContractVerify(ctx)) return false;
                break;

            default:
                ctx.error = "Unknown or unsupported opcode: 0x" +
                           std::to_string(static_cast<int>(opcode));
                return false;
        }

        if (ctx.stack.size() + ctx.altstack.size() > 1000) {
            ctx.error =
                "Stack size limit exceeded (BIP342: 1000 elements max)";
            return false;
        }
    }

    if (!condition_stack.empty()) {
        ctx.error = "Unbalanced conditional";
        return false;
    }

    return true;
}

// ============================================================
// Stack Operations
// ============================================================

bool TapscriptInterpreter::OpDup(ExecutionContext& ctx) {
    if (ctx.stack.empty()) {
        ctx.error = "OP_DUP: stack empty";
        return false;
    }
    return PushStack(ctx, ctx.stack.back());  // BIP342: Check limits
}

bool TapscriptInterpreter::OpDrop(ExecutionContext& ctx) {
    std::vector<uint8_t> dummy;
    return PopStack(ctx, dummy);
}

bool TapscriptInterpreter::OpEqual(ExecutionContext& ctx) {
    if (ctx.stack.size() < 2) {
        ctx.error = "OP_EQUAL: insufficient stack elements";
        return false;
    }

    std::vector<uint8_t> a, b;
    if (!PopStack(ctx, b)) return false;
    if (!PopStack(ctx, a)) return false;

    bool equal = (a == b);
    return PushStack(ctx, equal ? std::vector<uint8_t>{0x01} : std::vector<uint8_t>{});  // BIP342: Check limits
}

bool TapscriptInterpreter::OpEqualVerify(ExecutionContext& ctx) {
    if (!OpEqual(ctx)) return false;
    return OpVerify(ctx);
}

bool TapscriptInterpreter::OpVerify(ExecutionContext& ctx) {
    std::vector<uint8_t> top;
    if (!PopStack(ctx, top)) return false;

    if (!CastToBool(top)) {
        ctx.error = "OP_VERIFY failed";
        return false;
    }

    return true;
}

bool TapscriptInterpreter::OpReturn(ExecutionContext& ctx) {
    ctx.error = "OP_RETURN executed";
    return false;
}

// ============================================================
// Signature Operations (BIP342 Schnorr)
// ============================================================

bool TapscriptInterpreter::OpCheckSig(ExecutionContext& ctx) {
    if (ctx.stack.size() < 2) {
        ctx.error = "OP_CHECKSIG: insufficient stack elements (need pubkey + signature)";
        return false;
    }

    std::vector<uint8_t> pubkey, signature;
    if (!PopStack(ctx, pubkey)) return false;
    if (!PopStack(ctx, signature)) return false;

    // Empty signature is always invalid (BIP342)
    if (signature.empty()) {
        return PushStack(ctx, {});  // BIP342: Push false and check limits
    }

    if (pubkey.empty()) {
        ctx.error = "OP_CHECKSIG: empty public key";
        return false;
    }

    if (!ConsumeSignatureBudget(ctx.validation_weight_left, ctx.error)) {
        return false;
    }

    // BIP342 reserves non-empty, non-32-byte public keys for future
    // signature algorithms. Consensus treats a non-empty signature as valid;
    // relay policy may discourage this upgrade path.
    if (pubkey.size() != 32) {
        if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_PUBKEYTYPE) {
            ctx.error = "OP_CHECKSIG: discouraged upgradable public key type";
            return false;
        }
        return PushStack(ctx, {0x01});
    }

    if (signature.size() != 64 && signature.size() != 65) {
        ctx.error = "OP_CHECKSIG: invalid Schnorr signature size";
        return false;
    }
    const uint8_t hash_type = signature.size() == 65 ? signature.back() : 0;
    if (signature.size() == 65 && hash_type == 0) {
        ctx.error = "OP_CHECKSIG: explicit SIGHASH_DEFAULT byte is invalid";
        return false;
    }

    ScriptExecutionContext sighash_context = MakeTaprootSighashContext(
        ctx.tx, ctx.input_index, ctx.flags, *ctx.input_utxos,
        ctx.covenant_precomputed);
    std::vector<uint8_t> sighash = SignatureHashTaproot(
        sighash_context, hash_type, *ctx.tapleaf_hash,
        ctx.annex ? *ctx.annex : std::vector<uint8_t>{},
        ctx.code_separator_position);
    if (sighash.size() != 32) {
        ctx.error = "OP_CHECKSIG: invalid signature hash type";
        return false;
    }

    // Verify Schnorr signature
    const std::vector<uint8_t> schnorr_signature(signature.begin(), signature.begin() + 64);
    bool valid = VerifySchnorrSignature(schnorr_signature, pubkey, sighash);
    if (!valid) {
        ctx.error = "OP_CHECKSIG: non-empty invalid Schnorr signature";
        return false;
    }
    return PushStack(ctx, {0x01});
}

bool TapscriptInterpreter::OpCheckSigVerify(ExecutionContext& ctx) {
    if (!OpCheckSig(ctx)) return false;
    return OpVerify(ctx);
}

bool TapscriptInterpreter::OpCheckSigAdd(ExecutionContext& ctx) {
    // BIP342: OP_CHECKSIGADD for batch signature verification
    // Stack: <n> <sig> <pubkey> -> <n+1> (if valid) or <n> (if invalid)

    if (ctx.stack.size() < 3) {
        ctx.error = "OP_CHECKSIGADD: insufficient stack elements (need n + sig + pubkey)";
        return false;
    }

    std::vector<uint8_t> pubkey, signature, n_bytes;
    if (!PopStack(ctx, pubkey)) return false;
    if (!PopStack(ctx, n_bytes)) return false;
    if (!PopStack(ctx, signature)) return false;

    int64_t n = 0;
    if (!DecodeScriptNum(
            n_bytes, (ctx.flags & SCRIPT_VERIFY_MINIMALDATA) != 0, 4, n)) {
        ctx.error = "OP_CHECKSIGADD: invalid script number";
        return false;
    }

    // Empty signature is always invalid (doesn't increment n)
    if (signature.empty()) {
        return PushStack(ctx, n_bytes);  // BIP342: Push n unchanged and check limits
    }

    if (pubkey.empty()) {
        ctx.error = "OP_CHECKSIGADD: empty public key";
        return false;
    }

    if (!ConsumeSignatureBudget(ctx.validation_weight_left, ctx.error)) {
        return false;
    }

    bool valid = false;
    if (pubkey.size() != 32) {
        if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_PUBKEYTYPE) {
            ctx.error = "OP_CHECKSIGADD: discouraged upgradable public key type";
            return false;
        }
        valid = true;
    } else {
        if (signature.size() != 64 && signature.size() != 65) {
            ctx.error = "OP_CHECKSIGADD: invalid Schnorr signature size";
            return false;
        }
        const uint8_t hash_type = signature.size() == 65 ? signature.back() : 0;
        if (signature.size() == 65 && hash_type == 0) {
            ctx.error = "OP_CHECKSIGADD: explicit SIGHASH_DEFAULT byte is invalid";
            return false;
        }
        ScriptExecutionContext sighash_context = MakeTaprootSighashContext(
            ctx.tx, ctx.input_index, ctx.flags, *ctx.input_utxos,
            ctx.covenant_precomputed);
        std::vector<uint8_t> sighash = SignatureHashTaproot(
            sighash_context, hash_type, *ctx.tapleaf_hash,
            ctx.annex ? *ctx.annex : std::vector<uint8_t>{},
            ctx.code_separator_position);
        if (sighash.size() != 32) {
            ctx.error = "OP_CHECKSIGADD: invalid signature hash type";
            return false;
        }
        const std::vector<uint8_t> schnorr_signature(
            signature.begin(), signature.begin() + 64);
        valid = VerifySchnorrSignature(schnorr_signature, pubkey, sighash);
        if (!valid) {
            ctx.error = "OP_CHECKSIGADD: non-empty invalid Schnorr signature";
            return false;
        }
    }

    // Increment n if valid
    if (valid) {
        n++;
    }

    // Push updated n back to stack
    return PushStack(ctx, EncodeScriptNum(n));
}

// ============================================================
// Stack Helpers
// ============================================================

bool TapscriptInterpreter::PopStack(ExecutionContext& ctx, std::vector<uint8_t>& out) {
    if (ctx.stack.empty()) {
        ctx.error = "Stack underflow";
        return false;
    }
    out = ctx.stack.back();
    ctx.stack.pop_back();
    return true;
}

bool TapscriptInterpreter::PushStack(ExecutionContext& ctx, const std::vector<uint8_t>& data) {
    // BIP342 inherits the combined main-stack + altstack 1,000-element limit.
    if (ctx.stack.size() + ctx.altstack.size() >= 1000) {
        ctx.error = "Stack size limit exceeded (BIP342: 1000 elements max)";
        return false;
    }

    // BIP342 Fix #2: Enforce maximum element size (520 bytes)
    if (data.size() > 520) {
        ctx.error = "Stack element size limit exceeded (BIP342: 520 bytes max)";
        return false;
    }

    ctx.stack.push_back(data);
    return true;
}

bool TapscriptInterpreter::CastToBool(const std::vector<uint8_t>& data) {
    // Empty vector is false
    if (data.empty()) {
        return false;
    }

    // Check for negative zero (0x80)
    if (data.size() == 1 && data[0] == 0x80) {
        return false;
    }

    // All other values are true
    // (Note: In full Bitcoin script, we'd check for all zeros except possibly 0x80 in last byte)
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] != 0) {
            // Non-zero byte found
            if (i == data.size() - 1 && data[i] == 0x80) {
                // Last byte is 0x80 (negative zero marker)
                return false;
            }
            return true;
        }
    }

    return false; // All zeros
}

// ============================================================
// Signature Verification
// ============================================================

bool TapscriptInterpreter::VerifySchnorrSignature(
    const std::vector<uint8_t>& signature,
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& sighash
) {
    // Validate inputs
    if (signature.size() != 64) return false;
    if (pubkey.size() != 32) return false;
    if (sighash.size() != 32) return false;

    // Shared secp256k1 verification context
    auto* ctx = dinero::crypto::GetSecp256k1ContextVerify();

    // Parse x-only public key (BIP340)
    secp256k1_xonly_pubkey xonly_pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &xonly_pubkey, pubkey.data())) {
        return false;
    }

    // Verify Schnorr signature (BIP340)
    int result = secp256k1_schnorrsig_verify(
        ctx,
        signature.data(),
        sighash.data(),
        32,
        &xonly_pubkey
    );

    return result == 1;
}

// ============================================================
// Phase L0.3: Covenant Opcode Handlers
// ============================================================

bool TapscriptInterpreter::OpCheckTemplateVerify(ExecutionContext& ctx) {
    // Check if CTV flag is enabled (consensus enforcement)
    if (!(ctx.flags & SCRIPT_VERIFY_CHECKTEMPLATEVERIFY)) {
        // NOT ENABLED: Fail explicitly (no silent fallbacks)
        ctx.error = "OP_CHECKTEMPLATEVERIFY not enabled (SCRIPT_VERIFY_CHECKTEMPLATEVERIFY flag not set)";
        return false;
    }
    if (++ctx.ctv_executions > MAX_CTV_OPS_PER_TAPSCRIPT) {
        ctx.error =
            "OP_CHECKTEMPLATEVERIFY: per-tapscript execution limit exceeded";
        return false;
    }

    // Stack: <32-byte template hash>
    if (ctx.stack.empty()) {
        ctx.error = "OP_CHECKTEMPLATEVERIFY: stack empty";
        return false;
    }

    const auto& expected_hash = ctx.stack.back();

    // BIP119 reserves all non-32-byte arguments as NOP behavior for future
    // upgrades. Nodes may discourage them as policy, but consensus succeeds.
    if (expected_hash.size() != 32) {
        if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
            ctx.error = "OP_CHECKTEMPLATEVERIFY: discouraged non-32-byte argument";
            return false;
        }
        return true;
    }

    // Need transaction context
    if (!ctx.tx) {
        ctx.error = "OP_CHECKTEMPLATEVERIFY: no transaction context";
        return false;
    }

    // Verify CTV template hash using covenant verification function
    if (!VerifyCTV(
            *ctx.tx,
            static_cast<uint32_t>(ctx.input_index),
            expected_hash,
            ctx.covenant_precomputed)) {
        ctx.error = "OP_CHECKTEMPLATEVERIFY: template hash verification failed";
        return false;
    }

    // Success: Leave hash on stack (NOP-like behavior for soft-fork compatibility)
    return true;
}

bool TapscriptInterpreter::OpCheckSigFromStack(ExecutionContext& ctx) {
    // Check if CSFS flag is enabled (consensus enforcement)
    if (!(ctx.flags & SCRIPT_VERIFY_CHECKSIGFROMSTACK)) {
        // NOT ENABLED: Fail explicitly (no silent fallbacks)
        ctx.error = "OP_CHECKSIGFROMSTACK not enabled (SCRIPT_VERIFY_CHECKSIGFROMSTACK flag not set)";
        return false;
    }

    // Stack: <sig> <msg> <pubkey> -> <result>
    if (ctx.stack.size() < 3) {
        ctx.error = "OP_CHECKSIGFROMSTACK: stack must have at least 3 elements";
        return false;
    }

    const auto& pubkey = ctx.stack[ctx.stack.size() - 1];
    const auto& msg = ctx.stack[ctx.stack.size() - 2];
    const auto& sig = ctx.stack[ctx.stack.size() - 3];

    // Verify signature using covenant verification function
    bool valid = VerifySignatureFromStack(sig, msg, pubkey);

    // Pop the three inputs
    ctx.stack.pop_back(); // pubkey
    ctx.stack.pop_back(); // msg
    ctx.stack.pop_back(); // sig

    // Push result (1 for valid, 0 for invalid)
    PushStack(ctx, valid ? std::vector<uint8_t>{0x01} : std::vector<uint8_t>{});

    return true;
}

bool TapscriptInterpreter::OpTxHash(ExecutionContext& ctx) {
    // Check if TXHASH flag is enabled (consensus enforcement)
    if (!(ctx.flags & SCRIPT_VERIFY_TXHASH)) {
        // NOT ENABLED: Fail explicitly (no silent fallbacks)
        ctx.error = "OP_TXHASH not enabled (SCRIPT_VERIFY_TXHASH flag not set)";
        return false;
    }

    // Stack: <flags> -> <32-byte hash>
    if (ctx.stack.empty()) {
        ctx.error = "OP_TXHASH: stack empty";
        return false;
    }

    const auto& flags_bytes = ctx.stack.back();
    ctx.stack.pop_back();

    // Flags must be exactly 4 bytes (uint32_t)
    if (flags_bytes.size() != 4) {
        ctx.error = "OP_TXHASH: flags must be 4 bytes, got " + std::to_string(flags_bytes.size());
        return false;
    }

    // Parse flags (little-endian)
    uint32_t txhash_flags = static_cast<uint32_t>(flags_bytes[0]) |
                            (static_cast<uint32_t>(flags_bytes[1]) << 8) |
                            (static_cast<uint32_t>(flags_bytes[2]) << 16) |
                            (static_cast<uint32_t>(flags_bytes[3]) << 24);

    // Need transaction context
    if (!ctx.tx) {
        ctx.error = "OP_TXHASH: no transaction context";
        return false;
    }

    // Compute transaction hash using covenant verification function
    auto hash = ComputeTxHash(*ctx.tx, static_cast<TxHashFlags>(txhash_flags), static_cast<uint32_t>(ctx.input_index));

    // Push hash to stack
    std::vector<uint8_t> hash_vec(hash.begin(), hash.end());
    return PushStack(ctx, hash_vec);  // BIP342: Check limits
}

// ============================================================
// Contract State Serialization/Deserialization
// ============================================================

namespace {

// Deserialize ContractState from bytes
// Format: stateHash(32) || codeHash(32) || counter(4) || dataLen(4) || data(variable)
bool DeserializeContractState(const std::vector<uint8_t>& bytes, ContractState& state) {
    if (bytes.size() < 72) {  // Minimum: 32 + 32 + 4 + 4 = 72 bytes
        return false;
    }

    size_t offset = 0;

    // stateHash (32 bytes)
    std::copy(bytes.begin() + offset, bytes.begin() + offset + 32, state.stateHash.begin());
    offset += 32;

    // codeHash (32 bytes)
    std::copy(bytes.begin() + offset, bytes.begin() + offset + 32, state.codeHash.begin());
    offset += 32;

    // counter (4 bytes, little-endian)
    state.counter = static_cast<uint32_t>(bytes[offset]) |
                    (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                    (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                    (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;

    // data length (4 bytes, little-endian)
    uint32_t dataLen = static_cast<uint32_t>(bytes[offset]) |
                       (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                       (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                       (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;

    // Verify remaining bytes match dataLen
    if (offset + dataLen != bytes.size()) {
        return false;
    }

    // data (variable length)
    state.data.assign(bytes.begin() + offset, bytes.end());

    return true;
}

} // anonymous namespace

// ============================================================
// Phase 3 Fix: Complete OP_CHECKCONTRACTVERIFY Implementation
// ============================================================

bool TapscriptInterpreter::OpCheckContractVerify(ExecutionContext& ctx) {
    // Check if CCV flag is enabled (consensus enforcement)
    if (!(ctx.flags & SCRIPT_VERIFY_CHECKCONTRACT)) {
        // NOT ENABLED: Fail explicitly (no silent fallbacks)
        ctx.error = "OP_CHECKCONTRACTVERIFY not enabled (SCRIPT_VERIFY_CHECKCONTRACT flag not set)";
        return false;
    }
    if (++ctx.ccv_executions > MAX_CCV_OPS_PER_TAPSCRIPT) {
        ctx.error =
            "OP_CHECKCONTRACTVERIFY: per-tapscript execution limit exceeded";
        return false;
    }

    // Stack: <prev_state_bytes> <new_state_bytes> -> (verify)
    if (ctx.stack.size() < 2) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: stack must have at least 2 elements";
        return false;
    }

    // Need transaction context
    if (!ctx.tx) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: no transaction context";
        return false;
    }

    // Copy before pop_back: retaining references to vector elements across a
    // pop is undefined behavior.
    const auto new_state_bytes = ctx.stack.back();
    ctx.stack.pop_back();
    const auto prev_state_bytes = ctx.stack.back();
    ctx.stack.pop_back();

    // Deserialize previous state
    ContractState prev_state;
    if (!DeserializeContractState(prev_state_bytes, prev_state)) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: failed to deserialize previous state";
        return false;
    }

    // Deserialize new state
    ContractState new_state;
    if (!DeserializeContractState(new_state_bytes, new_state)) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: failed to deserialize new state";
        return false;
    }

    if (!ctx.input_utxos || !ctx.tapscript ||
        !ctx.internal_key || !ctx.merkle_root) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: missing Taproot binding context";
        return false;
    }

    const ContractSpendContext spend_context{
        *ctx.input_utxos,
        *ctx.tapscript,
        *ctx.internal_key,
        *ctx.merkle_root,
        ctx.output_key_parity
    };
    if (!VerifyContractTransition(
            *ctx.tx, static_cast<uint32_t>(ctx.input_index),
            prev_state, new_state, spend_context)) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: state transition verification failed";
        return false;
    }

    // Success: contract state transition is valid
    return true;
}

} // namespace consensus
} // namespace dinero
