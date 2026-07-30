#pragma once

/**
 * Phase 28: Covenant Framework
 *
 * This module implements covenant opcodes that enable advanced smart contracts
 * in Dinero. Covenants allow scripts to introspect and constrain how
 * outputs can be spent, enabling:
 *
 * - Vaults (time-locked cold storage with recovery)
 * - Payment pools (scaling via shared UTXOs)
 * - Congestion control (fee market improvements)
 * - Recurring payments (subscriptions)
 * - DeFi primitives (atomic swaps, AMMs, lending)
 *
 * Opcodes implemented:
 * - OP_CHECKTEMPLATEVERIFY (CTV) - BIP-119 style template verification
 * - OP_CHECKSIGFROMSTACK (CSFS) - Signature verification over arbitrary messages
 * - OP_TXHASH - Transaction introspection
 * - OP_CHECKCONTRACTVERIFY (CCV) - Advanced contract state verification
 */

#include <vector>
#include <cstddef>
#include <cstdint>
#include <array>
#include "consensus/utxo_entry.h"

namespace dinero {

// Forward declarations
struct Transaction;

namespace consensus {

// ============================================================================
// CTV (CheckTemplateVerify) - BIP-119
// ============================================================================

/**
 * CTV Hash Flags - components to include in the template hash
 */
enum class CTVHashFlags : uint8_t {
    VERSION     = 0x01,  // Include nVersion
    LOCKTIME    = 0x02,  // Include nLockTime
    SCRIPTSIGS  = 0x04,  // Include hash of all scriptSigs
    SEQUENCES   = 0x08,  // Include hash of all input sequences
    OUTPUTS     = 0x10,  // Include hash of all outputs
    INPUT_INDEX = 0x20,  // Include the current input index

    // Standard CTV hash includes all components
    ALL = VERSION | LOCKTIME | SCRIPTSIGS | SEQUENCES | OUTPUTS | INPUT_INDEX,
};

/**
 * Compute the BIP119 DefaultCheckTemplateVerifyHash for a transaction.
 *
 * The template hash commits to:
 * - nVersion (4 bytes, little-endian)
 * - nLockTime (4 bytes, little-endian)
 * - scriptSigs hash (32 bytes), present only when any scriptSig is non-empty
 * - number of inputs (4 bytes, little-endian)
 * - sequences hash (32 bytes) - hash of all input sequences
 * - number of outputs (4 bytes, little-endian)
 * - outputs hash (32 bytes) - hash of all outputs
 * - input index (4 bytes, little-endian)
 *
 * @param tx The spending transaction
 * @param inputIndex The index of the input being verified
 * This construction-only convenience overload throws std::invalid_argument
 * when the transaction uses a Dinero-only serialization extension that BIP119
 * does not commit to. Consensus validation must use TryComputeCTVHash().
 *
 * @return 32-byte BIP119 template hash
 */
std::array<uint8_t, 32> ComputeCTVHash(const Transaction& tx, uint32_t inputIndex);

/**
 * Consensus-safe BIP119 hash computation.
 *
 * Returns false for an out-of-range input or for transaction forms outside
 * BIP119's canonical transparent serialization (confidential outputs,
 * shielded versions, or explicit-fee encoding). No custom hash extension is
 * substituted for those forms.
 */
bool TryComputeCTVHash(const Transaction& tx,
                       uint32_t inputIndex,
                       std::array<uint8_t, 32>& hashOut);

/**
 * Verify CTV opcode.
 *
 * Pops a 32-byte hash from the stack and verifies it matches
 * the computed template hash of the spending transaction.
 *
 * @param tx The spending transaction
 * @param inputIndex The index of the input being verified
 * @param expectedHash The expected 32-byte template hash from the stack
 * @return true if template matches, false otherwise
 */
bool VerifyCTV(const Transaction& tx, uint32_t inputIndex,
               const std::vector<uint8_t>& expectedHash);

// ============================================================================
// CSFS (CheckSigFromStack) - Signature verification over arbitrary messages
// ============================================================================

/**
 * Verify a Schnorr signature over an arbitrary message.
 *
 * This enables delegation patterns where signatures can authorize
 * specific actions without requiring the full transaction sighash.
 *
 * @param signature 64-byte Schnorr signature
 * @param message Arbitrary message bytes
 * @param pubkey 32-byte x-only public key
 * @return true if signature is valid, false otherwise
 */
bool VerifySignatureFromStack(const std::vector<uint8_t>& signature,
                               const std::vector<uint8_t>& message,
                               const std::vector<uint8_t>& pubkey);

// ============================================================================
// TXHASH - Transaction introspection
// ============================================================================

/**
 * TxHash Flags - select which transaction components to hash
 */
enum class TxHashFlags : uint8_t {
    // Input-related
    INPUT_COUNT        = 0x01,
    INPUT_PREVOUT      = 0x02,  // Specific input's prevout
    INPUT_SEQUENCE     = 0x04,  // Specific input's sequence
    INPUT_SCRIPTSIG    = 0x08,  // Specific input's scriptSig

    // Output-related
    OUTPUT_COUNT       = 0x10,
    OUTPUT_VALUE       = 0x20,  // Specific output's value
    OUTPUT_SCRIPTPUBKEY = 0x40, // Specific output's scriptPubKey

    // Transaction metadata
    VERSION            = 0x80,
    LOCKTIME           = 0x81,

    // Aggregate hashes
    ALL_INPUTS_HASH    = 0x90,
    ALL_OUTPUTS_HASH   = 0x91,
    ALL_SEQUENCES_HASH = 0x92,
};

/**
 * Compute a hash of selected transaction components.
 *
 * @param tx The transaction to introspect
 * @param flags Which components to include
 * @param index Optional index for input/output-specific components
 * @return 32-byte hash of selected components
 */
std::array<uint8_t, 32> ComputeTxHash(const Transaction& tx,
                                       TxHashFlags flags,
                                       uint32_t index = 0);

// ============================================================================
// CCV (CheckContractVerify) - Advanced contract verification
// ============================================================================

/**
 * Contract state descriptor for CCV verification.
 */
struct ContractState {
    std::array<uint8_t, 32> stateHash;      // Current state commitment
    std::array<uint8_t, 32> codeHash;       // Contract code hash
    std::vector<uint8_t> data;              // Contract data
    uint32_t counter;                        // State counter/nonce
};

// Two hashes, a counter, and a length consume 72 bytes of Tapscript's
// 520-byte consensus stack-element limit.
inline constexpr size_t MAX_CONTRACT_STATE_DATA_SIZE = 520 - 72;

/**
 * Authenticated Taproot data required to bind a CCV transition to the spent
 * output and its successor. The script verifier supplies these values only
 * after validating the control block and output key.
 */
struct ContractSpendContext {
    const std::vector<UTXOEntry>& inputUtxos;
    const std::vector<uint8_t>& tapscript;
    const std::array<uint8_t, 32>& internalKey;
    const std::array<uint8_t, 32>& merkleRoot;
    uint8_t outputKeyParity;
};

std::array<uint8_t, 32> ComputeContractCodeHash(
    const std::vector<uint8_t>& tapscript);

std::array<uint8_t, 32> ComputeContractStateHash(
    const ContractState& state);

bool DeriveContractInternalKey(
    const ContractState& state,
    std::array<uint8_t, 32>& internalKey);

bool ComputeContractOutputScript(
    const ContractState& state,
    const std::array<uint8_t, 32>& merkleRoot,
    std::vector<uint8_t>& scriptPubKey,
    uint8_t* outputKeyParity = nullptr);

/**
 * Verify a complete transparent CCV state transition.
 *
 * The previous state must describe the authenticated spent P2TR output. The
 * matching transaction output must preserve its exact transparent value and
 * commit to the next state under the same immutable Taproot tree.
 *
 * @param tx The spending transaction
 * @param inputIndex Current input index
 * @param prevState Previous contract state
 * @param newState New contract state
 * @param spendContext Authenticated control-block and spent-output context
 * @return true only if state and successor binding are valid
 */
bool VerifyContractTransition(const Transaction& tx,
                               uint32_t inputIndex,
                               const ContractState& prevState,
                               const ContractState& newState,
                               const ContractSpendContext& spendContext);

// ============================================================================
// Verification Flags
// ============================================================================

// Phase L0.2: Covenant verification flags moved to script_interpreter.h
// to ensure single source of truth for all SCRIPT_VERIFY_* flags.
// See include/consensus/script_interpreter.h for:
//   - SCRIPT_VERIFY_CHECKTEMPLATEVERIFY
//   - SCRIPT_VERIFY_CHECKSIGFROMSTACK
//   - SCRIPT_VERIFY_TXHASH
//   - SCRIPT_VERIFY_CHECKCONTRACT
//   - SCRIPT_VERIFY_COVENANTS (combined flags)
//
// These flags are height-dependent and are not statically part of
// SCRIPT_VERIFY_STANDARD.

} // namespace consensus
} // namespace dinero
