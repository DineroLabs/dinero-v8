// Copyright (c) 2024 The Dinero Core developers
// Distributed under the MIT software license

#ifndef DINERO_WALLET_WIRE_PROTOCOL_H
#define DINERO_WALLET_WIRE_PROTOCOL_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dinero {
namespace lightning {

/**
 * Wallet Wire Protocol (Bitcoin-style serialization)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Simple binary protocol for wallet<->lightning communication in release builds.
 * Replaces protobuf/gRPC with zero-dependency binary serialization.
 *
 * Message format:
 *   [4 bytes] message_type (uint32_t, network byte order)
 *   [4 bytes] payload_size (uint32_t, network byte order)
 *   [N bytes] payload (serialized fields)
 *
 * Serialization rules (Bitcoin Core style):
 *   - Integers: Little-endian encoding
 *   - Strings: [varint length][bytes]
 *   - Byte arrays: [varint length][bytes]
 *   - Booleans: 1 byte (0x00 or 0x01)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

// Message type constants
enum class WalletMessageType : uint32_t {
    // Health check
    GET_NETWORK_HRP_REQUEST = 0x0001,
    GET_NETWORK_HRP_RESPONSE = 0x0002,

    // UTXO operations
    LIST_UTXOS_REQUEST = 0x0003,
    LIST_UTXOS_RESPONSE = 0x0004,

    // Lightning key derivation
    DERIVE_LIGHTNING_KEY_REQUEST = 0x0005,
    DERIVE_LIGHTNING_KEY_RESPONSE = 0x0006,

    // Taproot signing
    TAPROOT_SIGHASH_REQUEST = 0x0007,
    TAPROOT_SIGHASH_RESPONSE = 0x0008,

    // Address generation
    GET_CHANGE_ADDRESS_REQUEST = 0x0009,
    GET_CHANGE_ADDRESS_RESPONSE = 0x000A,

    // Key derivation for scriptPubKey
    DERIVE_KEY_FOR_SCRIPTPUBKEY_REQUEST = 0x000B,
    DERIVE_KEY_FOR_SCRIPTPUBKEY_RESPONSE = 0x000C,

    // Error response
    ERROR_RESPONSE = 0xFFFF,
};

// Lightning key types (matches proto enum)
enum class LightningKeyType : uint8_t {
    NODE_IDENTITY = 0,
    FUNDING = 1,
    REVOCATION_BASE = 2,
    PAYMENT_BASE = 3,
    DELAYED_PAYMENT_BASE = 4,
    HTLC_BASE = 5,
};

/**
 * Binary serializer/deserializer (Bitcoin Core style)
 */
class WireSerializer {
public:
    WireSerializer() : pos_(0) {}

    // ═══════════════════════════════════════════════════════════════════════════
    // Serialization (writing)
    // ═══════════════════════════════════════════════════════════════════════════

    void writeUint8(uint8_t value) {
        buffer_.push_back(value);
    }

    void writeUint32(uint32_t value) {
        buffer_.push_back(static_cast<uint8_t>(value & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }

    void writeUint64(uint64_t value) {
        buffer_.push_back(static_cast<uint8_t>(value & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
        buffer_.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    }

    void writeBool(bool value) {
        buffer_.push_back(value ? 0x01 : 0x00);
    }

    void writeVarInt(uint64_t value) {
        if (value < 0xFD) {
            writeUint8(static_cast<uint8_t>(value));
        } else if (value <= 0xFFFF) {
            writeUint8(0xFD);
            writeUint8(static_cast<uint8_t>(value & 0xFF));
            writeUint8(static_cast<uint8_t>((value >> 8) & 0xFF));
        } else if (value <= 0xFFFFFFFF) {
            writeUint8(0xFE);
            writeUint32(static_cast<uint32_t>(value));
        } else {
            writeUint8(0xFF);
            writeUint64(value);
        }
    }

    void writeString(const std::string& str) {
        writeVarInt(str.size());
        buffer_.insert(buffer_.end(), str.begin(), str.end());
    }

    void writeBytes(const std::vector<uint8_t>& bytes) {
        writeVarInt(bytes.size());
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    void writeBytesDirect(const std::vector<uint8_t>& bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    std::vector<uint8_t> finalize() const {
        return buffer_;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Deserialization (reading)
    // ═══════════════════════════════════════════════════════════════════════════

    void reset(const std::vector<uint8_t>& data) {
        buffer_ = data;
        pos_ = 0;
    }

    uint8_t readUint8() {
        if (pos_ >= buffer_.size()) throw std::runtime_error("Buffer underflow");
        return buffer_[pos_++];
    }

    uint32_t readUint32() {
        if (pos_ + 4 > buffer_.size()) throw std::runtime_error("Buffer underflow");
        uint32_t value =
            static_cast<uint32_t>(buffer_[pos_]) |
            (static_cast<uint32_t>(buffer_[pos_ + 1]) << 8) |
            (static_cast<uint32_t>(buffer_[pos_ + 2]) << 16) |
            (static_cast<uint32_t>(buffer_[pos_ + 3]) << 24);
        pos_ += 4;
        return value;
    }

    uint64_t readUint64() {
        if (pos_ + 8 > buffer_.size()) throw std::runtime_error("Buffer underflow");
        uint64_t value =
            static_cast<uint64_t>(buffer_[pos_]) |
            (static_cast<uint64_t>(buffer_[pos_ + 1]) << 8) |
            (static_cast<uint64_t>(buffer_[pos_ + 2]) << 16) |
            (static_cast<uint64_t>(buffer_[pos_ + 3]) << 24) |
            (static_cast<uint64_t>(buffer_[pos_ + 4]) << 32) |
            (static_cast<uint64_t>(buffer_[pos_ + 5]) << 40) |
            (static_cast<uint64_t>(buffer_[pos_ + 6]) << 48) |
            (static_cast<uint64_t>(buffer_[pos_ + 7]) << 56);
        pos_ += 8;
        return value;
    }

    bool readBool() {
        uint8_t val = readUint8();
        return val != 0;
    }

    uint64_t readVarInt() {
        uint8_t first = readUint8();
        if (first < 0xFD) {
            return first;
        } else if (first == 0xFD) {
            uint8_t low = readUint8();
            uint8_t high = readUint8();
            return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 8);
        } else if (first == 0xFE) {
            return readUint32();
        } else {
            return readUint64();
        }
    }

    std::string readString() {
        uint64_t len = readVarInt();
        if (pos_ + len > buffer_.size()) throw std::runtime_error("Buffer underflow");
        std::string str(buffer_.begin() + pos_, buffer_.begin() + pos_ + len);
        pos_ += len;
        return str;
    }

    std::vector<uint8_t> readBytes() {
        uint64_t len = readVarInt();
        if (pos_ + len > buffer_.size()) throw std::runtime_error("Buffer underflow");
        std::vector<uint8_t> bytes(buffer_.begin() + pos_, buffer_.begin() + pos_ + len);
        pos_ += len;
        return bytes;
    }

    std::vector<uint8_t> readBytesDirect(size_t count) {
        if (pos_ + count > buffer_.size()) throw std::runtime_error("Buffer underflow");
        std::vector<uint8_t> bytes(buffer_.begin() + pos_, buffer_.begin() + pos_ + count);
        pos_ += count;
        return bytes;
    }

    bool hasMore() const {
        return pos_ < buffer_.size();
    }

private:
    std::vector<uint8_t> buffer_;
    size_t pos_;
};

/**
 * UTXO wire representation (matches dinerod::UTXO proto)
 */
struct WireUTXO {
    std::vector<uint8_t> txid;          // 32 bytes
    uint32_t vout;
    uint64_t value;
    std::vector<uint8_t> scriptPubKey;
    uint32_t confirmations;
    bool is_coinbase;

    void serialize(WireSerializer& s) const {
        s.writeBytes(txid);
        s.writeUint32(vout);
        s.writeUint64(value);
        s.writeBytes(scriptPubKey);
        s.writeUint32(confirmations);
        s.writeBool(is_coinbase);
    }

    static WireUTXO deserialize(WireSerializer& s) {
        WireUTXO utxo;
        utxo.txid = s.readBytes();
        utxo.vout = s.readUint32();
        utxo.value = s.readUint64();
        utxo.scriptPubKey = s.readBytes();
        utxo.confirmations = s.readUint32();
        utxo.is_coinbase = s.readBool();
        return utxo;
    }
};

} // namespace lightning
} // namespace dinero

#endif // DINERO_WALLET_WIRE_PROTOCOL_H
