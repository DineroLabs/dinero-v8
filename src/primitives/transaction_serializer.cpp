#include "primitives/transaction.h"
#include "crypto/sha256.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cstring>

namespace dinero {

// ============================================================================
// TransactionSerializer - Canonical Implementation
// ============================================================================
//
// These are thin serialization helpers needed by both consensus and wallet.
// Placed in primitives layer so dinero_consensus can access them
// (dinero_consensus does NOT link against dinero_wallet).
//
// Pattern from user: "Wrap CDataStream, use canonical serialization flags,
// return bytes / hex — not identity"
//

// ===== Serialization Helpers =====

void TransactionSerializer::WriteUint32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

void TransactionSerializer::WriteUint64(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
    out.push_back((value >> 32) & 0xff);
    out.push_back((value >> 40) & 0xff);
    out.push_back((value >> 48) & 0xff);
    out.push_back((value >> 56) & 0xff);
}

void TransactionSerializer::WriteVarint(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0xfd) {
        out.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(value & 0xff);
        out.push_back((value >> 8) & 0xff);
    } else if (value <= 0xffffffff) {
        out.push_back(0xfe);
        WriteUint32(out, static_cast<uint32_t>(value));
    } else {
        out.push_back(0xff);
        WriteUint64(out, value);
    }
}

void TransactionSerializer::WriteBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    WriteVarint(out, data.size());
    out.insert(out.end(), data.begin(), data.end());
}

void TransactionSerializer::WriteString(std::vector<uint8_t>& out, const std::string& str) {
    std::vector<uint8_t> bytes(str.begin(), str.end());
    WriteBytes(out, bytes);
}

// ===== Hash Functions =====

std::string TransactionSerializer::DoubleSHA256(const std::vector<uint8_t>& data) {
    auto hash = DoubleSHA256Bytes(data);
    // For display: reverse bytes (Bitcoin convention shows hashes in big-endian)
    std::reverse(hash.begin(), hash.end());
    return ToHex(hash);
}

std::vector<uint8_t> TransactionSerializer::DoubleSHA256Bytes(const std::vector<uint8_t>& data) {
    // First SHA256
    uint8_t hash1[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash1);

    // Second SHA256
    uint8_t hash2[32];
    crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    // Return raw SHA256 output - NO reversal
    // Consensus rule: store hash bytes as-is (little-endian uint256 identity)
    // Display reversal happens in uint256::ToString() (Bitcoin convention)
    return std::vector<uint8_t>(hash2, hash2 + 32);
}

// ===== Hex Conversion =====

std::string TransactionSerializer::ToHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : data) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> TransactionSerializer::FromHex(const std::string& hex) {
    std::vector<uint8_t> result;
    if (hex.size() % 2 != 0) return result;

    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        result.push_back(byte);
    }
    return result;
}

// ===== Deserialization Helpers =====

// Helper class for reading from byte buffer
class ByteReader {
public:
    ByteReader(const std::vector<uint8_t>& data) : data_(data), pos_(0) {}

    bool eof() const { return pos_ >= data_.size(); }
    size_t remaining() const { return data_.size() - pos_; }
    size_t position() const { return pos_; }
    void setPosition(size_t pos) {
        if (pos > data_.size()) throw std::runtime_error("Invalid reader position");
        pos_ = pos;
    }

    uint8_t readUint8() {
        if (pos_ >= data_.size()) throw std::runtime_error("Buffer underflow");
        return data_[pos_++];
    }

    uint32_t readUint32() {
        if (pos_ + 4 > data_.size()) throw std::runtime_error("Buffer underflow");
        uint32_t val = 0;
        val |= static_cast<uint32_t>(data_[pos_++]);
        val |= static_cast<uint32_t>(data_[pos_++]) << 8;
        val |= static_cast<uint32_t>(data_[pos_++]) << 16;
        val |= static_cast<uint32_t>(data_[pos_++]) << 24;
        return val;
    }

    uint64_t readUint64() {
        if (pos_ + 8 > data_.size()) throw std::runtime_error("Buffer underflow");
        uint64_t val = 0;
        for (int i = 0; i < 8; i++) {
            val |= static_cast<uint64_t>(data_[pos_++]) << (i * 8);
        }
        return val;
    }

    uint64_t readVarInt() {
        uint8_t first = readUint8();
        if (first < 0xfd) return first;
        if (first == 0xfd) {
            uint16_t lo = static_cast<uint16_t>(readUint8());
            uint16_t hi = static_cast<uint16_t>(readUint8());
            return static_cast<uint16_t>(lo | (hi << 8));
        }
        if (first == 0xfe) return readUint32();
        return readUint64();
    }

    std::vector<uint8_t> readBytes(size_t count) {
        if (pos_ + count > data_.size()) throw std::runtime_error("Buffer underflow");
        std::vector<uint8_t> result(data_.begin() + pos_, data_.begin() + pos_ + count);
        pos_ += count;
        return result;
    }

    std::vector<uint8_t> readVarBytes() {
        uint64_t len = readVarInt();
        return readBytes(len);
    }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_;
};

// NOTE: Most Deserialize methods are implemented in src/wallet/transaction_deserializer.cpp
// This variant with consumed_out is needed by consensus code and doesn't exist in wallet

bool TransactionSerializer::Deserialize(Transaction& tx, const std::vector<uint8_t>& data, size_t& consumed_out) {
    try {
        ByteReader reader(data);

        // Clear transaction
        tx.vin.clear();
        tx.vout.clear();
        tx.witness_version = 0xFF;  // Default to legacy (no witness)
        tx.has_explicit_fee = false;
        tx.shielded_bundle_bytes.clear();
        // Phase M.6.1: Use AmountUna::Zero()
        tx.explicit_fee = AmountUna::Zero();

        // Read version
        tx.version = static_cast<int32_t>(reader.readUint32());

        // Check for SegWit marker (0x00 0x01)
        bool is_segwit = false;
        if (reader.remaining() >= 2) {
            size_t peek_pos = reader.position();
            if (peek_pos + 1 < data.size() && data[peek_pos] == 0x00 && data[peek_pos + 1] == 0x01) {
                is_segwit = true;
                tx.witness_version = 0;
                reader.readUint8();  // 0x00
                reader.readUint8();  // 0x01
            }
        }

        // Read inputs
        uint64_t input_count = reader.readVarInt();
        for (uint64_t i = 0; i < input_count; i++) {
            TxInput input;

            // Read prevout txid (32 bytes) - Phase M.4.3-B: Deserialize to TxId
            auto txid_bytes = reader.readBytes(32);
            uint256 txid_raw;
            std::memcpy(txid_raw.data, txid_bytes.data(), 32);
            input.prevout.txid = TxId(txid_raw);

            // Read prevout vout
            input.prevout.vout = reader.readUint32();

            // Read scriptSig
            input.scriptSig = reader.readVarBytes();

            // Read sequence
            input.sequence = reader.readUint32();

            tx.vin.push_back(input);
        }

        // Read outputs
        uint64_t output_count = reader.readVarInt();

        bool has_confidential_outputs = false;
        for (uint64_t i = 0; i < output_count; i++) {
            TxOutput output;

            // Read value (8 bytes)
            const uint64_t raw_value_una = reader.readUint64();
            output.value = AmountUna::Una(raw_value_una);

            // Read scriptPubKey (varbytes)
            output.scriptPubKey = reader.readVarBytes();

            output.is_confidential = false;

            // Confidential output format (Phase I):
            // - Value marker (8 bytes) = 0
            // - ScriptPubKey (varbytes)
            // - Commitment (varbytes; typically 33 bytes)
            // - Range proof (varbytes)
            // - Nonce (varbytes; typically 65 bytes for ECDH)
            //
            // NOTE: value==0 is valid for transparent outputs (OP_RETURN / edge cases),
            // so we only treat this as confidential if the following fields decode
            // cleanly and match expected structural sizes.
            if (raw_value_una == 0) {
                const size_t after_spk_pos = reader.position();
                try {
                    auto commitment = reader.readVarBytes();
                    auto range_proof = reader.readVarBytes();
                    auto nonce = reader.readVarBytes();

                    if (commitment.size() == 33 && nonce.size() == 65) {
                        output.is_confidential = true;
                        output.commitment = std::move(commitment);
                        output.range_proof = std::move(range_proof);
                        output.nonce = std::move(nonce);
                        has_confidential_outputs = true;
                    } else {
                        // Not a confidential output; rewind and treat as transparent 0-value output.
                        reader.setPosition(after_spk_pos);
                    }
                } catch (...) {
                    // Not enough bytes / invalid encoding - treat as transparent.
                    reader.setPosition(after_spk_pos);
                }
            }

            tx.vout.push_back(output);
        }

        // Explicit fee marker (Phase I / v7 shielded):
        // Present when the tx has CT outputs OR shielded value semantics.
        // Shielded txs need this persisted too: replay/reindex cannot recover
        // the economic fee from transparent values alone when
        // bundle.value_balance moves value into/out of the pool.
        const bool is_shielded_tx = Transaction::IsShieldedVersion(tx.version);
        if (has_confidential_outputs || is_shielded_tx) {
            uint8_t fee_marker = reader.readUint8();
            if (fee_marker == 0x01) {
                uint64_t fee_una = reader.readUint64();
                tx.SetExplicitFee(fee_una);
            } else if (fee_marker == 0x00) {
                tx.has_explicit_fee = false;
                tx.explicit_fee = AmountUna::Zero();
            } else {
                throw std::runtime_error("Invalid explicit fee marker");
            }
        }

        // Read witness data
        if (is_segwit) {
            for (uint64_t i = 0; i < input_count; i++) {
                uint64_t witness_count = reader.readVarInt();
                std::vector<std::vector<uint8_t>> witness_stack;
                for (uint64_t j = 0; j < witness_count; j++) {
                    witness_stack.push_back(reader.readVarBytes());
                }
                // Store witness data in transaction input
                tx.vin[i].witness = witness_stack;
            }
        }

        // Shielded bundle bytes are serialized after witness data and before
        // locktime. v5 carries them in witness-inclusive form for legacy
        // blocks; v6 carries them in both txid and witness-inclusive forms.
        if (is_shielded_tx) {
            tx.shielded_bundle_bytes = reader.readVarBytes();
        }

        // Read locktime
        tx.lockTime = reader.readUint32();

        consumed_out = reader.position();
        return true;

    } catch (const std::exception& e) {
        return false;
    }
}

} // namespace dinero
