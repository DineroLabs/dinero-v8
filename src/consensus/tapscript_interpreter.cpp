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

namespace dinero {
namespace consensus {

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
    uint32_t flags,
    std::string& error,
    const std::vector<uint8_t>& annex
) {
    // BIP342 Fix #3: Enforce maximum script size (10,000 bytes)
    if (script.size() > 10000) {
        error = "Script size limit exceeded (BIP342: 10,000 bytes max)";
        return false;
    }

    // Initialize execution context
    ExecutionContext ctx;
    ctx.stack = witness_stack; // Initial stack from witness
    ctx.tx = &tx;
    ctx.input_index = input_index;
    ctx.input_utxos = &input_utxos;
    ctx.tapleaf_hash = &tapleaf_hash;
    ctx.annex = &annex;  // BIP341 annex for sighash computation
    ctx.flags = flags;  // Phase L0.3: Pass flags for covenant enforcement

    // Execute the script
    if (!Execute(script, ctx)) {
        error = ctx.error.empty() ? "Script execution failed" : ctx.error;
        return false;
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
    size_t pc = 0; // Program counter

    while (pc < script.size()) {
        uint8_t opcode = script[pc++];

        // OP_SUCCESS opcodes (BIP342 soft fork mechanism)
        if (TapscriptOpcodes::IsOpSuccess(opcode)) {
            // OP_SUCCESS always succeeds (for future soft forks)
            return true;
        }

        // Push data opcodes (0x01-0x4b: direct push)
        if (opcode >= 0x01 && opcode <= 0x4b) {
            size_t push_len = opcode;
            if (pc + push_len > script.size()) {
                ctx.error = "Push operation exceeds script bounds";
                return false;
            }
            std::vector<uint8_t> data(script.begin() + pc, script.begin() + pc + push_len);
            if (!PushStack(ctx, data)) return false;  // BIP342: Check limits
            pc += push_len;
            continue;
        }

        // Handle opcodes
        switch (opcode) {
            // Constants
            case OP_0:  // OP_0 == OP_FALSE (0x00)
                if (!PushStack(ctx, {})) return false;  // BIP342: Check limits
                break;

            case OP_1:  // OP_1 == OP_TRUE (0x51)
                if (!PushStack(ctx, {0x01})) return false;  // BIP342: Check limits
                break;

            case OP_1NEGATE:
                if (!PushStack(ctx, {0x81})) return false;  // BIP342: Check limits
                break;

            // Stack operations
            case OP_DUP:
                if (!OpDup(ctx)) return false;
                break;

            case OP_DROP:
                if (!OpDrop(ctx)) return false;
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

            // Phase L0.3: Covenant opcodes (consensus-critical)
            case OP_CHECKTEMPLATEVERIFY:
                if (!OpCheckTemplateVerify(ctx)) return false;
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

            // Push data with length prefix
            case OP_PUSHDATA1: {
                if (pc >= script.size()) {
                    ctx.error = "OP_PUSHDATA1: missing length byte";
                    return false;
                }
                size_t len = script[pc++];
                if (pc + len > script.size()) {
                    ctx.error = "OP_PUSHDATA1: data exceeds script bounds";
                    return false;
                }
                std::vector<uint8_t> data(script.begin() + pc, script.begin() + pc + len);
                if (!PushStack(ctx, data)) return false;  // BIP342: Check limits
                pc += len;
                break;
            }

            default:
                ctx.error = "Unknown or unsupported opcode: 0x" +
                           std::to_string(static_cast<int>(opcode));
                return false;
        }
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

    // Schnorr signature must be exactly 64 bytes (BIP340)
    if (signature.size() != 64) {
        ctx.error = "OP_CHECKSIG: invalid signature size (expected 64 bytes for Schnorr)";
        return false;
    }

    // Public key must be 32 bytes (x-only, BIP340)
    if (pubkey.size() != 32) {
        ctx.error = "OP_CHECKSIG: invalid pubkey size (expected 32 bytes for x-only)";
        return false;
    }

    // Compute BIP341 sighash for script path spending
    std::vector<uint64_t> prevout_values;
    std::vector<std::vector<uint8_t>> prevout_scripts;

    for (const auto& utxo : *ctx.input_utxos) {
        // Phase M.6.2: Extract raw value for signature hashing
        prevout_values.push_back(utxo.value.GetUna());
        prevout_scripts.push_back(utxo.scriptPubKey);
    }

    // BIP342: hash_type is always 0x00 (SIGHASH_DEFAULT) for Tapscript
    // BIP341: Include annex in sighash computation if present
    std::vector<uint8_t> sighash = ScriptVerifier::ComputeTaprootSighash(
        *ctx.tx,
        ctx.input_index,
        prevout_values,
        prevout_scripts,
        0x00, // SIGHASH_DEFAULT
        *ctx.tapleaf_hash, // Script path spending
        ctx.annex ? *ctx.annex : std::vector<uint8_t>{} // BIP341 annex
    );

    // Verify Schnorr signature
    bool valid = VerifySchnorrSignature(signature, pubkey, sighash);
    return PushStack(ctx, valid ? std::vector<uint8_t>{0x01} : std::vector<uint8_t>{});  // BIP342: Check limits
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
    if (!PopStack(ctx, signature)) return false;
    if (!PopStack(ctx, n_bytes)) return false;

    // n must be a valid number (CScriptNum in Bitcoin Core)
    // For simplicity, we'll assume it's a single byte for now
    if (n_bytes.empty()) {
        ctx.error = "OP_CHECKSIGADD: n is empty";
        return false;
    }

    int64_t n = static_cast<int64_t>(n_bytes[0]);

    // Empty signature is always invalid (doesn't increment n)
    if (signature.empty()) {
        return PushStack(ctx, n_bytes);  // BIP342: Push n unchanged and check limits
    }

    // Validate signature format
    if (signature.size() != 64) {
        ctx.error = "OP_CHECKSIGADD: invalid signature size";
        return false;
    }

    if (pubkey.size() != 32) {
        ctx.error = "OP_CHECKSIGADD: invalid pubkey size";
        return false;
    }

    // Compute sighash
    std::vector<uint64_t> prevout_values;
    std::vector<std::vector<uint8_t>> prevout_scripts;

    for (const auto& utxo : *ctx.input_utxos) {
        // Phase M.6.2: Extract raw value for signature hashing
        prevout_values.push_back(utxo.value.GetUna());
        prevout_scripts.push_back(utxo.scriptPubKey);
    }

    // BIP341: Include annex in sighash computation if present
    std::vector<uint8_t> sighash = ScriptVerifier::ComputeTaprootSighash(
        *ctx.tx,
        ctx.input_index,
        prevout_values,
        prevout_scripts,
        0x00,
        *ctx.tapleaf_hash,
        ctx.annex ? *ctx.annex : std::vector<uint8_t>{} // BIP341 annex
    );

    // Verify signature
    bool valid = VerifySchnorrSignature(signature, pubkey, sighash);

    // Increment n if valid
    if (valid) {
        n++;
    }

    // Push updated n back to stack
    return PushStack(ctx, {static_cast<uint8_t>(n & 0xff)});  // BIP342: Check limits
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
    // BIP342 Fix #1: Enforce maximum stack size (1000 elements)
    if (ctx.stack.size() >= 1000) {
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

    // Stack: <32-byte template hash>
    if (ctx.stack.empty()) {
        ctx.error = "OP_CHECKTEMPLATEVERIFY: stack empty";
        return false;
    }

    const auto& expected_hash = ctx.stack.back();

    // Expected hash must be exactly 32 bytes
    if (expected_hash.size() != 32) {
        ctx.error = "OP_CHECKTEMPLATEVERIFY: hash must be 32 bytes, got " + std::to_string(expected_hash.size());
        return false;
    }

    // Need transaction context
    if (!ctx.tx) {
        ctx.error = "OP_CHECKTEMPLATEVERIFY: no transaction context";
        return false;
    }

    // Verify CTV template hash using covenant verification function
    if (!VerifyCTV(*ctx.tx, static_cast<uint32_t>(ctx.input_index), expected_hash)) {
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

    // Pop contract state data from stack
    const auto& new_state_bytes = ctx.stack.back();
    ctx.stack.pop_back();
    const auto& prev_state_bytes = ctx.stack.back();
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

    // Verify contract state transition using covenant verification function
    if (!VerifyContractTransition(*ctx.tx, static_cast<uint32_t>(ctx.input_index),
                                   prev_state, new_state)) {
        ctx.error = "OP_CHECKCONTRACTVERIFY: state transition verification failed";
        return false;
    }

    // Success: contract state transition is valid
    return true;
}

} // namespace consensus
} // namespace dinero
