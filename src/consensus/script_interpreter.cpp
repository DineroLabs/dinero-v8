#include "consensus/script_interpreter.h"
#include "consensus/covenants.h"
#include "consensus/cpu_budget_monitor.h"
#include "primitives/transaction.h"
#include "common/sha256d.h"
#include <algorithm>
#include <cstring>

namespace dinero {
namespace consensus {

// ============================================================================
// Error String Conversion
// ============================================================================

const char* ScriptErrorString(ScriptError error) {
    switch (error) {
        case ScriptError::OK: return "OK";
        case ScriptError::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
        case ScriptError::EVAL_FALSE: return "EVAL_FALSE";
        case ScriptError::OP_RETURN: return "OP_RETURN";
        case ScriptError::STACK_SIZE: return "STACK_SIZE";
        case ScriptError::INVALID_STACK_OPERATION: return "INVALID_STACK_OPERATION";
        case ScriptError::INVALID_ALTSTACK_OPERATION: return "INVALID_ALTSTACK_OPERATION";
        case ScriptError::DISABLED_OPCODE: return "DISABLED_OPCODE";
        case ScriptError::BAD_OPCODE: return "BAD_OPCODE";
        case ScriptError::UNBALANCED_CONDITIONAL: return "UNBALANCED_CONDITIONAL";
        case ScriptError::PUSH_SIZE: return "PUSH_SIZE";
        case ScriptError::OP_COUNT: return "OP_COUNT";
        case ScriptError::INVALID_NUMBER_RANGE: return "INVALID_NUMBER_RANGE";
        case ScriptError::SIG_DER: return "SIG_DER";
        case ScriptError::SIG_HASHTYPE: return "SIG_HASHTYPE";
        case ScriptError::SIG_NULLDUMMY: return "SIG_NULLDUMMY";
        case ScriptError::SIG_NULLFAIL: return "SIG_NULLFAIL";
        case ScriptError::PUBKEYTYPE: return "PUBKEYTYPE";
        case ScriptError::WITNESS_PUBKEYTYPE: return "WITNESS_PUBKEYTYPE";
        case ScriptError::SIG_HIGH_S: return "SIG_HIGH_S";
        case ScriptError::SIG_PUSHONLY: return "SIG_PUSHONLY";
        case ScriptError::CHECKSIGVERIFY: return "CHECKSIGVERIFY";
        case ScriptError::CHECKMULTISIGVERIFY: return "CHECKMULTISIGVERIFY";
        case ScriptError::NEGATIVE_LOCKTIME: return "NEGATIVE_LOCKTIME";
        case ScriptError::UNSATISFIED_LOCKTIME: return "UNSATISFIED_LOCKTIME";
        case ScriptError::WITNESS_PROGRAM_WRONG_LENGTH: return "WITNESS_PROGRAM_WRONG_LENGTH";
        case ScriptError::WITNESS_PROGRAM_WITNESS_EMPTY: return "WITNESS_PROGRAM_WITNESS_EMPTY";
        case ScriptError::WITNESS_PROGRAM_MISMATCH: return "WITNESS_PROGRAM_MISMATCH";
        case ScriptError::WITNESS_MALLEATED: return "WITNESS_MALLEATED";
        case ScriptError::WITNESS_MALLEATED_P2SH: return "WITNESS_MALLEATED_P2SH";
        case ScriptError::WITNESS_UNEXPECTED: return "WITNESS_UNEXPECTED";
        case ScriptError::TAPROOT_WRONG_CONTROL_SIZE: return "TAPROOT_WRONG_CONTROL_SIZE";
        case ScriptError::TAPSCRIPT_VALIDATION_WEIGHT: return "TAPSCRIPT_VALIDATION_WEIGHT";
        case ScriptError::TAPSCRIPT_CHECKMULTISIG: return "TAPSCRIPT_CHECKMULTISIG";
        case ScriptError::TAPSCRIPT_MINIMALIF: return "TAPSCRIPT_MINIMALIF";
        case ScriptError::DISCOURAGE_UPGRADABLE_NOPS: return "DISCOURAGE_UPGRADABLE_NOPS";
        case ScriptError::DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM: return "DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM";
        case ScriptError::DISCOURAGE_UPGRADABLE_TAPROOT_VERSION: return "DISCOURAGE_UPGRADABLE_TAPROOT_VERSION";
        case ScriptError::MINIMALDATA: return "MINIMALDATA";
        case ScriptError::CLEANSTACK: return "CLEANSTACK";
        case ScriptError::MINIMALIF: return "MINIMALIF";
        // Phase 28: Covenant errors
        case ScriptError::CTV_WRONG_LENGTH: return "CTV_WRONG_LENGTH";
        case ScriptError::CTV_VERIFY_FAILED: return "CTV_VERIFY_FAILED";
        case ScriptError::CSFS_WRONG_SIG_SIZE: return "CSFS_WRONG_SIG_SIZE";
        case ScriptError::CSFS_WRONG_PUBKEY_SIZE: return "CSFS_WRONG_PUBKEY_SIZE";
        case ScriptError::CSFS_VERIFY_FAILED: return "CSFS_VERIFY_FAILED";
        case ScriptError::TXHASH_INVALID_FLAGS: return "TXHASH_INVALID_FLAGS";
        case ScriptError::CCV_INVALID_STATE: return "CCV_INVALID_STATE";
        case ScriptError::CCV_VERIFY_FAILED: return "CCV_VERIFY_FAILED";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// Stack Helper Functions
// ============================================================================

static bool CastToBool(const std::vector<uint8_t>& vch) {
    for (size_t i = 0; i < vch.size(); i++) {
        if (vch[i] != 0) {
            // Last byte can be 0x80 (negative zero)
            if (i == vch.size() - 1 && vch[i] == 0x80) {
                return false;
            }
            return true;
        }
    }
    return false;
}

static bool CheckMinimalPush(const std::vector<uint8_t>& data, opcodetype opcode) {
    if (data.size() == 0) {
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

// Maximum number of bytes for arithmetic operands (4 bytes = int32_t range)
static const size_t MAX_SCRIPT_NUM_LENGTH = 4;

// Check if a script number is within valid range for arithmetic operations
static bool CheckScriptNumRange(const std::vector<uint8_t>& data) {
    return data.size() <= MAX_SCRIPT_NUM_LENGTH;
}

// Check if a script number is valid for arithmetic operations with MINIMALDATA enforcement
static bool CheckArithmeticOperand(const std::vector<uint8_t>& data, uint32_t flags) {
    // Always check range
    if (!CheckScriptNumRange(data)) {
        return false;
    }
    // Check minimal encoding if MINIMALDATA flag is set
    if ((flags & SCRIPT_VERIFY_MINIMALDATA) && !isMinimallyEncoded(data, MAX_SCRIPT_NUM_LENGTH)) {
        return false;
    }
    return true;
}

// Check if a public key has valid encoding (for STRICTENC)
// Valid formats:
//   - Compressed: 33 bytes starting with 0x02 or 0x03
//   - Uncompressed: 65 bytes starting with 0x04
static bool IsValidPubKeyEncoding(const std::vector<uint8_t>& pubkey) {
    if (pubkey.size() == 33) {
        // Compressed public key: 0x02 or 0x03 prefix
        return pubkey[0] == 0x02 || pubkey[0] == 0x03;
    } else if (pubkey.size() == 65) {
        // Uncompressed public key: 0x04 prefix
        return pubkey[0] == 0x04;
    }
    return false;
}

// ============================================================================
// Script Evaluation (Core VM)
// ============================================================================

bool EvalScript(
    const Script& script,
    std::vector<std::vector<uint8_t>>& stack,
    const ScriptExecutionContext& ctx,
    ScriptError& error,
    CPUBudgetMonitor* cpu_monitor
) {
    // Phase E.3: Script validation CPU budget tracking
    ScopedCPUBudget cpu_budget(cpu_monitor, ScopedCPUBudget::Operation::SCRIPT_VALIDATION);
    static const size_t MAX_SCRIPT_SIZE = 10000;
    static const size_t MAX_STACK_SIZE = 1000;
    static const size_t MAX_SCRIPT_ELEMENT_SIZE = 520;
    static const size_t MAX_OPS_PER_SCRIPT = 201;

    if (script.size() > MAX_SCRIPT_SIZE) {
        error = ScriptError::STACK_SIZE;
        return false;
    }

    std::vector<std::vector<uint8_t>> altstack;
    std::vector<bool> vfExec;  // Execution flow (IF/ELSE/ENDIF)
    size_t nOpCount = 0;

    const uint8_t* pc = script.begin();
    const uint8_t* pend = script.end();
    const uint8_t* pbegincodehash = script.begin();

    error = ScriptError::OK;

    auto stacktop = [&](int i) -> std::vector<uint8_t>& {
        return stack[stack.size() + i];
    };

    while (pc < pend) {
        bool fExec = std::find(vfExec.begin(), vfExec.end(), false) == vfExec.end();

        // Read instruction
        opcodetype opcode = static_cast<opcodetype>(*pc++);

        // Check for disabled opcodes
        if (isOpcodeDisabled(opcode)) {
            error = ScriptError::DISABLED_OPCODE;
            return false;
        }

        // Handle data push opcodes
        std::vector<uint8_t> vchPushValue;
        if (opcode <= OP_PUSHDATA4) {
            size_t nSize = 0;
            if (opcode < OP_PUSHDATA1) {
                nSize = opcode;
            } else if (opcode == OP_PUSHDATA1) {
                if (pend - pc < 1) {
                    error = ScriptError::BAD_OPCODE;
                    return false;
                }
                nSize = *pc++;
            } else if (opcode == OP_PUSHDATA2) {
                if (pend - pc < 2) {
                    error = ScriptError::BAD_OPCODE;
                    return false;
                }
                nSize = pc[0] | (pc[1] << 8);
                pc += 2;
            } else if (opcode == OP_PUSHDATA4) {
                if (pend - pc < 4) {
                    error = ScriptError::BAD_OPCODE;
                    return false;
                }
                nSize = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
            }

            if (static_cast<size_t>(pend - pc) < nSize) {
                error = ScriptError::BAD_OPCODE;
                return false;
            }

            vchPushValue.assign(pc, pc + nSize);
            pc += nSize;

            if (vchPushValue.size() > MAX_SCRIPT_ELEMENT_SIZE) {
                error = ScriptError::PUSH_SIZE;
                return false;
            }

            if (fExec) {
                // Check minimal push requirement (only for executed code paths)
                if ((ctx.flags & SCRIPT_VERIFY_MINIMALDATA) && !CheckMinimalPush(vchPushValue, opcode)) {
                    error = ScriptError::MINIMALDATA;
                    return false;
                }

                stack.push_back(vchPushValue);
                if (stack.size() + altstack.size() > MAX_STACK_SIZE) {
                    error = ScriptError::STACK_SIZE;
                    return false;
                }
            }
            continue;
        }

        // Count operations
        if (opcode > OP_16) {
            ++nOpCount;
            if (nOpCount > MAX_OPS_PER_SCRIPT) {
                error = ScriptError::OP_COUNT;
                return false;
            }

            // Phase E.3: Check CPU budget timeout (every 10 opcodes for performance)
            if (nOpCount % 10 == 0 && cpu_budget.isTimedOut()) {
                error = ScriptError::OP_COUNT;  // Reuse OP_COUNT error for timeout
                return false;
            }
        }

        if (!fExec && !(opcode >= OP_IF && opcode <= OP_ENDIF)) {
            continue;
        }

        // Execute opcode
        switch (opcode) {
            // Small integer push opcodes
            case OP_1NEGATE:
            case OP_1:
            case OP_2:
            case OP_3:
            case OP_4:
            case OP_5:
            case OP_6:
            case OP_7:
            case OP_8:
            case OP_9:
            case OP_10:
            case OP_11:
            case OP_12:
            case OP_13:
            case OP_14:
            case OP_15:
            case OP_16: {
                // OP_1NEGATE = -1, OP_1 = 1, ..., OP_16 = 16
                int64_t n = (opcode == OP_1NEGATE) ? -1 : (opcode - OP_1 + 1);
                stack.push_back(scriptNumEncode(n));
                break;
            }

            case OP_RESERVED:
            case OP_VER:
            case OP_RESERVED1:
            case OP_RESERVED2: {
                // These are invalid only when executed
                if (fExec) {
                    error = ScriptError::BAD_OPCODE;
                    return false;
                }
                break;
            }

            // Flow control
            case OP_IF:
            case OP_NOTIF: {
                bool fValue = false;
                if (fExec) {
                    if (stack.size() < 1) {
                        error = ScriptError::INVALID_STACK_OPERATION;
                        return false;
                    }
                    std::vector<uint8_t> vch = stack.back();
                    stack.pop_back();

                    // Check minimal IF requirement (BIP 141)
                    // MINIMALIF is only enforced in witness script execution (SegWit v0)
                    // In witness mode, argument must be exactly empty [] or [0x01]
                    if ((ctx.flags & SCRIPT_VERIFY_MINIMALIF) && ctx.is_witness_v0) {
                        // Witness MINIMALIF: must be exactly [] (false) or [0x01] (true)
                        if (vch.size() > 1) {
                            error = ScriptError::MINIMALIF;
                            return false;
                        }
                        if (vch.size() == 1 && vch[0] != 1) {
                            error = ScriptError::MINIMALIF;
                            return false;
                        }
                    }

                    fValue = CastToBool(vch);
                    if (opcode == OP_NOTIF) {
                        fValue = !fValue;
                    }
                }
                vfExec.push_back(fValue);
                break;
            }

            case OP_ELSE: {
                if (vfExec.empty()) {
                    error = ScriptError::UNBALANCED_CONDITIONAL;
                    return false;
                }
                vfExec.back() = !vfExec.back();
                break;
            }

            case OP_ENDIF: {
                if (vfExec.empty()) {
                    error = ScriptError::UNBALANCED_CONDITIONAL;
                    return false;
                }
                vfExec.pop_back();
                break;
            }

            case OP_VERIFY: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                bool fValue = CastToBool(stack.back());
                if (fValue) {
                    stack.pop_back();
                } else {
                    error = ScriptError::EVAL_FALSE;
                    return false;
                }
                break;
            }

            case OP_RETURN: {
                error = ScriptError::OP_RETURN;
                return false;
            }

            // Stack operations
            case OP_TOALTSTACK: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                altstack.push_back(stack.back());
                stack.pop_back();
                break;
            }

            case OP_FROMALTSTACK: {
                if (altstack.size() < 1) {
                    error = ScriptError::INVALID_ALTSTACK_OPERATION;
                    return false;
                }
                stack.push_back(altstack.back());
                altstack.pop_back();
                break;
            }

            case OP_2DROP: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.pop_back();
                stack.pop_back();
                break;
            }

            case OP_2DUP: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch1 = stacktop(-2);
                std::vector<uint8_t> vch2 = stacktop(-1);
                stack.push_back(vch1);
                stack.push_back(vch2);
                break;
            }

            case OP_3DUP: {
                if (stack.size() < 3) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch1 = stacktop(-3);
                std::vector<uint8_t> vch2 = stacktop(-2);
                std::vector<uint8_t> vch3 = stacktop(-1);
                stack.push_back(vch1);
                stack.push_back(vch2);
                stack.push_back(vch3);
                break;
            }

            case OP_DROP: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.pop_back();
                break;
            }

            case OP_DUP: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.push_back(stack.back());
                break;
            }

            case OP_SWAP: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::swap(stacktop(-2), stacktop(-1));
                break;
            }

            case OP_NIP: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.erase(stack.end() - 2);
                break;
            }

            case OP_OVER: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.push_back(stacktop(-2));
                break;
            }

            case OP_PICK: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                // MINIMALDATA check for stack operand
                if ((ctx.flags & SCRIPT_VERIFY_MINIMALDATA) && !isMinimallyEncoded(stack.back(), MAX_SCRIPT_NUM_LENGTH)) {
                    error = ScriptError::MINIMALDATA;
                    return false;
                }
                int64_t n = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                if (n < 0 || static_cast<size_t>(n) >= stack.size()) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.push_back(stacktop(-1 - n));
                break;
            }

            case OP_ROLL: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                // MINIMALDATA check for stack operand
                if ((ctx.flags & SCRIPT_VERIFY_MINIMALDATA) && !isMinimallyEncoded(stack.back(), MAX_SCRIPT_NUM_LENGTH)) {
                    error = ScriptError::MINIMALDATA;
                    return false;
                }
                int64_t n = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                if (n < 0 || static_cast<size_t>(n) >= stack.size()) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stacktop(-1 - n);
                stack.erase(stack.end() - 1 - n);
                stack.push_back(vch);
                break;
            }

            case OP_ROT: {
                if (stack.size() < 3) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::swap(stacktop(-3), stacktop(-2));
                std::swap(stacktop(-2), stacktop(-1));
                break;
            }

            case OP_TUCK: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stack.back();
                stack.insert(stack.end() - 2, vch);
                break;
            }

            case OP_2OVER: {
                if (stack.size() < 4) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.push_back(stacktop(-4));
                stack.push_back(stacktop(-4));
                break;
            }

            case OP_2ROT: {
                if (stack.size() < 6) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch1 = stacktop(-6);
                std::vector<uint8_t> vch2 = stacktop(-5);
                stack.erase(stack.end() - 6);
                stack.erase(stack.end() - 5);
                stack.push_back(vch1);
                stack.push_back(vch2);
                break;
            }

            case OP_2SWAP: {
                if (stack.size() < 4) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::swap(stacktop(-4), stacktop(-2));
                std::swap(stacktop(-3), stacktop(-1));
                break;
            }

            case OP_IFDUP: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (CastToBool(stack.back())) {
                    stack.push_back(stack.back());
                }
                break;
            }

            case OP_DEPTH: {
                stack.push_back(scriptNumEncode(static_cast<int64_t>(stack.size())));
                break;
            }

            case OP_SIZE: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                stack.push_back(scriptNumEncode(static_cast<int64_t>(stack.back().size())));
                break;
            }

            // Crypto
            case OP_RIPEMD160: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stack.back();
                stack.pop_back();
                stack.push_back(RIPEMD160_Hash(vch));
                break;
            }

            case OP_SHA1: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stack.back();
                stack.pop_back();
                stack.push_back(SHA1_Hash(vch));
                break;
            }

            case OP_SHA256: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stack.back();
                stack.pop_back();
                stack.push_back(SHA256_Hash(vch));
                break;
            }

            case OP_HASH160: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stack.back();
                stack.pop_back();
                stack.push_back(HASH160_Hash(vch));
                break;
            }

            case OP_HASH256: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch = stack.back();
                stack.pop_back();
                stack.push_back(HASH256_Hash(vch));
                break;
            }

            case OP_CHECKSIG:
            case OP_CHECKSIGVERIFY: {
                // Phase 26: Optimized CHECKSIG implementation
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vchPubKey = stack.back();
                stack.pop_back();
                std::vector<uint8_t> vchSig = stack.back();
                stack.pop_back();

                bool fSuccess = false;

                // Fast path: Empty signature always fails (no further checks needed)
                if (vchSig.empty()) {
                    // Empty sig: just push false, no NULLFAIL issue
                    stack.push_back(std::vector<uint8_t>{});
                    if (opcode == OP_CHECKSIGVERIFY) {
                        error = ScriptError::CHECKSIGVERIFY;
                        return false;
                    }
                    break;
                }

                // BIP 66: Check DER encoding (DERSIG or STRICTENC)
                if (ctx.flags & (SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_STRICTENC)) {
                    if (!IsValidSignatureEncoding(vchSig)) {
                        error = ScriptError::SIG_DER;
                        return false;
                    }
                }

                // STRICTENC: Check pubkey encoding
                if (ctx.flags & SCRIPT_VERIFY_STRICTENC) {
                    if (!IsValidPubKeyEncoding(vchPubKey)) {
                        error = ScriptError::PUBKEYTYPE;
                        return false;
                    }
                }

                // Extract hash type from signature
                uint8_t hash_type = vchSig.back();

                // STRICTENC: Validate hash type (must be valid SIGHASH_*)
                if (ctx.flags & SCRIPT_VERIFY_STRICTENC) {
                    uint8_t base_type = hash_type & ~SIGHASH_ANYONECANPAY;
                    if (base_type < SIGHASH_ALL || base_type > SIGHASH_SINGLE) {
                        error = ScriptError::SIG_HASHTYPE;
                        return false;
                    }
                }

                // Compute sighash based on script type
                Script script_code(script.begin() + (pbegincodehash - script.begin()), script.end());
                std::vector<uint8_t> sighash;
                if (ctx.is_witness_v0) {
                    // BIP 143: Use witness sighash for SegWit v0 (P2WSH)
                    sighash = SignatureHashWitness(script_code, ctx, hash_type);
                } else {
                    // Legacy sighash for non-witness scripts
                    sighash = SignatureHashLegacy(script_code, ctx, hash_type);
                }

                // Verify signature
                fSuccess = CheckECDSASignature(vchSig, vchPubKey, sighash, ctx.flags);

                if (!fSuccess && (ctx.flags & SCRIPT_VERIFY_NULLFAIL)) {
                    error = ScriptError::SIG_NULLFAIL;
                    return false;
                }

                stack.push_back(fSuccess ? std::vector<uint8_t>{1} : std::vector<uint8_t>{});

                if (opcode == OP_CHECKSIGVERIFY) {
                    if (!fSuccess) {
                        error = ScriptError::CHECKSIGVERIFY;
                        return false;
                    }
                    stack.pop_back();
                }
                break;
            }

            case OP_CHECKMULTISIG:
            case OP_CHECKMULTISIGVERIFY: {
                // Multisig verification: m-of-n signatures
                // Format: <sig1> ... <sigM> <m> <pubkey1> ... <pubkeyN> <n> OP_CHECKMULTISIG

                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // Get n (number of pubkeys)
                // MINIMALDATA check for n operand
                if ((ctx.flags & SCRIPT_VERIFY_MINIMALDATA) && !isMinimallyEncoded(stack.back(), MAX_SCRIPT_NUM_LENGTH)) {
                    error = ScriptError::MINIMALDATA;
                    return false;
                }
                int64_t n = scriptNumDecode(stack.back(), false);
                stack.pop_back();

                if (n < 0 || n > 20) {
                    error = ScriptError::PUBKEYTYPE;
                    return false;
                }

                size_t ikey = static_cast<size_t>(n);
                nOpCount += static_cast<size_t>(n);
                if (nOpCount > MAX_OPS_PER_SCRIPT) {
                    error = ScriptError::OP_COUNT;
                    return false;
                }

                if (stack.size() < ikey) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // Get pubkeys
                std::vector<std::vector<uint8_t>> vchPubKeys;
                for (size_t i = 0; i < ikey; i++) {
                    vchPubKeys.push_back(stack.back());
                    stack.pop_back();
                }

                // Get m (number of required signatures)
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // MINIMALDATA check for m operand
                if ((ctx.flags & SCRIPT_VERIFY_MINIMALDATA) && !isMinimallyEncoded(stack.back(), MAX_SCRIPT_NUM_LENGTH)) {
                    error = ScriptError::MINIMALDATA;
                    return false;
                }
                int64_t m = scriptNumDecode(stack.back(), false);
                stack.pop_back();

                if (m < 0 || m > n) {
                    error = ScriptError::PUBKEYTYPE;
                    return false;
                }

                size_t isig = static_cast<size_t>(m);
                if (stack.size() < isig) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // Get signatures
                std::vector<std::vector<uint8_t>> vchSigs;
                for (size_t i = 0; i < isig; i++) {
                    vchSigs.push_back(stack.back());
                    stack.pop_back();
                }

                // Remove dummy element (Bitcoin Core bug compatibility)
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                std::vector<uint8_t> dummy = stack.back();
                stack.pop_back();

                // Check dummy is null (BIP 147)
                if ((ctx.flags & SCRIPT_VERIFY_NULLDUMMY) && !dummy.empty()) {
                    error = ScriptError::SIG_NULLDUMMY;
                    return false;
                }

                // Verify signatures
                bool fSuccess = true;
                size_t ikey2 = 0;
                size_t isig2 = 0;

                // NOTE: Unlike CHECKSIG, CHECKMULTISIG only DER-checks each signature
                // when it's actually being tried against a pubkey, not all upfront.
                // This allows scripts like "sig empty CHECKMULTISIG NOT" to succeed
                // even if sig has invalid DER, as long as empty fails first.

                while (fSuccess && isig2 < isig) {
                    const std::vector<uint8_t>& vchSig = vchSigs[isig2];
                    const std::vector<uint8_t>& vchPubKey = vchPubKeys[ikey2];

                    bool fOk = false;
                    if (!vchSig.empty()) {
                        // BIP 66: Check DER encoding when attempting to verify this signature
                        if (ctx.flags & (SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_STRICTENC)) {
                            if (!IsValidSignatureEncoding(vchSig)) {
                                error = ScriptError::SIG_DER;
                                return false;
                            }
                        }

                        // STRICTENC: Check pubkey encoding when actually checking against this pubkey
                        if (ctx.flags & SCRIPT_VERIFY_STRICTENC) {
                            if (!IsValidPubKeyEncoding(vchPubKey)) {
                                error = ScriptError::PUBKEYTYPE;
                                return false;
                            }
                        }

                        // WITNESS_PUBKEYTYPE: In witness scripts, pubkeys we attempt to use must be compressed
                        // This check happens when we ATTEMPT verification (not just on success)
                        if ((ctx.flags & SCRIPT_VERIFY_WITNESS_PUBKEYTYPE) && ctx.is_witness_v0) {
                            if (vchPubKey.size() != 33 || (vchPubKey[0] != 0x02 && vchPubKey[0] != 0x03)) {
                                error = ScriptError::WITNESS_PUBKEYTYPE;
                                return false;
                            }
                        }
                        uint8_t hash_type = vchSig.back();
                        Script script_code_copy(script.begin() + (pbegincodehash - script.begin()), script.end());
                        std::vector<uint8_t> sighash;
                        if (ctx.is_witness_v0) {
                            // BIP 143: Use witness sighash for SegWit v0 (P2WSH)
                            sighash = SignatureHashWitness(script_code_copy, ctx, hash_type);
                        } else {
                            // Legacy sighash for non-witness scripts
                            sighash = SignatureHashLegacy(script_code_copy, ctx, hash_type);
                        }
                        fOk = CheckECDSASignature(vchSig, vchPubKey, sighash, ctx.flags);
                    }

                    if (fOk) {
                        isig2++;
                    }
                    ikey2++;

                    // If there are more signatures than keys left, fail
                    if (isig - isig2 > ikey - ikey2) {
                        fSuccess = false;
                    }
                }

                // NULLFAIL: All signatures must be valid or empty
                if (!fSuccess && (ctx.flags & SCRIPT_VERIFY_NULLFAIL)) {
                    for (const auto& sig : vchSigs) {
                        if (!sig.empty()) {
                            error = ScriptError::SIG_NULLFAIL;
                            return false;
                        }
                    }
                }

                stack.push_back(fSuccess ? std::vector<uint8_t>{1} : std::vector<uint8_t>{});

                if (opcode == OP_CHECKMULTISIGVERIFY) {
                    if (!fSuccess) {
                        error = ScriptError::CHECKMULTISIGVERIFY;
                        return false;
                    }
                    stack.pop_back();
                }
                break;
            }

            case OP_EQUAL:
            case OP_EQUALVERIFY: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                std::vector<uint8_t> vch1 = stacktop(-2);
                std::vector<uint8_t> vch2 = stacktop(-1);
                bool fEqual = (vch1 == vch2);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(fEqual ? std::vector<uint8_t>{1} : std::vector<uint8_t>{});
                if (opcode == OP_EQUALVERIFY) {
                    if (!fEqual) {
                        error = ScriptError::EVAL_FALSE;
                        return false;
                    }
                    stack.pop_back();
                }
                break;
            }

            // Numeric operations
            case OP_1ADD: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stack.back(), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn + 1));
                break;
            }

            case OP_1SUB: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stack.back(), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn - 1));
                break;
            }

            case OP_ADD: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn1 + bn2));
                break;
            }

            case OP_SUB: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn1 - bn2));
                break;
            }

            case OP_NEGATE: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stack.back(), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                stack.push_back(scriptNumEncode(-bn));
                break;
            }

            case OP_ABS: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stack.back(), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn < 0 ? -bn : bn));
                break;
            }

            case OP_NOT: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stack.back(), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn == 0 ? 1 : 0));
                break;
            }

            case OP_0NOTEQUAL: {
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stack.back(), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn = scriptNumDecode(stack.back(), false);
                stack.pop_back();
                stack.push_back(scriptNumEncode(bn != 0 ? 1 : 0));
                break;
            }

            case OP_BOOLAND: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 != 0 && bn2 != 0) ? 1 : 0));
                break;
            }

            case OP_BOOLOR: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 != 0 || bn2 != 0) ? 1 : 0));
                break;
            }

            case OP_NUMEQUAL:
            case OP_NUMEQUALVERIFY: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                bool fEqual = (bn1 == bn2);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode(fEqual ? 1 : 0));
                if (opcode == OP_NUMEQUALVERIFY) {
                    if (!fEqual) {
                        error = ScriptError::EVAL_FALSE;
                        return false;
                    }
                    stack.pop_back();
                }
                break;
            }

            case OP_NUMNOTEQUAL: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 != bn2) ? 1 : 0));
                break;
            }

            case OP_LESSTHAN: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 < bn2) ? 1 : 0));
                break;
            }

            case OP_GREATERTHAN: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 > bn2) ? 1 : 0));
                break;
            }

            case OP_LESSTHANOREQUAL: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 <= bn2) ? 1 : 0));
                break;
            }

            case OP_GREATERTHANOREQUAL: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 >= bn2) ? 1 : 0));
                break;
            }

            case OP_MIN: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 < bn2) ? bn1 : bn2));
                break;
            }

            case OP_MAX: {
                if (stack.size() < 2) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-2), false);
                int64_t bn2 = scriptNumDecode(stacktop(-1), false);
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn1 > bn2) ? bn1 : bn2));
                break;
            }

            case OP_WITHIN: {
                if (stack.size() < 3) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }
                if (!CheckArithmeticOperand(stacktop(-3), ctx.flags) || !CheckArithmeticOperand(stacktop(-2), ctx.flags) || !CheckArithmeticOperand(stacktop(-1), ctx.flags)) {
                    error = ScriptError::UNKNOWN_ERROR;
                    return false;
                }
                int64_t bn1 = scriptNumDecode(stacktop(-3), false);  // x
                int64_t bn2 = scriptNumDecode(stacktop(-2), false);  // min
                int64_t bn3 = scriptNumDecode(stacktop(-1), false);  // max
                stack.pop_back();
                stack.pop_back();
                stack.pop_back();
                stack.push_back(scriptNumEncode((bn2 <= bn1 && bn1 < bn3) ? 1 : 0));
                break;
            }

            case OP_CODESEPARATOR: {
                pbegincodehash = pc;
                break;
            }

            // OP_NOP - Original NOP, always allowed
            case OP_NOP: {
                break;
            }

            // Upgradable NOPs - discouraged for future soft-fork upgrades
            // Note: OP_NOP4 is now OP_CHECKTEMPLATEVERIFY (Phase 28 covenant)
            case OP_NOP1:
            case OP_NOP5:
            case OP_NOP6:
            case OP_NOP7:
            case OP_NOP8:
            case OP_NOP9:
            case OP_NOP10: {
                if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                    error = ScriptError::DISCOURAGE_UPGRADABLE_NOPS;
                    return false;
                }
                break;
            }

            case OP_CHECKLOCKTIMEVERIFY: {
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)) {
                    break;  // Treated as NOP if flag not set
                }

                // BIP 65: CHECKLOCKTIMEVERIFY
                // Fails if stack is empty
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // Top stack value must be >= 0
                int64_t nLockTime = scriptNumDecode(stack.back(), false);
                if (nLockTime < 0) {
                    error = ScriptError::NEGATIVE_LOCKTIME;
                    return false;
                }

                // Check that transaction locktime is set
                if (!ctx.tx) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                // Transaction locktime must match type (height vs. time)
                // If both < 500000000: compare as block heights
                // If both >= 500000000: compare as timestamps
                bool fLockTimeIsBlockHeight = (nLockTime < 500000000);
                bool fTxLockTimeIsBlockHeight = (ctx.tx->lockTime < 500000000);

                if (fLockTimeIsBlockHeight != fTxLockTimeIsBlockHeight) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                // Transaction locktime must be >= stack value
                if (static_cast<uint32_t>(nLockTime) > ctx.tx->lockTime) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                // Transaction input sequence must be < 0xFFFFFFFF (final)
                if (ctx.input_index < ctx.tx->vin.size()) {
                    if (ctx.tx->vin[ctx.input_index].sequence == 0xFFFFFFFF) {
                        error = ScriptError::UNSATISFIED_LOCKTIME;
                        return false;
                    }
                }

                // Leave value on stack (OP_NOP-like behavior)
                break;
            }

            case OP_CHECKSEQUENCEVERIFY: {
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKSEQUENCEVERIFY)) {
                    break;  // Treated as NOP if flag not set
                }

                // BIP 112: CHECKSEQUENCEVERIFY
                // Fails if stack is empty
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // Top stack value must be >= 0
                int64_t nSequence = scriptNumDecode(stack.back(), false);
                if (nSequence < 0) {
                    error = ScriptError::NEGATIVE_LOCKTIME;
                    return false;
                }

                // Disable bit (bit 31) must not be set
                if ((nSequence & (1ULL << 31)) != 0) {
                    // Sequence is disabled, always passes
                    break;
                }

                // Check transaction input sequence
                if (!ctx.tx || ctx.input_index >= ctx.tx->vin.size()) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                uint32_t txSequence = ctx.tx->vin[ctx.input_index].sequence;

                // Disable bit must not be set in transaction
                if ((txSequence & (1U << 31)) != 0) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                // Type bit (bit 22) must match
                uint32_t nSequenceType = nSequence & (1 << 22);
                uint32_t txSequenceType = txSequence & (1 << 22);

                if (nSequenceType != txSequenceType) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                // Masked comparison (lower 16 bits for locktime value)
                uint32_t nSequenceMasked = nSequence & 0xFFFF;
                uint32_t txSequenceMasked = txSequence & 0xFFFF;

                if (txSequenceMasked < nSequenceMasked) {
                    error = ScriptError::UNSATISFIED_LOCKTIME;
                    return false;
                }

                // Leave value on stack (OP_NOP-like behavior)
                break;
            }

            // ================================================================
            // Phase 28: Covenant Opcodes
            // ================================================================

            case OP_CHECKTEMPLATEVERIFY: {
                // BIP-119 style: OP_CHECKTEMPLATEVERIFY (CTV)
                // If flag not set, treat as NOP4 (soft-fork compatible)
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKTEMPLATEVERIFY)) {
                    if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                        error = ScriptError::DISCOURAGE_UPGRADABLE_NOPS;
                        return false;
                    }
                    break;
                }

                // Stack: <32-byte template hash>
                if (stack.size() < 1) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                // BIP119: non-32-byte arguments are reserved NOP behavior.
                if (stack.back().size() != 32) {
                    if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                        error = ScriptError::DISCOURAGE_UPGRADABLE_NOPS;
                        return false;
                    }
                    break;
                }

                // Need transaction context
                if (!ctx.tx) {
                    error = ScriptError::CTV_VERIFY_FAILED;
                    return false;
                }

                // Verify CTV template hash
                if (!VerifyCTV(
                        *ctx.tx, ctx.input_index, stack.back(),
                        ctx.covenant_precomputed)) {
                    error = ScriptError::CTV_VERIFY_FAILED;
                    return false;
                }

                // Leave hash on stack (NOP-like behavior for soft-fork compat)
                break;
            }

            case OP_CHECKSIGFROMSTACK: {
                // CSFS: Verify Schnorr signature over arbitrary message
                // If flag not set, fail (not NOP)
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKSIGFROMSTACK)) {
                    error = ScriptError::BAD_OPCODE;
                    return false;
                }

                // Stack: <sig> <msg> <pubkey> -> <result>
                if (stack.size() < 3) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                const auto& pubkey = stack[stack.size() - 1];
                const auto& msg = stack[stack.size() - 2];
                const auto& sig = stack[stack.size() - 3];

                // Validate sizes
                if (sig.size() != 64) {
                    error = ScriptError::CSFS_WRONG_SIG_SIZE;
                    return false;
                }
                if (pubkey.size() != 32) {
                    error = ScriptError::CSFS_WRONG_PUBKEY_SIZE;
                    return false;
                }

                // Verify signature
                bool valid = VerifySignatureFromStack(sig, msg, pubkey);

                // Pop 3 items
                stack.pop_back();
                stack.pop_back();
                stack.pop_back();

                // Push result
                stack.push_back(valid ? std::vector<uint8_t>{1} : std::vector<uint8_t>{});
                break;
            }

            case OP_CHECKSIGFROMSTACKVERIFY: {
                // CSFSVERIFY: Same as CSFS but with VERIFY semantics
                if (!(ctx.flags & SCRIPT_VERIFY_CHECKSIGFROMSTACK)) {
                    error = ScriptError::BAD_OPCODE;
                    return false;
                }

                // Stack: <sig> <msg> <pubkey>
                if (stack.size() < 3) {
                    error = ScriptError::INVALID_STACK_OPERATION;
                    return false;
                }

                const auto& pubkey = stack[stack.size() - 1];
                const auto& msg = stack[stack.size() - 2];
                const auto& sig = stack[stack.size() - 3];

                // Validate sizes
                if (sig.size() != 64) {
                    error = ScriptError::CSFS_WRONG_SIG_SIZE;
                    return false;
                }
                if (pubkey.size() != 32) {
                    error = ScriptError::CSFS_WRONG_PUBKEY_SIZE;
                    return false;
                }

                // Verify signature
                if (!VerifySignatureFromStack(sig, msg, pubkey)) {
                    error = ScriptError::CSFS_VERIFY_FAILED;
                    return false;
                }

                // Pop 3 items (verification passed)
                stack.pop_back();
                stack.pop_back();
                stack.pop_back();
                break;
            }

            case OP_TXHASH: {
                // OP_TXHASH is a tapscript-only covenant opcode.
                // The tapscript interpreter (tapscript_interpreter.cpp) implements it
                // with 4-byte LE flags. This legacy interpreter used 1-byte flags —
                // a divergent semantic that no covenant output should rely on.
                // Force BAD_OPCODE here unconditionally: covenant outputs must be
                // spent through the tapscript path.
                error = ScriptError::BAD_OPCODE;
                return false;
            }

            case OP_CHECKCONTRACTVERIFY: {
                // OP_CHECKCONTRACTVERIFY is a tapscript-only covenant opcode.
                // The tapscript interpreter (tapscript_interpreter.cpp) implements it
                // consuming 2 serialized ContractState blobs. This legacy interpreter
                // previously consumed 4 separate scalar items — a divergent semantic.
                // Force BAD_OPCODE here unconditionally: covenant outputs must be
                // spent through the tapscript path.
                error = ScriptError::BAD_OPCODE;
                return false;
            }

            default: {
                error = ScriptError::BAD_OPCODE;
                return false;
            }
        }

        if (stack.size() + altstack.size() > MAX_STACK_SIZE) {
            error = ScriptError::STACK_SIZE;
            return false;
        }
    }

    if (!vfExec.empty()) {
        error = ScriptError::UNBALANCED_CONDITIONAL;
        return false;
    }

    return true;
}

// ============================================================================
// Script Verification (Main Entry Point)
// ============================================================================

bool VerifyScript(
    const Script& scriptSig,
    const Script& scriptPubKey,
    const std::vector<std::vector<uint8_t>>& witness,
    const ScriptExecutionContext& ctx,
    ScriptError& error
) {
    // SIGPUSHONLY: scriptSig must contain only push operations
    if ((ctx.flags & SCRIPT_VERIFY_SIGPUSHONLY) && !scriptSig.isPushOnly()) {
        error = ScriptError::SIG_PUSHONLY;
        return false;
    }

    std::vector<std::vector<uint8_t>> stack;

    // 1. Execute scriptSig
    if (!EvalScript(scriptSig, stack, ctx, error)) {
        return false;
    }

    std::vector<std::vector<uint8_t>> stackCopy = stack;

    // 2. Execute scriptPubKey
    if (!EvalScript(scriptPubKey, stack, ctx, error)) {
        return false;
    }

    // 3. Check final stack state
    if (stack.empty()) {
        error = ScriptError::EVAL_FALSE;
        return false;
    }

    if (!CastToBool(stack.back())) {
        error = ScriptError::EVAL_FALSE;
        return false;
    }

    // 4. P2SH validation (BIP 16)
    // Track if we have a P2SH-wrapped witness program
    bool hadP2SHWitness = false;
    int p2sh_witness_version = 0;
    std::vector<uint8_t> p2sh_witness_program;

    if ((ctx.flags & SCRIPT_VERIFY_P2SH) && scriptPubKey.isPayToScriptHash()) {
        // BIP 16: scriptSig must be push-only for P2SH
        if (!scriptSig.isPushOnly()) {
            error = ScriptError::SIG_PUSHONLY;
            return false;
        }

        if (stackCopy.empty()) {
            error = ScriptError::EVAL_FALSE;
            return false;
        }

        Script redeemScript(stackCopy.back());

        // Check if redeemScript is a witness program (P2SH-wrapped SegWit)
        if ((ctx.flags & SCRIPT_VERIFY_WITNESS) &&
            redeemScript.isWitnessProgram(p2sh_witness_version, p2sh_witness_program)) {
            // P2SH-wrapped SegWit: don't execute redeemScript, just verify witness
            hadP2SHWitness = true;

            // BIP 141: For P2SH-wrapped witness, scriptSig must be exactly the redeemScript push
            // stackCopy should have exactly one element (the redeemScript)
            if (stackCopy.size() != 1) {
                error = ScriptError::WITNESS_MALLEATED_P2SH;
                return false;
            }
        } else {
            // Regular P2SH: execute the redeemScript
            stack = stackCopy;
            stack.pop_back();

            if (!EvalScript(redeemScript, stack, ctx, error)) {
                return false;
            }

            if (stack.empty() || !CastToBool(stack.back())) {
                error = ScriptError::EVAL_FALSE;
                return false;
            }
        }
    }

    // 5. Witness validation (BIP 141, BIP 341)
    if (ctx.flags & SCRIPT_VERIFY_WITNESS) {
        int witness_version = 0;
        std::vector<uint8_t> witness_program;
        bool has_witness_program = false;

        // Check for native witness program or P2SH-wrapped witness
        bool is_native_witness = false;
        if (scriptPubKey.isWitnessProgram(witness_version, witness_program)) {
            has_witness_program = true;
            is_native_witness = true;

            // BIP 141: For native witness programs, scriptSig must be empty
            if (!scriptSig.empty()) {
                error = ScriptError::WITNESS_MALLEATED;
                return false;
            }
        } else if (hadP2SHWitness) {
            // Use the P2SH-wrapped witness program
            witness_version = p2sh_witness_version;
            witness_program = p2sh_witness_program;
            has_witness_program = true;
        }

        if (has_witness_program) {
            if (witness.empty()) {
                error = ScriptError::WITNESS_PROGRAM_WITNESS_EMPTY;
                return false;
            }

            // SegWit v0 (P2WPKH, P2WSH)
            if (witness_version == 0) {
                if (witness_program.size() == 20) {
                    // P2WPKH: OP_0 <20-byte-hash>
                    // BIP 141: Witness must be exactly [signature, pubkey]
                    if (witness.size() != 2) {
                        error = ScriptError::WITNESS_PROGRAM_MISMATCH;
                        return false;
                    }

                    const std::vector<uint8_t>& sig = witness[0];
                    const std::vector<uint8_t>& pubkey = witness[1];

                    // BIP 143: Pubkey must be compressed (33 bytes) when WITNESS_PUBKEYTYPE is set
                    if (ctx.flags & SCRIPT_VERIFY_WITNESS_PUBKEYTYPE) {
                        if (pubkey.size() != 33) {
                            error = ScriptError::WITNESS_PUBKEYTYPE;
                            return false;
                        }
                    }

                    // Verify HASH160(pubkey) == witness_program
                    std::vector<uint8_t> pubkey_hash = HASH160_Hash(pubkey);
                    if (pubkey_hash != witness_program) {
                        error = ScriptError::WITNESS_PROGRAM_MISMATCH;
                        return false;
                    }

                    // Construct script code for BIP 143 sighash:
                    // OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
                    Script script_code;
                    script_code << OP_DUP << OP_HASH160 << witness_program << OP_EQUALVERIFY << OP_CHECKSIG;

                    // Extract hash type from signature
                    if (sig.empty()) {
                        error = ScriptError::SIG_NULLFAIL;
                        return false;
                    }
                    uint8_t hash_type = sig.back();

                    // Compute BIP 143 sighash
                    std::vector<uint8_t> sighash = SignatureHashWitness(script_code, ctx, hash_type);

                    // Verify ECDSA signature
                    if (!CheckECDSASignature(sig, pubkey, sighash, ctx.flags)) {
                        error = ScriptError::CHECKSIGVERIFY;
                        return false;
                    }

                    // BIP141: For P2WPKH, set stack to single "true" for CLEANSTACK check
                    stack.clear();
                    stack.push_back({0x01});

                } else if (witness_program.size() == 32) {
                    // P2WSH: OP_0 <32-byte-hash>
                    // BIP 141: Last witness element is the witness script
                    if (witness.empty()) {
                        error = ScriptError::WITNESS_PROGRAM_WITNESS_EMPTY;
                        return false;
                    }

                    // Extract witness script (last element)
                    const std::vector<uint8_t>& witness_script_data = witness.back();
                    Script witness_script(witness_script_data);

                    // Verify SHA256(witness_script) == witness_program
                    std::vector<uint8_t> script_hash = SHA256_Hash(witness_script_data);
                    if (script_hash != witness_program) {
                        error = ScriptError::WITNESS_PROGRAM_MISMATCH;
                        return false;
                    }

                    // Build stack from witness elements (excluding witness script)
                    std::vector<std::vector<uint8_t>> witness_stack;
                    for (size_t i = 0; i < witness.size() - 1; i++) {
                        witness_stack.push_back(witness[i]);
                    }

                    // Create execution context for witness script
                    // Inherit flags from original context (don't force WITNESS_PUBKEYTYPE)
                    ScriptExecutionContext witness_ctx = ctx;
                    witness_ctx.is_witness_v0 = true;  // Mark as SegWit v0 for MINIMALIF enforcement

                    // Execute witness script
                    if (!EvalScript(witness_script, witness_stack, witness_ctx, error)) {
                        return false;
                    }

                    // Check final stack state
                    if (witness_stack.empty() || !CastToBool(witness_stack.back())) {
                        error = ScriptError::EVAL_FALSE;
                        return false;
                    }

                    // Clean stack check for witness
                    if (witness_stack.size() != 1) {
                        error = ScriptError::CLEANSTACK;
                        return false;
                    }

                } else {
                    error = ScriptError::WITNESS_PROGRAM_WRONG_LENGTH;
                    return false;
                }
            }
            // Taproot v1 (BIP 341)
            else if (witness_version == 1) {
                if (witness_program.size() != 32) {
                    error = ScriptError::WITNESS_PROGRAM_WRONG_LENGTH;
                    return false;
                }

                // The witness program is the x-only public key (32 bytes)
                const std::vector<uint8_t>& x_only_pubkey = witness_program;

                if (witness.empty()) {
                    error = ScriptError::WITNESS_PROGRAM_WITNESS_EMPTY;
                    return false;
                }

                // Determine spend type: key path or script path
                // Key path: witness is [signature] or [signature] where signature may have sighash byte
                // Script path: witness ends with [..., script, control_block]

                const std::vector<uint8_t>& last_elem = witness.back();

                // Check if this is a script path spend (control block starts with leaf version)
                bool is_script_path = false;
                if (witness.size() >= 2 && !last_elem.empty()) {
                    // Control block: first byte is (leaf_version & 0xfe) | parity
                    // Minimum control block size is 33 bytes (1 + 32 for internal key)
                    if (last_elem.size() >= 33 && (last_elem.size() - 33) % 32 == 0) {
                        is_script_path = true;
                    }
                }

                if (!is_script_path) {
                    // Key path spend: verify Schnorr signature against output key
                    if (witness.size() != 1) {
                        error = ScriptError::WITNESS_PROGRAM_MISMATCH;
                        return false;
                    }

                    const std::vector<uint8_t>& signature = witness[0];

                    // Signature must be 64 or 65 bytes
                    if (signature.size() != 64 && signature.size() != 65) {
                        error = ScriptError::SIG_DER;
                        return false;
                    }

                    // Extract hash type (default 0x00 for Taproot = SIGHASH_ALL)
                    uint8_t hash_type = 0x00;
                    if (signature.size() == 65) {
                        hash_type = signature.back();
                        if (hash_type == 0x00) {
                            // Explicit 0x00 is invalid in BIP 341
                            error = ScriptError::SIG_HASHTYPE;
                            return false;
                        }
                    }

                    // Compute BIP 341 sighash (key path = no leaf hash)
                    std::vector<uint8_t> sighash = SignatureHashTaproot(ctx, hash_type, {});

                    // Verify Schnorr signature
                    if (!CheckSchnorrSignature(signature, x_only_pubkey, sighash, ctx.flags)) {
                        error = ScriptError::CHECKSIGVERIFY;
                        return false;
                    }

                    // BIP341: For Taproot key-path, set stack to single "true" for CLEANSTACK check
                    // The scriptPubKey evaluation leaves junk on the stack (OP_1 + pubkey),
                    // but for native witness programs, only the witness verification matters.
                    stack.clear();
                    stack.push_back({0x01});  // Push "true" for CLEANSTACK
                } else {
                    // Script path spend
                    // Last element is control block, second-to-last is the script
                    if (witness.size() < 2) {
                        error = ScriptError::WITNESS_PROGRAM_MISMATCH;
                        return false;
                    }

                    const std::vector<uint8_t>& control_block = witness.back();
                    const std::vector<uint8_t>& tap_script = witness[witness.size() - 2];

                    // Validate control block size
                    if (control_block.size() < 33 || control_block.size() > 33 + 128 * 32) {
                        error = ScriptError::TAPROOT_WRONG_CONTROL_SIZE;
                        return false;
                    }

                    // Extract leaf version and parity from control block
                    uint8_t leaf_version = control_block[0] & 0xfe;

                    // For now, only support leaf version 0xc0 (tapscript)
                    if (leaf_version != 0xc0) {
                        if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION) {
                            error = ScriptError::DISCOURAGE_UPGRADABLE_TAPROOT_VERSION;
                            return false;
                        }
                        // Unknown leaf version - pass (future soft fork)
                    } else {
                        // Tapscript execution
                        // Build stack from witness elements (excluding script and control block)
                        std::vector<std::vector<uint8_t>> tap_stack;
                        for (size_t i = 0; i < witness.size() - 2; i++) {
                            tap_stack.push_back(witness[i]);
                        }

                        // Execute tapscript
                        Script tapscript(tap_script);
                        ScriptExecutionContext tap_ctx = ctx;
                        tap_ctx.flags |= SCRIPT_VERIFY_WITNESS_PUBKEYTYPE;

                        if (!EvalScript(tapscript, tap_stack, tap_ctx, error)) {
                            return false;
                        }

                        // Check final stack state
                        if (tap_stack.empty() || !CastToBool(tap_stack.back())) {
                            error = ScriptError::EVAL_FALSE;
                            return false;
                        }
                    }
                }
            }
            // Unknown witness version
            else {
                if (ctx.flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM) {
                    error = ScriptError::DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM;
                    return false;
                }
            }
        } else {
            // No witness program (neither native nor P2SH-wrapped)
            // If witness data is provided, it's unexpected
            if (!witness.empty()) {
                error = ScriptError::WITNESS_UNEXPECTED;
                return false;
            }
        }
    }

    // 6. Clean stack check
    if ((ctx.flags & SCRIPT_VERIFY_CLEANSTACK) && stack.size() != 1) {
        error = ScriptError::CLEANSTACK;
        return false;
    }

    error = ScriptError::OK;
    return true;
}

// ============================================================================
// Signature Verification (Implemented in script_verify_sig.cpp)
// ============================================================================

// NOTE: CheckECDSASignature() and CheckSchnorrSignature() are fully
// implemented in src/consensus/script_verify_sig.cpp with real secp256k1
// verification. This file only contains the sighash computation functions.

// ============================================================================
// Signature Hash Computation (Implemented in script_sighash.cpp)
// ============================================================================

// NOTE: SignatureHashLegacy(), SignatureHashWitness(), and SignatureHashTaproot()
// are implemented in src/consensus/script_sighash.cpp. Currently they use simplified
// stubs that need to be upgraded to full Bitcoin-compatible implementations.

// ============================================================================
// Standard Script Creation
// ============================================================================

Script createP2PKHScript(const std::vector<uint8_t>& pubkey_hash) {
    Script script;
    script << OP_DUP << OP_HASH160 << pubkey_hash << OP_EQUALVERIFY << OP_CHECKSIG;
    return script;
}

Script createP2SHScript(const std::vector<uint8_t>& script_hash) {
    Script script;
    script << OP_HASH160 << script_hash << OP_EQUAL;
    return script;
}

Script createP2WPKHScript(const std::vector<uint8_t>& pubkey_hash) {
    Script script;
    script << OP_0 << pubkey_hash;
    return script;
}

Script createP2WSHScript(const std::vector<uint8_t>& script_hash) {
    Script script;
    script << OP_0 << script_hash;
    return script;
}

Script createP2TRScript(const std::vector<uint8_t>& x_only_pubkey) {
    Script script;
    script << OP_1 << x_only_pubkey;
    return script;
}

} // namespace consensus
} // namespace dinero
