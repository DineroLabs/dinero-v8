/**
 * Phase 28: Covenant Framework Implementation
 *
 * This file implements the covenant opcodes for advanced smart contracts.
 */

#include "consensus/covenants.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"  // Phase C.1: Use canonical primitive location
#include <cstring>
#include <algorithm>

// secp256k1 for Schnorr verification
extern "C" {
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
}

namespace dinero {
namespace consensus {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

secp256k1_context* GetSecp256k1Context() {
    return dinero::crypto::GetSecp256k1ContextSignVerify();
}

// Compute SHA256 hash
std::array<uint8_t, 32> SHA256Hash(const uint8_t* data, size_t len) {
    std::array<uint8_t, 32> hash{};
    crypto::CSHA256().Write(data, len).Finalize(hash.data());
    return hash;
}

// Compute SHA256 hash of a vector
std::array<uint8_t, 32> SHA256Hash(const std::vector<uint8_t>& data) {
    return SHA256Hash(data.data(), data.size());
}

// Write uint32 in little-endian
void WriteLE32(std::vector<uint8_t>& buf, uint32_t value) {
    buf.push_back(value & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
    buf.push_back((value >> 16) & 0xFF);
    buf.push_back((value >> 24) & 0xFF);
}

// Write uint64 in little-endian
void WriteLE64(std::vector<uint8_t>& buf, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        buf.push_back((value >> (i * 8)) & 0xFF);
    }
}

// Convert hex string to bytes (for txid)
std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2 && i + j < hex.size(); ++j) {
            char c = hex[i + j];
            byte <<= 4;
            if (c >= '0' && c <= '9') byte |= (c - '0');
            else if (c >= 'a' && c <= 'f') byte |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') byte |= (c - 'A' + 10);
        }
        bytes.push_back(byte);
    }
    return bytes;
}

} // anonymous namespace

// ============================================================================
// CTV (CheckTemplateVerify) Implementation - BIP-119
// ============================================================================

std::array<uint8_t, 32> ComputeCTVHash(const Transaction& tx, uint32_t inputIndex) {
    std::vector<uint8_t> preimage;
    preimage.reserve(256);

    // 1. nVersion (4 bytes, little-endian)
    WriteLE32(preimage, static_cast<uint32_t>(tx.version));

    // 2. nLockTime (4 bytes, little-endian)
    WriteLE32(preimage, tx.lockTime);

    // 3. Hash of all scriptSigs
    // If any scriptSig is non-empty, hash them all. Otherwise, use 32 zero bytes.
    bool hasNonEmptyScriptSig = false;
    for (const auto& vin : tx.vin) {
        if (!vin.scriptSig.empty()) {
            hasNonEmptyScriptSig = true;
            break;
        }
    }

    if (hasNonEmptyScriptSig) {
        std::vector<uint8_t> scriptSigData;
        for (const auto& vin : tx.vin) {
            // Length-prefixed scriptSig
            WriteLE32(scriptSigData, static_cast<uint32_t>(vin.scriptSig.size()));
            scriptSigData.insert(scriptSigData.end(),
                                 vin.scriptSig.begin(), vin.scriptSig.end());
        }
        auto scriptSigHash = SHA256Hash(scriptSigData);
        preimage.insert(preimage.end(), scriptSigHash.begin(), scriptSigHash.end());
    } else {
        // 32 zero bytes for empty scriptSigs
        preimage.insert(preimage.end(), 32, 0x00);
    }

    // 4. Number of inputs (4 bytes, little-endian)
    WriteLE32(preimage, static_cast<uint32_t>(tx.vin.size()));

    // 5. Hash of all input sequences
    std::vector<uint8_t> sequenceData;
    sequenceData.reserve(tx.vin.size() * 4);
    for (const auto& vin : tx.vin) {
        WriteLE32(sequenceData, vin.sequence);
    }
    auto sequenceHash = SHA256Hash(sequenceData);
    preimage.insert(preimage.end(), sequenceHash.begin(), sequenceHash.end());

    // 6. Number of outputs (4 bytes, little-endian)
    WriteLE32(preimage, static_cast<uint32_t>(tx.vout.size()));

    // 7. Hash of all outputs
    // Transparent outputs: value(8) + scriptPubKey
    // Confidential outputs: value(8, =0 marker) + scriptPubKey + commitment(33) + SHA256(range_proof)
    // This allows CTV templates to commit to confidential output structure
    // without embedding the full ~5KB range proof in the template hash.
    std::vector<uint8_t> outputData;
    outputData.reserve(tx.vout.size() * 80);  // Estimate
    for (const auto& vout : tx.vout) {
        // Value (8 bytes, little-endian) — 0 for confidential outputs
        WriteLE64(outputData, vout.value.GetUna());
        // Length-prefixed scriptPubKey
        WriteLE32(outputData, static_cast<uint32_t>(vout.scriptPubKey.size()));
        outputData.insert(outputData.end(),
                          vout.scriptPubKey.begin(), vout.scriptPubKey.end());

        if (vout.is_confidential) {
            // Commitment (33 bytes) — binds the hidden amount
            WriteLE32(outputData, static_cast<uint32_t>(vout.commitment.size()));
            outputData.insert(outputData.end(),
                              vout.commitment.begin(), vout.commitment.end());
            // Hash of range proof (32 bytes) — commits to proof without 5KB bloat
            if (!vout.range_proof.empty()) {
                auto proofHash = SHA256Hash(vout.range_proof);
                outputData.insert(outputData.end(), proofHash.begin(), proofHash.end());
            } else {
                outputData.insert(outputData.end(), 32, 0x00);
            }
        }
    }
    auto outputHash = SHA256Hash(outputData);
    preimage.insert(preimage.end(), outputHash.begin(), outputHash.end());

    // 8. Input index (4 bytes, little-endian)
    WriteLE32(preimage, inputIndex);

    // Final hash: SHA256(SHA256(preimage))
    auto firstHash = SHA256Hash(preimage);
    return SHA256Hash(firstHash.data(), firstHash.size());
}

bool VerifyCTV(const Transaction& tx, uint32_t inputIndex,
               const std::vector<uint8_t>& expectedHash) {
    // Expected hash must be exactly 32 bytes
    if (expectedHash.size() != 32) {
        return false;
    }

    // Input index must be valid
    if (inputIndex >= tx.vin.size()) {
        return false;
    }

    // Compute the template hash
    auto computedHash = ComputeCTVHash(tx, inputIndex);

    // Compare hashes
    return std::equal(computedHash.begin(), computedHash.end(), expectedHash.begin());
}

// ============================================================================
// CSFS (CheckSigFromStack) Implementation
// ============================================================================

bool VerifySignatureFromStack(const std::vector<uint8_t>& signature,
                               const std::vector<uint8_t>& message,
                               const std::vector<uint8_t>& pubkey) {
    // Signature must be 64 bytes (Schnorr)
    if (signature.size() != 64) {
        return false;
    }

    // Public key must be 32 bytes (x-only)
    if (pubkey.size() != 32) {
        return false;
    }

    // Message can be any length - we hash it to 32 bytes
    std::array<uint8_t, 32> msgHash;
    if (message.size() == 32) {
        // Already 32 bytes, use directly
        std::copy(message.begin(), message.end(), msgHash.begin());
    } else {
        // Hash the message
        msgHash = SHA256Hash(message);
    }

    // Parse the x-only public key
    secp256k1_xonly_pubkey xonly_pubkey;
    if (!secp256k1_xonly_pubkey_parse(GetSecp256k1Context(),
                                       &xonly_pubkey, pubkey.data())) {
        return false;
    }

    // Verify the Schnorr signature
    return secp256k1_schnorrsig_verify(GetSecp256k1Context(),
                                        signature.data(),
                                        msgHash.data(), 32,
                                        &xonly_pubkey) == 1;
}

// ============================================================================
// TXHASH Implementation
// ============================================================================

std::array<uint8_t, 32> ComputeTxHash(const Transaction& tx,
                                       TxHashFlags flags,
                                       uint32_t index) {
    std::vector<uint8_t> data;
    data.reserve(128);

    uint8_t flagByte = static_cast<uint8_t>(flags);

    switch (flags) {
        case TxHashFlags::VERSION:
            WriteLE32(data, static_cast<uint32_t>(tx.version));
            break;

        case TxHashFlags::LOCKTIME:
            WriteLE32(data, tx.lockTime);
            break;

        case TxHashFlags::INPUT_COUNT:
            WriteLE32(data, static_cast<uint32_t>(tx.vin.size()));
            break;

        case TxHashFlags::OUTPUT_COUNT:
            WriteLE32(data, static_cast<uint32_t>(tx.vout.size()));
            break;

        case TxHashFlags::INPUT_PREVOUT:
            if (index < tx.vin.size()) {
                // Prevout hash (32 bytes) + prevout index (4 bytes) - Phase M.4.3-B: Unwrap TxId
                const auto& txid_u256 = tx.vin[index].prevout.txid.AsUint256();
                auto prevoutHash = std::vector<uint8_t>(txid_u256.data, txid_u256.data + 32);
                data.insert(data.end(), prevoutHash.begin(), prevoutHash.end());
                WriteLE32(data, tx.vin[index].prevout.vout);
            }
            break;

        case TxHashFlags::INPUT_SEQUENCE:
            if (index < tx.vin.size()) {
                WriteLE32(data, tx.vin[index].sequence);
            }
            break;

        case TxHashFlags::INPUT_SCRIPTSIG:
            if (index < tx.vin.size()) {
                data.insert(data.end(), tx.vin[index].scriptSig.begin(),
                            tx.vin[index].scriptSig.end());
            }
            break;

        case TxHashFlags::OUTPUT_VALUE:
            if (index < tx.vout.size()) {
                const auto& out = tx.vout[index];
                WriteLE64(data, out.value.GetUna());
                // For confidential outputs, also include the commitment
                if (out.is_confidential && !out.commitment.empty()) {
                    data.insert(data.end(), out.commitment.begin(), out.commitment.end());
                }
            }
            break;

        case TxHashFlags::OUTPUT_SCRIPTPUBKEY:
            if (index < tx.vout.size()) {
                data.insert(data.end(), tx.vout[index].scriptPubKey.begin(),
                            tx.vout[index].scriptPubKey.end());
            }
            break;

        case TxHashFlags::ALL_INPUTS_HASH: {
            std::vector<uint8_t> inputData;
            for (const auto& vin : tx.vin) {
                // Phase M.4.3-B: Unwrap TxId for serialization
                const auto& txid_u256 = vin.prevout.txid.AsUint256();
                auto prevoutHash = std::vector<uint8_t>(txid_u256.data, txid_u256.data + 32);
                inputData.insert(inputData.end(), prevoutHash.begin(), prevoutHash.end());
                WriteLE32(inputData, vin.prevout.vout);
            }
            return SHA256Hash(inputData);
        }

        case TxHashFlags::ALL_OUTPUTS_HASH: {
            std::vector<uint8_t> outputData;
            for (const auto& vout : tx.vout) {
                WriteLE64(outputData, vout.value.GetUna());
                WriteLE32(outputData, static_cast<uint32_t>(vout.scriptPubKey.size()));
                outputData.insert(outputData.end(), vout.scriptPubKey.begin(),
                                  vout.scriptPubKey.end());
                // Include commitment for confidential outputs (same as CTV hash)
                if (vout.is_confidential && !vout.commitment.empty()) {
                    WriteLE32(outputData, static_cast<uint32_t>(vout.commitment.size()));
                    outputData.insert(outputData.end(),
                                      vout.commitment.begin(), vout.commitment.end());
                    if (!vout.range_proof.empty()) {
                        auto proofHash = SHA256Hash(vout.range_proof);
                        outputData.insert(outputData.end(), proofHash.begin(), proofHash.end());
                    } else {
                        outputData.insert(outputData.end(), 32, 0x00);
                    }
                }
            }
            return SHA256Hash(outputData);
        }

        case TxHashFlags::ALL_SEQUENCES_HASH: {
            std::vector<uint8_t> seqData;
            for (const auto& vin : tx.vin) {
                WriteLE32(seqData, vin.sequence);
            }
            return SHA256Hash(seqData);
        }

        default:
            // Unknown flag, return empty hash
            break;
    }

    // Hash the collected data
    if (data.empty()) {
        std::array<uint8_t, 32> empty;
        empty.fill(0);
        return empty;
    }

    return SHA256Hash(data);
}

// ============================================================================
// CCV (CheckContractVerify) Implementation
// ============================================================================

bool VerifyContractTransition(const Transaction& tx,
                               uint32_t inputIndex,
                               const ContractState& prevState,
                               const ContractState& newState) {
    // Basic validation
    if (inputIndex >= tx.vin.size()) {
        return false;
    }

    // State counter must increment by exactly 1
    if (newState.counter != prevState.counter + 1) {
        return false;
    }

    // Code hash must remain the same (contract immutability)
    if (prevState.codeHash != newState.codeHash) {
        return false;
    }

    // Verify the new state hash is correctly computed
    std::vector<uint8_t> statePreimage;
    statePreimage.reserve(96 + newState.data.size());

    // codeHash (32 bytes)
    statePreimage.insert(statePreimage.end(),
                         newState.codeHash.begin(), newState.codeHash.end());

    // counter (4 bytes)
    WriteLE32(statePreimage, newState.counter);

    // data
    statePreimage.insert(statePreimage.end(),
                         newState.data.begin(), newState.data.end());

    auto computedStateHash = SHA256Hash(statePreimage);

    // Verify state hash matches
    if (computedStateHash != newState.stateHash) {
        return false;
    }

    // TODO(P2 — consensus-incomplete): This function does NOT verify:
    //   1. That any output commits to the new state (no output binding).
    //   2. That the output value is preserved minus fees (value conservation).
    //   3. Any contract-specific business rules.
    //
    // Without output binding, a transaction can satisfy CCV and spend the locked
    // coin without carrying the new state forward in any output — the contract
    // state machine "exits" without a successor. This violates the contract
    // invariant that motivates CCV's existence.
    //
    // Until this is implemented, CCV-based contracts are NOT safe for production
    // use. Treat them as experimental / proof-of-concept only. Do not activate
    // CCV-locked outputs on mainnet until this is resolved.

    return true;
}

} // namespace consensus
} // namespace dinero
