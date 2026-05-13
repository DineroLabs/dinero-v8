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
#include <cstdint>
#include <array>

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
 * Compute the CTV template hash for a transaction.
 *
 * The template hash commits to:
 * - nVersion (4 bytes, little-endian)
 * - nLockTime (4 bytes, little-endian)
 * - scriptSigs hash (32 bytes) - hash of all scriptSigs
 * - number of inputs (4 bytes, little-endian)
 * - sequences hash (32 bytes) - hash of all input sequences
 * - number of outputs (4 bytes, little-endian)
 * - outputs hash (32 bytes) - hash of all outputs
 * - input index (4 bytes, little-endian)
 *
 * @param tx The spending transaction
 * @param inputIndex The index of the input being verified
 * @return 32-byte template hash
 */
std::array<uint8_t, 32> ComputeCTVHash(const Transaction& tx, uint32_t inputIndex);

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

/**
 * Verify a contract state transition.
 *
 * This opcode enables stateful contracts in Tapscript by verifying
 * that state transitions follow the contract's rules.
 *
 * @param tx The spending transaction
 * @param inputIndex Current input index
 * @param prevState Previous contract state
 * @param newState New contract state
 * @return true if state transition is valid
 */
bool VerifyContractTransition(const Transaction& tx,
                               uint32_t inputIndex,
                               const ContractState& prevState,
                               const ContractState& newState);

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
// These flags are now part of SCRIPT_VERIFY_STANDARD for consensus enforcement.

} // namespace consensus
} // namespace dinero
