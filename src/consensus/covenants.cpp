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
#include <stdexcept>

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

std::array<uint8_t, 32> TaggedHash(const char* tag,
                                   const std::vector<uint8_t>& data) {
    const auto tagHash = SHA256Hash(
        reinterpret_cast<const uint8_t*>(tag), std::strlen(tag));
    crypto::CSHA256 hasher;
    hasher.Write(tagHash.data(), tagHash.size());
    hasher.Write(tagHash.data(), tagHash.size());
    if (!data.empty()) {
        hasher.Write(data.data(), data.size());
    }
    std::array<uint8_t, 32> result{};
    hasher.Finalize(result.data());
    return result;
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

// Bitcoin CompactSize encoding. BIP119 commits to canonical transaction
// serialization, not fixed-width vector lengths.
void WriteCompactSize(std::vector<uint8_t>& buf, uint64_t value) {
    if (value < 0xfd) {
        buf.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        buf.push_back(0xfd);
        buf.push_back(static_cast<uint8_t>(value));
        buf.push_back(static_cast<uint8_t>(value >> 8));
    } else if (value <= 0xffffffffULL) {
        buf.push_back(0xfe);
        WriteLE32(buf, static_cast<uint32_t>(value));
    } else {
        buf.push_back(0xff);
        WriteLE64(buf, value);
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

PrecomputedTransactionData::PrecomputedTransactionData(
    const Transaction& tx)
    : transaction_(&tx) {
    InitializeCTV(tx);
}

PrecomputedTransactionData::PrecomputedTransactionData(
    const Transaction& tx,
    const std::vector<UTXOEntry>& inputUtxos)
    : transaction_(&tx) {
    InitializeCTV(tx);
    InitializeTaproot(tx, inputUtxos);
}

void PrecomputedTransactionData::InitializeCTV(
    const Transaction& tx) {
    version_ = tx.version;
    lockTime_ = tx.lockTime;

    // BIP119 specifies Bitcoin's transparent transaction serialization. These
    // Dinero extensions carry value or authorization data that its template
    // hash does not commit to. Reject them rather than defining an unaudited,
    // project-specific hash under the BIP119 name.
    ctvEligible_ =
        !Transaction::IsShieldedVersion(tx.version) &&
        !tx.has_explicit_fee &&
        !tx.HasConfidentialOutputs() &&
        tx.vin.size() <= UINT32_MAX &&
        tx.vout.size() <= UINT32_MAX;
    if (!ctvEligible_) {
        return;
    }
    inputCount_ = static_cast<uint32_t>(tx.vin.size());
    outputCount_ = static_cast<uint32_t>(tx.vout.size());

    for (const auto& vin : tx.vin) {
        if (!vin.scriptSig.empty()) {
            hasNonEmptyScriptSig_ = true;
            break;
        }
    }

    if (hasNonEmptyScriptSig_) {
        std::vector<uint8_t> scriptSigData;
        for (const auto& vin : tx.vin) {
            WriteCompactSize(scriptSigData, vin.scriptSig.size());
            scriptSigData.insert(scriptSigData.end(),
                                 vin.scriptSig.begin(), vin.scriptSig.end());
        }
        scriptSigHash_ = SHA256Hash(scriptSigData);
    }

    std::vector<uint8_t> sequenceData;
    sequenceData.reserve(tx.vin.size() * 4);
    for (const auto& vin : tx.vin) {
        WriteLE32(sequenceData, vin.sequence);
    }
    sequenceHash_ = SHA256Hash(sequenceData);

    std::vector<uint8_t> outputData;
    outputData.reserve(tx.vout.size() * 48);
    for (const auto& vout : tx.vout) {
        WriteLE64(outputData, vout.value.GetUna());
        WriteCompactSize(outputData, vout.scriptPubKey.size());
        outputData.insert(outputData.end(),
                          vout.scriptPubKey.begin(), vout.scriptPubKey.end());
    }
    outputHash_ = SHA256Hash(outputData);
}

void PrecomputedTransactionData::InitializeTaproot(
    const Transaction& tx,
    const std::vector<UTXOEntry>& inputUtxos) {
    if (inputUtxos.size() != tx.vin.size()) {
        return;
    }
    taprootInputUtxos_ = inputUtxos;

    std::vector<uint8_t> prevouts;
    std::vector<uint8_t> amounts;
    std::vector<uint8_t> scriptPubKeys;
    std::vector<uint8_t> sequences;
    std::vector<uint8_t> outputs;
    std::vector<uint8_t> confidentialPrevouts;
    prevouts.reserve(tx.vin.size() * 36);
    amounts.reserve(tx.vin.size() * 8);
    sequences.reserve(tx.vin.size() * 4);
    outputs.reserve(tx.vout.size() * 48);

    confidentialPrevouts.push_back(0x01);
    confidentialPrevouts.push_back(0x00);
    WriteCompactSize(confidentialPrevouts, tx.vin.size());

    for (size_t index = 0; index < tx.vin.size(); ++index) {
        const auto& input = tx.vin[index];
        const auto& spent = inputUtxos[index];
        const auto& txid = input.prevout.txid.AsUint256();
        prevouts.insert(prevouts.end(), txid.data, txid.data + 32);
        WriteLE32(prevouts, input.prevout.vout);
        WriteLE64(amounts, spent.is_confidential ? 0 : spent.value.GetUna());
        WriteCompactSize(scriptPubKeys, spent.scriptPubKey.size());
        scriptPubKeys.insert(
            scriptPubKeys.end(),
            spent.scriptPubKey.begin(), spent.scriptPubKey.end());
        WriteLE32(sequences, input.sequence);

        confidentialPrevouts.push_back(spent.is_confidential ? 1 : 0);
        if (spent.is_confidential) {
            hasConfidentialPrevouts_ = true;
            WriteCompactSize(confidentialPrevouts, spent.commitment.size());
            confidentialPrevouts.insert(
                confidentialPrevouts.end(),
                spent.commitment.begin(), spent.commitment.end());
        } else {
            WriteCompactSize(confidentialPrevouts, 0);
        }
    }
    for (const auto& output : tx.vout) {
        WriteLE64(outputs, output.value.GetUna());
        WriteCompactSize(outputs, output.scriptPubKey.size());
        outputs.insert(
            outputs.end(),
            output.scriptPubKey.begin(), output.scriptPubKey.end());
    }

    taprootPrevoutsHash_ = SHA256Hash(prevouts);
    taprootAmountsHash_ = SHA256Hash(amounts);
    taprootScriptPubKeysHash_ = SHA256Hash(scriptPubKeys);
    taprootSequencesHash_ = SHA256Hash(sequences);
    taprootOutputsHash_ = SHA256Hash(outputs);
    if (hasConfidentialPrevouts_) {
        taprootConfidentialPrevoutsHash_ =
            TaggedHash("dinero/ct-prevouts/v1", confidentialPrevouts);
    }
    taprootEligible_ = true;
}

bool PrecomputedTransactionData::HasTaprootDataFor(
    const Transaction& tx) const {
    return taprootEligible_ && transaction_ == &tx;
}

bool PrecomputedTransactionData::TryComputeCTVHash(
    uint32_t inputIndex,
    std::array<uint8_t, 32>& hashOut) const {
    if (!ctvEligible_ || inputIndex >= inputCount_) {
        return false;
    }

    std::vector<uint8_t> preimage;
    preimage.reserve(116);
    WriteLE32(preimage, static_cast<uint32_t>(version_));
    WriteLE32(preimage, lockTime_);
    if (hasNonEmptyScriptSig_) {
        preimage.insert(
            preimage.end(), scriptSigHash_.begin(), scriptSigHash_.end());
    }
    WriteLE32(preimage, inputCount_);
    preimage.insert(
        preimage.end(), sequenceHash_.begin(), sequenceHash_.end());
    WriteLE32(preimage, outputCount_);
    preimage.insert(
        preimage.end(), outputHash_.begin(), outputHash_.end());

    WriteLE32(preimage, inputIndex);

    hashOut = SHA256Hash(preimage);
    return true;
}

bool TryComputeCTVHash(
    const Transaction& tx,
    uint32_t inputIndex,
    std::array<uint8_t, 32>& hashOut,
    const PrecomputedTransactionData* precomputed) {
    if (precomputed != nullptr) {
        if (!precomputed->IsFor(tx)) {
            return false;
        }
        return precomputed->TryComputeCTVHash(inputIndex, hashOut);
    }
    const PrecomputedTransactionData local(tx);
    return local.TryComputeCTVHash(inputIndex, hashOut);
}

std::array<uint8_t, 32> ComputeCTVHash(const Transaction& tx, uint32_t inputIndex) {
    std::array<uint8_t, 32> hash{};
    if (!TryComputeCTVHash(tx, inputIndex, hash)) {
        throw std::invalid_argument(
            "BIP119 CTV hash requires a valid input and transparent transaction");
    }
    return hash;
}

bool VerifyCTV(
    const Transaction& tx,
    uint32_t inputIndex,
    const std::vector<uint8_t>& expectedHash,
    const PrecomputedTransactionData* precomputed) {
    // Expected hash must be exactly 32 bytes
    if (expectedHash.size() != 32) {
        return false;
    }

    std::array<uint8_t, 32> computedHash{};
    if (!TryComputeCTVHash(
            tx, inputIndex, computedHash, precomputed)) {
        return false;
    }

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

std::array<uint8_t, 32> ComputeContractCodeHash(
    const std::vector<uint8_t>& tapscript) {
    return SHA256Hash(tapscript);
}

std::array<uint8_t, 32> ComputeContractStateHash(
    const ContractState& state) {
    std::vector<uint8_t> preimage;
    preimage.reserve(36 + state.data.size());
    preimage.insert(preimage.end(), state.codeHash.begin(), state.codeHash.end());
    WriteLE32(preimage, state.counter);
    preimage.insert(preimage.end(), state.data.begin(), state.data.end());
    return SHA256Hash(preimage);
}

bool DeriveContractInternalKey(
    const ContractState& state,
    std::array<uint8_t, 32>& internalKey) {
    std::vector<uint8_t> preimage;
    preimage.reserve(36);
    preimage.insert(preimage.end(), state.stateHash.begin(), state.stateHash.end());

    secp256k1_context* ctx = GetSecp256k1Context();
    for (uint32_t retry = 0;; ++retry) {
        preimage.resize(32);
        WriteLE32(preimage, retry);
        const auto candidate =
            TaggedHash("Dinero/CCVInternalKey/v1", preimage);
        secp256k1_xonly_pubkey parsed;
        if (secp256k1_xonly_pubkey_parse(ctx, &parsed, candidate.data())) {
            internalKey = candidate;
            return true;
        }
        if (retry == UINT32_MAX) {
            return false;
        }
    }
}

bool ComputeContractOutputScript(
    const ContractState& state,
    const std::array<uint8_t, 32>& merkleRoot,
    std::vector<uint8_t>& scriptPubKey,
    uint8_t* outputKeyParity) {
    std::array<uint8_t, 32> internalKey{};
    if (!DeriveContractInternalKey(state, internalKey)) {
        return false;
    }

    secp256k1_context* ctx = GetSecp256k1Context();
    secp256k1_xonly_pubkey internalPubkey;
    if (!secp256k1_xonly_pubkey_parse(
            ctx, &internalPubkey, internalKey.data())) {
        return false;
    }

    std::vector<uint8_t> tweakPreimage;
    tweakPreimage.reserve(64);
    tweakPreimage.insert(
        tweakPreimage.end(), internalKey.begin(), internalKey.end());
    tweakPreimage.insert(
        tweakPreimage.end(), merkleRoot.begin(), merkleRoot.end());
    const auto tweak = TaggedHash("TapTweak", tweakPreimage);

    secp256k1_pubkey tweakedPubkey;
    if (!secp256k1_xonly_pubkey_tweak_add(
            ctx, &tweakedPubkey, &internalPubkey, tweak.data())) {
        return false;
    }

    secp256k1_xonly_pubkey outputKey;
    int parity = 0;
    if (!secp256k1_xonly_pubkey_from_pubkey(
            ctx, &outputKey, &parity, &tweakedPubkey)) {
        return false;
    }
    if (outputKeyParity != nullptr) {
        *outputKeyParity = static_cast<uint8_t>(parity);
    }

    std::array<uint8_t, 32> serialized{};
    if (!secp256k1_xonly_pubkey_serialize(
            ctx, serialized.data(), &outputKey)) {
        return false;
    }

    scriptPubKey = {0x51, 0x20};
    scriptPubKey.insert(
        scriptPubKey.end(), serialized.begin(), serialized.end());
    return true;
}

bool VerifyContractTransition(const Transaction& tx,
                              uint32_t inputIndex,
                              const ContractState& prevState,
                              const ContractState& newState,
                              const ContractSpendContext& spendContext) {
    if (inputIndex >= tx.vin.size() ||
        inputIndex >= tx.vout.size() ||
        spendContext.inputUtxos.size() != tx.vin.size() ||
        prevState.data.size() > MAX_CONTRACT_STATE_DATA_SIZE ||
        newState.data.size() > MAX_CONTRACT_STATE_DATA_SIZE) {
        return false;
    }

    if (prevState.counter == UINT32_MAX ||
        newState.counter != prevState.counter + 1 ||
        newState.codeHash != prevState.codeHash ||
        ComputeContractStateHash(prevState) != prevState.stateHash ||
        ComputeContractStateHash(newState) != newState.stateHash ||
        ComputeContractCodeHash(spendContext.tapscript) != prevState.codeHash) {
        return false;
    }

    std::array<uint8_t, 32> expectedInternalKey{};
    if (!DeriveContractInternalKey(prevState, expectedInternalKey) ||
        expectedInternalKey != spendContext.internalKey) {
        return false;
    }

    const UTXOEntry& spent = spendContext.inputUtxos[inputIndex];
    if (spent.is_confidential) {
        return false;
    }

    std::vector<uint8_t> expectedCurrentScript;
    uint8_t expectedParity = 0;
    if (!ComputeContractOutputScript(
            prevState, spendContext.merkleRoot, expectedCurrentScript,
            &expectedParity) ||
        expectedParity != spendContext.outputKeyParity ||
        spent.scriptPubKey != expectedCurrentScript) {
        return false;
    }

    std::vector<uint8_t> expectedSuccessorScript;
    if (!ComputeContractOutputScript(
            newState, spendContext.merkleRoot, expectedSuccessorScript)) {
        return false;
    }

    const auto& successor = tx.vout[inputIndex];
    if (successor.is_confidential ||
        successor.value != spent.value ||
        successor.scriptPubKey != expectedSuccessorScript) {
        return false;
    }

    // A unique successor avoids ambiguous state lineage and prevents multiple
    // CCV inputs from claiming an indistinguishable output.
    for (size_t index = 0; index < tx.vout.size(); ++index) {
        if (index != inputIndex &&
            tx.vout[index].scriptPubKey == expectedSuccessorScript) {
            return false;
        }
    }

    return true;
}

} // namespace consensus
} // namespace dinero
