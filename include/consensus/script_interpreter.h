#pragma once

#include "consensus/script.h"
#include <vector>
#include <cstdint>
#include <string>

namespace dinero {

// Forward declarations
struct Transaction;
struct TxIn;
struct TxOut;

namespace consensus {

// Forward declaration for CPU budget monitoring
class CPUBudgetMonitor;

// ============================================================================
// Phase 24.1: Script Verification Flags
// ============================================================================

/**
 * Script verification flags
 *
 * These flags control which consensus rules are enforced during script
 * validation. They correspond to soft forks activated at different block
 * heights in Bitcoin's history.
 */
enum ScriptVerifyFlags : uint32_t {
    SCRIPT_VERIFY_NONE = 0,

    // BIP 16: Pay to Script Hash (April 2012)
    SCRIPT_VERIFY_P2SH = (1U << 0),

    // BIP 66: Strict DER signature encoding (July 2015)
    SCRIPT_VERIFY_DERSIG = (1U << 2),

    // BIP 62 rule 5: Low S value in signatures
    SCRIPT_VERIFY_LOW_S = (1U << 3),

    // BIP 65: OP_CHECKLOCKTIMEVERIFY (December 2015)
    SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY = (1U << 9),

    // BIP 112: OP_CHECKSEQUENCEVERIFY (July 2016)
    SCRIPT_VERIFY_CHECKSEQUENCEVERIFY = (1U << 10),

    // BIP 141: Segregated Witness (August 2017)
    SCRIPT_VERIFY_WITNESS = (1U << 11),

    // BIP 147: Dummy stack element malleability (SegWit)
    SCRIPT_VERIFY_NULLDUMMY = (1U << 4),

    // BIP 62 rule 2: ScriptSig must be push-only
    SCRIPT_VERIFY_SIGPUSHONLY = (1U << 5),

    // BIP 341/342: Taproot + Tapscript (November 2021)
    SCRIPT_VERIFY_TAPROOT = (1U << 17),

    // Phase L0.2: Covenant opcodes (consensus-critical)
    // BIP 119: OP_CHECKTEMPLATEVERIFY - Pre-signed transaction templates
    SCRIPT_VERIFY_CHECKTEMPLATEVERIFY = (1U << 20),
    // OP_CHECKSIGFROMSTACK - Signature verification over arbitrary messages
    SCRIPT_VERIFY_CHECKSIGFROMSTACK = (1U << 21),
    // OP_TXHASH - Transaction introspection (field selection)
    SCRIPT_VERIFY_TXHASH = (1U << 22),
    // OP_CHECKCONTRACTVERIFY - Advanced contract state verification
    SCRIPT_VERIFY_CHECKCONTRACT = (1U << 23),
    // Activated CCV successor-binding v1.
    SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING = (1U << 24),

    // Additional validation flags
    SCRIPT_VERIFY_STRICTENC = (1U << 1),       // Strict signature encoding
    SCRIPT_VERIFY_MINIMALDATA = (1U << 6),     // Require minimal push operations
    SCRIPT_VERIFY_NULLFAIL = (1U << 14),       // Failed signature must be empty
    SCRIPT_VERIFY_CLEANSTACK = (1U << 8),      // Require clean stack after execution
    SCRIPT_VERIFY_MINIMALIF = (1U << 13),      // OP_IF/NOTIF argument must be minimal
    SCRIPT_VERIFY_WITNESS_PUBKEYTYPE = (1U << 15),  // Witness pubkey must be compressed
    SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS = (1U << 7),  // Discourage NOPs
    SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM = (1U << 12),  // Unknown witness versions
    SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION = (1U << 18),  // Unknown Taproot leaf versions

    // Covenant verification flags combined (Phase L0.2)
    SCRIPT_VERIFY_COVENANTS = SCRIPT_VERIFY_CHECKTEMPLATEVERIFY |
                              SCRIPT_VERIFY_CHECKSIGFROMSTACK |
                              SCRIPT_VERIFY_TXHASH |
                              SCRIPT_VERIFY_CHECKCONTRACT,

    // Standard verification flags (post-Taproot + Covenants)
    // Phase L0.2: NOW INCLUDES COVENANT ENFORCEMENT
    SCRIPT_VERIFY_STANDARD = SCRIPT_VERIFY_P2SH |
                             SCRIPT_VERIFY_DERSIG |
                             SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
                             SCRIPT_VERIFY_CHECKSEQUENCEVERIFY |
                             SCRIPT_VERIFY_WITNESS |
                             SCRIPT_VERIFY_NULLDUMMY |
                             SCRIPT_VERIFY_TAPROOT |
                             SCRIPT_VERIFY_STRICTENC |
                             SCRIPT_VERIFY_MINIMALDATA |
                             SCRIPT_VERIFY_NULLFAIL |
                             SCRIPT_VERIFY_CLEANSTACK |
                             SCRIPT_VERIFY_MINIMALIF |
                             SCRIPT_VERIFY_WITNESS_PUBKEYTYPE |
                             SCRIPT_VERIFY_COVENANTS,  // ← CRITICAL: Enforces covenants in consensus
};

// ============================================================================
// Script Error Codes
// ============================================================================

/**
 * Script evaluation error codes
 *
 * These represent all possible failure modes during script execution.
 * They match Bitcoin Core's ScriptError enumeration.
 */
enum class ScriptError {
    OK = 0,

    // Evaluation errors
    UNKNOWN_ERROR,
    EVAL_FALSE,                  // Script evaluated to false
    OP_RETURN,                   // OP_RETURN encountered

    // Stack errors
    STACK_SIZE,                  // Stack size limit exceeded
    INVALID_STACK_OPERATION,     // Invalid stack operation (pop from empty)
    INVALID_ALTSTACK_OPERATION,  // Invalid altstack operation

    // Opcode errors
    DISABLED_OPCODE,             // Disabled opcode (OP_CAT, etc.)
    BAD_OPCODE,                  // Invalid opcode
    UNBALANCED_CONDITIONAL,      // Unbalanced IF/ENDIF

    // Push errors
    PUSH_SIZE,                   // Push value size limit exceeded
    OP_COUNT,                    // Operation count exceeded

    // Numeric errors
    INVALID_NUMBER_RANGE,        // Number out of valid range

    // Crypto errors
    SIG_DER,                     // Invalid DER signature encoding
    SIG_HASHTYPE,                // Invalid signature hash type
    SIG_NULLDUMMY,               // Non-null dummy argument (BIP 147)
    SIG_NULLFAIL,                // Failed signature must be empty (BIP 146)
    PUBKEYTYPE,                  // Invalid public key type
    WITNESS_PUBKEYTYPE,          // Witness pubkey must be compressed
    SIG_HIGH_S,                  // Signature S value is not low
    SIG_PUSHONLY,                // Script must be push-only

    // Signature verification
    CHECKSIGVERIFY,              // OP_CHECKSIGVERIFY failed
    CHECKMULTISIGVERIFY,         // OP_CHECKMULTISIGVERIFY failed

    // Locktime errors
    NEGATIVE_LOCKTIME,           // Negative locktime
    UNSATISFIED_LOCKTIME,        // Locktime requirement not satisfied

    // SegWit/Witness errors
    WITNESS_PROGRAM_WRONG_LENGTH,      // Invalid witness program length
    WITNESS_PROGRAM_WITNESS_EMPTY,     // Empty witness for witness program
    WITNESS_PROGRAM_MISMATCH,          // Witness program mismatch
    WITNESS_MALLEATED,                 // Malleated witness (P2SH witness)
    WITNESS_MALLEATED_P2SH,            // P2SH inside witness
    WITNESS_UNEXPECTED,                // Unexpected witness

    // Taproot/Tapscript errors
    TAPROOT_WRONG_CONTROL_SIZE,        // Invalid taproot control block size
    TAPSCRIPT_VALIDATION_WEIGHT,       // Tapscript validation weight exceeded
    TAPSCRIPT_CHECKMULTISIG,           // OP_CHECKMULTISIG in tapscript
    TAPSCRIPT_MINIMALIF,               // Non-minimal IF argument in tapscript
    TAPSCRIPT_EMPTY_PUBKEY,            // Empty public key in tapscript

    // Discouraged opcodes
    DISCOURAGE_UPGRADABLE_NOPS,        // Upgradable NOPs discouraged
    DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM,  // Unknown witness version
    DISCOURAGE_UPGRADABLE_TAPROOT_VERSION,  // Unknown taproot leaf version

    // Additional validation
    MINIMALDATA,                 // Non-minimal data push
    CLEANSTACK,                  // Stack not clean after execution
    MINIMALIF,                   // Non-minimal IF argument

    // Phase 28: Covenant errors
    CTV_WRONG_LENGTH,            // CTV hash wrong length (must be 32 bytes)
    CTV_VERIFY_FAILED,           // CTV template hash mismatch
    CSFS_WRONG_SIG_SIZE,         // CSFS signature wrong size (must be 64 bytes)
    CSFS_WRONG_PUBKEY_SIZE,      // CSFS pubkey wrong size (must be 32 bytes)
    CSFS_VERIFY_FAILED,          // CSFS signature verification failed
    TXHASH_INVALID_FLAGS,        // Invalid TXHASH flags
    CCV_INVALID_STATE,           // Invalid contract state
    CCV_VERIFY_FAILED,           // Contract verification failed
};

/**
 * Convert script error to human-readable string
 */
const char* ScriptErrorString(ScriptError error);

// ============================================================================
// Script Execution Context
// ============================================================================

// ============================================================================
// Phase 26: Sighash Preimage Cache (Optimization)
// ============================================================================

/**
 * Cache for BIP 143 sighash preimages
 *
 * These are computed once per transaction and reused for all signature
 * verifications. This provides significant speedup for multi-signature
 * scripts and P2WSH with multiple CHECKSIG operations.
 */
struct SighashCache {
    bool initialized = false;

    // BIP 143 cached preimages (witness sighash)
    std::vector<uint8_t> hash_prevouts;      // SHA256d of all input outpoints
    std::vector<uint8_t> hash_sequence;      // SHA256d of all input sequences
    std::vector<uint8_t> hash_outputs;       // SHA256d of all outputs

    // BIP 341 cached preimages (taproot sighash)
    std::vector<uint8_t> hash_amounts;       // SHA256 of all input amounts
    std::vector<uint8_t> hash_scriptpubkeys; // SHA256 of all input scriptPubKeys
    std::vector<uint8_t> hash_prevouts_single; // SHA256 (not double) for taproot
    std::vector<uint8_t> hash_sequences_single;
    std::vector<uint8_t> hash_outputs_single;

    void clear() {
        initialized = false;
        hash_prevouts.clear();
        hash_sequence.clear();
        hash_outputs.clear();
        hash_amounts.clear();
        hash_scriptpubkeys.clear();
        hash_prevouts_single.clear();
        hash_sequences_single.clear();
        hash_outputs_single.clear();
    }
};

/**
 * Context for script execution
 *
 * Provides transaction data needed for signature verification and timelock
 * validation. This is passed to the script interpreter during evaluation.
 */
struct ScriptExecutionContext {
    const Transaction* tx;           // Transaction being validated
    uint32_t input_index;            // Index of input being validated
    uint64_t amount;                 // Amount of output being spent (for SegWit sighash)
    uint32_t flags;                  // Verification flags
    bool is_witness_v0 = false;      // True when executing SegWit v0 witness script (for MINIMALIF)

    // Extended data for BIP 341 Taproot sighash (optional)
    // These are needed because Taproot commits to ALL input amounts and scriptPubKeys
    std::vector<uint64_t> all_amounts;           // Amounts for all inputs
    std::vector<std::vector<uint8_t>> all_scriptpubkeys;  // scriptPubKeys for all inputs
    std::vector<uint8_t> all_confidential_flags;          // 1 if the prevout is confidential
    std::vector<std::vector<uint8_t>> all_input_commitments; // Prevout CT commitments

    // Phase 26: Sighash preimage cache (mutable for caching in const methods)
    mutable SighashCache sighash_cache;

    ScriptExecutionContext(
        const Transaction* tx_,
        uint32_t input_index_,
        uint64_t amount_,
        uint32_t flags_
    )
        : tx(tx_)
        , input_index(input_index_)
        , amount(amount_)
        , flags(flags_)
        , is_witness_v0(false)
    {}

    // Extended constructor for Taproot
    ScriptExecutionContext(
        const Transaction* tx_,
        uint32_t input_index_,
        uint64_t amount_,
        uint32_t flags_,
        const std::vector<uint64_t>& amounts,
        const std::vector<std::vector<uint8_t>>& scriptpubkeys,
        const std::vector<uint8_t>& confidential_flags = {},
        const std::vector<std::vector<uint8_t>>& input_commitments = {}
    )
        : tx(tx_)
        , input_index(input_index_)
        , amount(amount_)
        , flags(flags_)
        , is_witness_v0(false)
        , all_amounts(amounts)
        , all_scriptpubkeys(scriptpubkeys)
        , all_confidential_flags(confidential_flags)
        , all_input_commitments(input_commitments)
    {}
};

// ============================================================================
// Script Evaluation (Stack-Based Virtual Machine)
// ============================================================================

/**
 * Evaluate a script on a stack
 *
 * This is the core script interpreter - a stack-based virtual machine that
 * executes Bitcoin Script opcodes. It processes each opcode sequentially,
 * manipulating the main stack and alt-stack according to the opcode semantics.
 *
 * @param script         Script to evaluate
 * @param stack          Main stack (input/output)
 * @param ctx            Execution context (transaction data)
 * @param error          Output: Error code if evaluation fails
 * @param cpu_monitor    Optional: CPU budget monitor for timeout enforcement (Phase E.3)
 * @return               True if script executed successfully, false otherwise
 */
bool EvalScript(
    const Script& script,
    std::vector<std::vector<uint8_t>>& stack,
    const ScriptExecutionContext& ctx,
    ScriptError& error,
    CPUBudgetMonitor* cpu_monitor = nullptr
);

/**
 * Verify script signature + pubkey combination
 *
 * This is the main entry point for script validation. It handles all script
 * types: P2PKH, P2SH, P2WPKH, P2WSH, P2TR (Taproot).
 *
 * Validation flow:
 * 1. Execute scriptSig (legacy inputs)
 * 2. Execute scriptPubKey (output script)
 * 3. If P2SH: execute redeemScript
 * 4. If witness: validate witness program (v0 SegWit or v1 Taproot)
 * 5. Check final stack state
 *
 * @param scriptSig      Signature script (from transaction input)
 * @param scriptPubKey   Public key script (from previous output)
 * @param witness        Witness stack (for SegWit)
 * @param ctx            Execution context
 * @param error          Output: Error code if verification fails
 * @return               True if script validates successfully
 */
bool VerifyScript(
    const Script& scriptSig,
    const Script& scriptPubKey,
    const std::vector<std::vector<uint8_t>>& witness,
    const ScriptExecutionContext& ctx,
    ScriptError& error
);

// ============================================================================
// Signature Verification
// ============================================================================

/**
 * Verify ECDSA signature (legacy P2PKH, P2SH)
 *
 * Validates secp256k1 ECDSA signature with DER encoding.
 *
 * @param signature      Signature (DER encoded + sighash byte)
 * @param pubkey         Public key (compressed or uncompressed)
 * @param sighash        Message hash (transaction sighash)
 * @param flags          Verification flags
 * @return               True if signature is valid
 */
bool CheckECDSASignature(
    const std::vector<uint8_t>& signature,
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& sighash,
    uint32_t flags
);

/**
 * Check if signature has valid DER encoding (BIP 66)
 *
 * This is a standalone DER validation check used before signature verification.
 * When DERSIG flag is set and this returns false, the script should fail with SIG_DER.
 *
 * @param sig            Signature bytes (DER + sighash byte)
 * @return               True if valid DER encoding, false otherwise
 */
bool IsValidSignatureEncoding(const std::vector<uint8_t>& sig);

/**
 * Verify Schnorr signature (Taproot BIP 340)
 *
 * Validates secp256k1 Schnorr signature (64 bytes, x-only pubkey).
 *
 * @param signature      Schnorr signature (64 bytes + optional sighash byte)
 * @param pubkey         X-only public key (32 bytes)
 * @param sighash        Message hash (taproot sighash)
 * @param flags          Verification flags
 * @return               True if signature is valid
 */
bool CheckSchnorrSignature(
    const std::vector<uint8_t>& signature,
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& sighash,
    uint32_t flags
);

// ============================================================================
// Transaction Signature Hashing
// ============================================================================

/**
 * Signature hash types (appended to signatures)
 */
enum SigHashType : uint8_t {
    SIGHASH_ALL = 1,
    SIGHASH_NONE = 2,
    SIGHASH_SINGLE = 3,
    SIGHASH_ANYONECANPAY = 0x80,
};

/**
 * Compute legacy signature hash (pre-SegWit)
 *
 * Computes the message hash for ECDSA signature verification in legacy
 * (non-SegWit) transactions.
 *
 * @param script_code    Script being executed (for signature checking)
 * @param ctx            Execution context
 * @param hash_type      Signature hash type
 * @return               32-byte signature hash
 */
std::vector<uint8_t> SignatureHashLegacy(
    const Script& script_code,
    const ScriptExecutionContext& ctx,
    uint8_t hash_type
);

/**
 * Compute SegWit v0 signature hash (BIP 143)
 *
 * Computes the message hash for ECDSA signature verification in SegWit v0
 * transactions (P2WPKH, P2WSH).
 *
 * @param script_code    Script being executed
 * @param ctx            Execution context
 * @param hash_type      Signature hash type
 * @return               32-byte signature hash
 */
std::vector<uint8_t> SignatureHashWitness(
    const Script& script_code,
    const ScriptExecutionContext& ctx,
    uint8_t hash_type
);

/**
 * Compute Taproot signature hash (BIP 341)
 *
 * Computes the message hash for Schnorr signature verification in Taproot
 * transactions (key-path and script-path spending).
 *
 * @param ctx            Execution context
 * @param hash_type      Signature hash type (default 0 = SIGHASH_DEFAULT)
 * @param leaf_hash      Tapleaf hash (for script-path spending, empty for key-path)
 * @param annex          BIP341 annex data (empty if no annex present)
 * @return               32-byte signature hash
 */
std::vector<uint8_t> SignatureHashTaproot(
    const ScriptExecutionContext& ctx,
    uint8_t hash_type,
    const std::vector<uint8_t>& leaf_hash,
    const std::vector<uint8_t>& annex = {}
);

// ============================================================================
// Standard Script Creation
// ============================================================================

/**
 * Create P2PKH script: OP_DUP OP_HASH160 <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG
 */
Script createP2PKHScript(const std::vector<uint8_t>& pubkey_hash);

/**
 * Create P2SH script: OP_HASH160 <script_hash> OP_EQUAL
 */
Script createP2SHScript(const std::vector<uint8_t>& script_hash);

/**
 * Create P2WPKH script: OP_0 <pubkey_hash>
 */
Script createP2WPKHScript(const std::vector<uint8_t>& pubkey_hash);

/**
 * Create P2WSH script: OP_0 <script_hash>
 */
Script createP2WSHScript(const std::vector<uint8_t>& script_hash);

/**
 * Create P2TR script: OP_1 <x_only_pubkey>
 */
Script createP2TRScript(const std::vector<uint8_t>& x_only_pubkey);

// ============================================================================
// Cryptographic Hash Functions (for script opcodes)
// ============================================================================

/**
 * SHA256 - Single SHA256 hash
 */
std::vector<uint8_t> SHA256_Hash(const std::vector<uint8_t>& data);

/**
 * HASH256 - Double SHA256 (SHA256(SHA256(x)))
 */
std::vector<uint8_t> HASH256_Hash(const std::vector<uint8_t>& data);

/**
 * RIPEMD160 - RIPEMD160 hash
 */
std::vector<uint8_t> RIPEMD160_Hash(const std::vector<uint8_t>& data);

/**
 * HASH160 - RIPEMD160(SHA256(x)) - Standard Bitcoin address hash
 */
std::vector<uint8_t> HASH160_Hash(const std::vector<uint8_t>& data);

/**
 * SHA1 - SHA1 hash (legacy)
 */
std::vector<uint8_t> SHA1_Hash(const std::vector<uint8_t>& data);

// ============================================================================
// BIP 340/341 Tagged Hashing
// ============================================================================

/**
 * Tagged Hash (BIP 340)
 *
 * TaggedHash(tag, msg) = SHA256(SHA256(tag) || SHA256(tag) || msg)
 *
 * This provides domain separation to prevent hash collisions across
 * different contexts (e.g., TapSighash, TapLeaf, TapBranch, TapTweak).
 */
std::vector<uint8_t> TaggedHash(const std::string& tag, const std::vector<uint8_t>& data);

/**
 * TapLeaf Hash (BIP 341)
 *
 * Computes the tapleaf hash for a script at a given leaf version.
 * TapLeaf(leaf_version, script) = TaggedHash("TapLeaf", leaf_version || compact_size(script) || script)
 */
std::vector<uint8_t> TapLeafHash(uint8_t leaf_version, const std::vector<uint8_t>& script);

/**
 * TapBranch Hash (BIP 341)
 *
 * Computes the hash of an internal taproot tree node.
 * TapBranch(left, right) = TaggedHash("TapBranch", sorted(left, right))
 */
std::vector<uint8_t> TapBranchHash(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right);

/**
 * TapTweak Hash (BIP 341)
 *
 * Computes the tweak for the internal key.
 * TapTweak(pubkey, merkle_root) = TaggedHash("TapTweak", pubkey || merkle_root)
 */
std::vector<uint8_t> TapTweakHash(const std::vector<uint8_t>& pubkey, const std::vector<uint8_t>& merkle_root);

} // namespace consensus
} // namespace dinero
