#pragma once

/**
 * Minimal Script Validation Engine
 *
 * Phase F.11: Explicit script validation for consensus
 *
 * This is NOT a script VM. This is explicit validation.
 * Each script type has its own validator - no shared state, no flags bleeding.
 *
 * Design Principle:
 * - P2PKH/P2WPKH: Extract sig+pubkey, verify ECDSA
 * - P2TR: Extract sig, verify Schnorr
 * - Unknown: Reject
 *
 * No opcodes. No VM. No Bitcoin Core parity chase.
 * Just: "Does this spend prove ownership?"
 */

#include "primitives/transaction.h"
#include "consensus/utxo_entry.h"
#include <vector>
#include <cstdint>

namespace dinero {
namespace consensus {

/**
 * Script validation result
 */
enum class ScriptValidationResult {
    OK,
    INVALID_SIGNATURE,       // Signature verification failed
    INVALID_SCRIPT,          // Malformed scriptSig or scriptPubKey
    UNSUPPORTED_SCRIPT,      // Unknown script type
    EXTRACT_FAILED           // Failed to extract sig/pubkey from scriptSig
};

/**
 * Supported script types
 */
enum class ScriptType {
    P2PKH,      // Pay-to-PubKey-Hash (legacy)
    P2WPKH,     // Pay-to-Witness-PubKey-Hash (SegWit v0)
    P2TR,       // Pay-to-Taproot (SegWit v1)
    P2MR,       // Pay-to-Merkle-Root (v7 PQ, witness v3: 0x53 0x20 <32 bytes>)
    UNKNOWN     // Unsupported or malformed
};

/**
 * Detect script type from scriptPubKey
 *
 * This is explicit pattern matching, not opcode execution.
 *
 * @param scriptPubKey The scriptPubKey to detect
 * @return ScriptType enum
 */
ScriptType DetectScriptType(const std::vector<uint8_t>& scriptPubKey);

/**
 * Validate a transaction input spend.
 *
 * This is the ONLY script validation entry point for consensus. Per
 * architectural rule: exactly one validation logic, used everywhere a
 * transaction's scripts must be checked against consensus.
 *
 * Flow:
 * 1. Detect script type from UTXO scriptPubKey
 * 2. Dispatch to appropriate validator (P2PKH, P2TR, P2WPKH, P2MR)
 * 3. Return result
 *
 * Called from:
 *   - Block validation (ConnectBlock / ValidateTransaction) — src/consensus/block_validation.cpp
 *   - Mempool admission (Mempool::validateTransaction)     — src/daemon/mempool.cpp
 *
 * Not called from Wallet or RPC: those are signers, not validators.
 *
 * @param tx Transaction being validated
 * @param input_index Index of input being validated
 * @param utxo UTXO being spent
 * @param block_height Height of the block the tx is being validated into
 *                     (mempool passes tip+1; used by height-gated rules
 *                      like PQSchemeRegistry activation)
 * @param all_utxos All input UTXOs (needed for BIP341 Taproot/P2MR sighash)
 * @return ScriptValidationResult
 */
ScriptValidationResult ValidateSpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height,
    const std::vector<UTXOEntry>& all_utxos = {}
);

/**
 * Validate legacy P2PKH spend
 *
 * P2PKH scriptPubKey: OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
 * P2PKH scriptSig: <sig> <pubkey>
 *
 * Validation:
 * 1. Extract sig and pubkey from scriptSig
 * 2. Verify pubkey hashes to scriptPubKey hash
 * 3. Compute legacy sighash
 * 4. Verify ECDSA signature
 *
 * @param tx Transaction
 * @param input_index Input index
 * @param utxo UTXO being spent
 * @param block_height Block height
 * @return ScriptValidationResult
 */
ScriptValidationResult ValidateLegacySpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height
);

/**
 * Validate Taproot spend (key-path only)
 *
 * P2TR scriptPubKey: OP_1 <32-byte-xonly-pubkey>
 * Witness: <64-byte-schnorr-sig> [<sighash_type>]
 *
 * Validation:
 * 1. Extract 64-byte Schnorr signature from witness[0]
 * 2. Extract optional sighash type (default: 0x00 = SIGHASH_DEFAULT)
 * 3. Extract x-only pubkey from scriptPubKey
 * 4. Compute BIP341 Taproot sighash (requires all input amounts/scriptPubKeys)
 * 5. Verify BIP340 Schnorr signature
 *
 * Note: Script-path spends (witness.size() > 1) not yet supported
 *
 * @param tx Transaction
 * @param input_index Input index
 * @param utxo UTXO being spent
 * @param block_height Block height
 * @param all_utxos All input UTXOs (required for BIP341 sighash)
 * @return ScriptValidationResult
 */
ScriptValidationResult ValidateTaprootSpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height,
    const std::vector<UTXOEntry>& all_utxos
);

} // namespace consensus
} // namespace dinero
