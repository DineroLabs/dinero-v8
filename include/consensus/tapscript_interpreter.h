#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include "primitives/transaction.h"
#include "consensus/utxo_entry.h"

namespace dinero {
namespace consensus {

/**
 * Tapscript Interpreter (BIP342)
 * Executes Taproot script path spending scripts
 *
 * Key differences from legacy Bitcoin Script:
 * - Schnorr signatures (BIP340) instead of ECDSA
 * - OP_CHECKSIGADD for batch verification
 * - OP_SUCCESS opcodes for soft fork upgrades
 * - Signature is always 64 bytes (no hash type appended)
 * - All signature operations use signature hash computed from transaction
 */
class TapscriptInterpreter {
public:
    /**
     * Execute a Tapscript and verify it evaluates to true
     *
     * @param script The Tapscript to execute (from witness)
     * @param witness_stack The witness stack (excluding script and control block)
     * @param tx The transaction being validated
     * @param input_index The input index being validated
     * @param input_utxos All input UTXOs (for BIP341 sighash)
     * @param tapleaf_hash The tapleaf hash (for script path sighash)
     * @param flags Script verification flags (for covenant enforcement)
     * @param error Output parameter for error message
     * @param annex BIP341 annex data (empty if no annex present)
     * @return true if script executes successfully and leaves true on stack
     */
    static bool ExecuteTapscript(
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
        const std::vector<uint8_t>& annex = {}
    );

private:
    // Execution context (stack machine)
    // Phase L0.3: Added flags for covenant enforcement
    struct ExecutionContext {
        std::vector<std::vector<uint8_t>> stack;
        const Transaction* tx;
        size_t input_index;
        const std::vector<UTXOEntry>* input_utxos;
        const std::vector<uint8_t>* tapscript;
        const std::vector<uint8_t>* tapleaf_hash;
        const std::array<uint8_t, 32>* internal_key;
        const std::array<uint8_t, 32>* merkle_root;
        uint8_t output_key_parity;
        const std::vector<uint8_t>* annex;  // BIP341 annex (for sighash computation)
        uint32_t flags;  // Script verification flags (for covenant enforcement)
        bool op_success{false};  // BIP342 immediate-success short circuit
        int64_t validation_weight_left{0};  // BIP342 witness budget
        std::string error;
    };

    // Core execution loop
    static bool Execute(const std::vector<uint8_t>& script, ExecutionContext& ctx);

    // Opcode handlers
    static bool OpCheckSig(ExecutionContext& ctx);
    static bool OpCheckSigVerify(ExecutionContext& ctx);
    static bool OpCheckSigAdd(ExecutionContext& ctx);
    static bool OpDup(ExecutionContext& ctx);
    static bool OpDrop(ExecutionContext& ctx);
    static bool OpEqual(ExecutionContext& ctx);
    static bool OpEqualVerify(ExecutionContext& ctx);
    static bool OpVerify(ExecutionContext& ctx);
    static bool OpReturn(ExecutionContext& ctx);

    // Phase L0.3: Covenant opcode handlers
    static bool OpCheckTemplateVerify(ExecutionContext& ctx);
    static bool OpCheckSigFromStack(ExecutionContext& ctx);
    static bool OpTxHash(ExecutionContext& ctx);
    static bool OpCheckContractVerify(ExecutionContext& ctx);

    // Stack helpers
    static bool PopStack(ExecutionContext& ctx, std::vector<uint8_t>& out);
    static bool PushStack(ExecutionContext& ctx, const std::vector<uint8_t>& data);  // BIP342: Returns false if limits exceeded
    static bool CastToBool(const std::vector<uint8_t>& data);

    // Signature verification
    static bool VerifySchnorrSignature(
        const std::vector<uint8_t>& signature,
        const std::vector<uint8_t>& pubkey,
        const std::vector<uint8_t>& sighash
    );
};

// Tapscript opcodes (BIP342)
namespace TapscriptOpcodes {
    // Constants
    constexpr uint8_t OP_0 = 0x00;
    constexpr uint8_t OP_FALSE = 0x00;
    constexpr uint8_t OP_PUSHDATA1 = 0x4c;
    constexpr uint8_t OP_PUSHDATA2 = 0x4d;
    constexpr uint8_t OP_PUSHDATA4 = 0x4e;
    constexpr uint8_t OP_1NEGATE = 0x4f;
    constexpr uint8_t OP_TRUE = 0x51;
    constexpr uint8_t OP_1 = 0x51;

    // Flow control
    constexpr uint8_t OP_VERIFY = 0x69;
    constexpr uint8_t OP_RETURN = 0x6a;

    // Stack
    constexpr uint8_t OP_DUP = 0x76;
    constexpr uint8_t OP_DROP = 0x75;
    constexpr uint8_t OP_SWAP = 0x7c;

    // Equality
    constexpr uint8_t OP_EQUAL = 0x87;
    constexpr uint8_t OP_EQUALVERIFY = 0x88;

    // Crypto (BIP342 - Schnorr signatures)
    constexpr uint8_t OP_CHECKSIG = 0xac;
    constexpr uint8_t OP_CHECKSIGVERIFY = 0xad;
    constexpr uint8_t OP_CHECKSIGADD = 0xba; // BIP342 new opcode

    // Phase L0.3: Covenant opcodes defined in script.h
    // (not redefined here to avoid ambiguity)
    // OP_CHECKTEMPLATEVERIFY = 0xb3
    // OP_CHECKSIGFROMSTACK = 0xbb
    // OP_TXHASH = 0xbd
    // OP_CHECKCONTRACTVERIFY = 0xbe

    // OP_SUCCESS opcodes (BIP342 upgradeable soft fork)
    // These opcodes immediately succeed (for future soft forks)
    constexpr uint8_t OP_SUCCESS80 = 0x50;
    constexpr uint8_t OP_SUCCESS98 = 0x62;
    // ... (there are many more, see BIP342)

    /**
     * Check if an opcode is an OP_SUCCESS opcode (BIP342)
     * OP_SUCCESS opcodes always succeed, enabling soft fork upgrades
     */
    inline bool IsOpSuccess(uint8_t opcode) {
        // Exact BIP342 set. Opcodes 0xbb-0xfe are deliberately upgradeable;
        // Dinero's custom opcodes in that range remain OP_SUCCESS until their
        // individual consensus flags activate.
        return opcode == 0x50 || opcode == 0x62 ||
               (opcode >= 0x7e && opcode <= 0x81) ||
               (opcode >= 0x83 && opcode <= 0x86) ||
               (opcode >= 0x89 && opcode <= 0x8a) ||
               (opcode >= 0x8d && opcode <= 0x8e) ||
               (opcode >= 0x95 && opcode <= 0x99) ||
               (opcode >= 0xbb && opcode <= 0xfe);
    }
}

} // namespace consensus
} // namespace dinero
