#include "consensus/crypto/sighash_bip143.h"
#include "primitives/transaction.h"
#include "crypto/sha256.h"
#include <algorithm>

namespace dinero {
namespace consensus {

// Double SHA256 hash (standard Bitcoin hash)
static std::vector<uint8_t> DoubleSHA256(const std::vector<uint8_t>& data) {
    uint8_t hash1[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash1);

    uint8_t hash2[32];
    crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    return std::vector<uint8_t>(hash2, hash2 + 32);
}

void SighashBIP143::WriteUint32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

void SighashBIP143::WriteUint64LE(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
    out.push_back((value >> 32) & 0xff);
    out.push_back((value >> 40) & 0xff);
    out.push_back((value >> 48) & 0xff);
    out.push_back((value >> 56) & 0xff);
}

// BIP143: Hash prevouts (all input txid+vout)
std::vector<uint8_t> SighashBIP143::GetPrevoutsHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& input : tx.vin) {
        // Txid (reversed for Bitcoin wire format)
        std::vector<uint8_t> txid_bytes(input.prevout.txid.v.data, input.prevout.txid.v.data + 32);
        std::reverse(txid_bytes.begin(), txid_bytes.end());
        data.insert(data.end(), txid_bytes.begin(), txid_bytes.end());

        // Vout
        WriteUint32LE(data, input.prevout.vout);
    }

    return DoubleSHA256(data);
}

// BIP143: Hash sequences (all input nSequence)
std::vector<uint8_t> SighashBIP143::GetSequenceHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& input : tx.vin) {
        WriteUint32LE(data, input.sequence);
    }

    return DoubleSHA256(data);
}

// BIP143: Hash outputs (all outputs)
std::vector<uint8_t> SighashBIP143::GetOutputsHash(const Transaction& tx) {
    std::vector<uint8_t> data;

    for (const auto& output : tx.vout) {
        // Value
        WriteUint64LE(data, output.value.GetUna());

        // ScriptPubKey with length
        TransactionSerializer::WriteBytes(data, output.scriptPubKey);
    }

    return DoubleSHA256(data);
}

// BIP143: Compute sighash for signing/verification
std::vector<uint8_t> SighashBIP143::ComputeSighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& scriptCode,
    uint64_t input_value,
    uint32_t sighash_type
) {
    if (input_index >= tx.vin.size()) {
        return {};  // Invalid input index
    }

    std::vector<uint8_t> preimage;

    // 1. nVersion (4 bytes)
    WriteUint32LE(preimage, static_cast<uint32_t>(tx.version));

    // 2. hashPrevouts (32 bytes)
    auto prevouts_hash = GetPrevoutsHash(tx);
    preimage.insert(preimage.end(), prevouts_hash.begin(), prevouts_hash.end());

    // 3. hashSequence (32 bytes)
    auto sequence_hash = GetSequenceHash(tx);
    preimage.insert(preimage.end(), sequence_hash.begin(), sequence_hash.end());

    // 4. outpoint (36 bytes: 32-byte txid + 4-byte vout)
    std::vector<uint8_t> txid_bytes(tx.vin[input_index].prevout.txid.v.data,
                                     tx.vin[input_index].prevout.txid.v.data + 32);
    std::reverse(txid_bytes.begin(), txid_bytes.end());
    preimage.insert(preimage.end(), txid_bytes.begin(), txid_bytes.end());
    WriteUint32LE(preimage, tx.vin[input_index].prevout.vout);

    // 5. scriptCode (with length)
    TransactionSerializer::WriteBytes(preimage, scriptCode);

    // 6. value (8 bytes)
    WriteUint64LE(preimage, input_value);

    // 7. nSequence (4 bytes)
    WriteUint32LE(preimage, tx.vin[input_index].sequence);

    // 8. hashOutputs (32 bytes)
    auto outputs_hash = GetOutputsHash(tx);
    preimage.insert(preimage.end(), outputs_hash.begin(), outputs_hash.end());

    // 9. nLocktime (4 bytes)
    WriteUint32LE(preimage, tx.lockTime);

    // 10. sighash type (4 bytes)
    WriteUint32LE(preimage, sighash_type);

    // Double SHA256 the preimage
    return DoubleSHA256(preimage);
}

} // namespace consensus
} // namespace dinero
