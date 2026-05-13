#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <stdexcept>
#include "status.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "primitives/transaction.h"  // Phase M.1: primitives, not wallet

namespace dinero {

// Forward declarations for serialization types
struct HeaderInfo;
struct TipInfo;

// Big-endian key encoding utilities
static inline void put_be32(std::string& k, uint32_t n) {
    k.push_back((n >> 24) & 0xFF);
    k.push_back((n >> 16) & 0xFF);
    k.push_back((n >> 8) & 0xFF);
    k.push_back(n & 0xFF);
}

static inline uint32_t get_be32(const std::string& k, size_t offset = 0) {
    if (k.size() < offset + 4) return 0;
    return (static_cast<uint32_t>(k[offset]) << 24) |
           (static_cast<uint32_t>(k[offset + 1]) << 16) |
           (static_cast<uint32_t>(k[offset + 2]) << 8) |
           static_cast<uint32_t>(k[offset + 3]);
}

// Unified height key encoding (big-endian)
static inline std::string KH(uint32_t height) {
    std::string k(1, 'H');
    put_be32(k, height);
    return k;
}

// Binary serialization utilities for RocksDB storage
class VectorWriter {
public:
    VectorWriter() = default;
    
    void write(const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
    }
    
    template<typename T>
    void write(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        write(&value, sizeof(T));
    }
    
    void writeVarInt(uint64_t value) {
        while (value >= 0x80) {
            buffer_.push_back(static_cast<uint8_t>(value | 0x80));
            value >>= 7;
        }
        buffer_.push_back(static_cast<uint8_t>(value));
    }
    
    void writeString(const std::string& str) {
        writeVarInt(str.size());
        write(str.data(), str.size());
    }
    
    void writeBytes(const std::vector<uint8_t>& bytes) {
        writeVarInt(bytes.size());
        write(bytes.data(), bytes.size());
    }

    void writeUint256(const uint256& hash) {
        write(hash.data, 32);
    }

    std::string release_string() {
        return std::string(buffer_.begin(), buffer_.end());
    }

    const std::vector<uint8_t>& data() const { return buffer_; }

private:
    std::vector<uint8_t> buffer_;
};

class Reader {
public:
    Reader(const std::string& data) : data_(data), pos_(0) {}
    Reader(const std::vector<uint8_t>& data) : pos_(0) {
        data_.assign(data.begin(), data.end());
    }
    
    void read(void* dest, size_t size) {
        if (pos_ + size > data_.size()) {
            throw std::runtime_error("Reader: insufficient data");
        }
        std::memcpy(dest, data_.data() + pos_, size);
        pos_ += size;
    }
    
    template<typename T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        T value;
        read(&value, sizeof(T));
        return value;
    }
    
    uint64_t readVarInt() {
        uint64_t result = 0;
        for (unsigned shift = 0; pos_ < data_.size(); shift += 7) {
            if (shift >= 70) {
                throw std::runtime_error("Reader: VarInt too large (>10 bytes)");
            }
            uint8_t byte = data_[pos_++];
            uint64_t payload = byte & 0x7F;
            // 10th byte (shift=63): only the LSB survives in uint64_t
            if (shift == 63 && payload > 1) {
                throw std::runtime_error("Reader: VarInt overflow");
            }
            result |= payload << shift;
            if ((byte & 0x80) == 0) {
                return result;
            }
        }
        throw std::runtime_error("Reader: incomplete VarInt");
    }
    
    std::string readString() {
        uint64_t size = readVarInt();
        if (size > 1024 * 1024) { // 1MB limit
            throw std::runtime_error("Reader: string too large");
        }
        std::string result(size, '\0');
        read(result.data(), size);
        return result;
    }
    
    std::vector<uint8_t> readBytes() {
        uint64_t size = readVarInt();
        if (size > 1024 * 1024) { // 1MB limit
            throw std::runtime_error("Reader: bytes too large");
        }
        std::vector<uint8_t> result(size);
        read(result.data(), size);
        return result;
    }

    uint256 readUint256() {
        uint256 result;
        read(result.data, 32);
        return result;
    }

    bool eof() const { return pos_ >= data_.size(); }
    size_t remaining() const { return data_.size() - pos_; }
    size_t position() const { return pos_; }
    void setPosition(size_t pos) { pos_ = pos; }

    // Skip bytes without reading
    void skip(size_t count) {
        if (pos_ + count > data_.size()) {
            throw std::runtime_error("Reader: insufficient data to skip");
        }
        pos_ += count;
    }

private:
    std::string data_;
    size_t pos_;
};

// Serialization interface
template<class S, typename T>
void Ser(S& s, const T& obj);

// Helper functions
template<typename T>
std::string SerializeToString(const T& obj) {
    VectorWriter w;
    Serialize(w, obj);
    return w.release_string();
}

template<typename T>
Status ParseFrom(const std::string& buf, T& out) {
    try {
        Reader r(buf);
        Deserialize(r, out);
        return Status::Ok;
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

// Use canonical types from primitives/

// ═══════════════════════════════════════════════════════════════════════════════
// CONSENSUS-CRITICAL: BlockHeader Wire Format (128 bytes)
// ═══════════════════════════════════════════════════════════════════════════════
// This MUST match SerializeForHash() in block.cpp EXACTLY.
// Any field order mismatch breaks P2P relay (timestamp reads from wrong offset).
//
// Canonical Layout (little-endian):
//   Offset 0x00 (4 bytes):   version
//   Offset 0x04 (32 bytes):  prev_block_hash
//   Offset 0x24 (32 bytes):  merkle_root
//   Offset 0x44 (32 bytes):  utreexo_root      ← MUST come before timestamp!
//   Offset 0x64 (8 bytes):   timestamp
//   Offset 0x6C (4 bytes):   difficulty
//   Offset 0x70 (4 bytes):   nonce
//   Offset 0x74 (12 bytes):  reserved
//
// INVARIANT: kHeaderWireSize == 128
// ═══════════════════════════════════════════════════════════════════════════════
static constexpr size_t kHeaderWireSize = 128;

// Compile-time verification that wire size matches expected layout
static_assert(kHeaderWireSize == 4 + 32 + 32 + 32 + 8 + 4 + 4 + 12,
              "BlockHeader wire size must be 128 bytes (version + prev_hash + merkle + utreexo + timestamp + difficulty + nonce + reserved)");

// Serialization implementations
// Phase M.1: Binary uint256 serialization
template<typename S>
void Serialize(S& s, const BlockHeader& h) {
    // CRITICAL: Field order must match SerializeForHash() in block.cpp (128-byte format)
    // Layout: version(4) | prev_hash(32) | merkle(32) | utreexo(32) | timestamp(8) | difficulty(4) | nonce(4) | reserved(12)
    s.write(h.version);
    s.write(h.prev_block_hash);      // Phase M.1: Binary uint256
    s.write(h.merkle_root);          // Phase M.1: Binary uint256
    s.write(h.utreexo_root);         // Phase M.1: Binary uint256 - MUST come before timestamp!
    s.write(h.timestamp);
    s.write(h.difficulty);           // Phase 2: Renamed from 'bits'
    s.write(h.nonce);
    // Write reserved[12] from the header struct
    s.write(h.reserved, 12);
}

template<typename S>
void Deserialize(S& s, BlockHeader& h) {
    // CRITICAL: Field order must match SerializeForHash() in block.cpp (128-byte format)
    // Layout: version(4) | prev_hash(32) | merkle(32) | utreexo(32) | timestamp(8) | difficulty(4) | nonce(4) | reserved(12)
    h.version = s.template read<uint32_t>();
    h.prev_block_hash = s.template read<uint256>();     // Phase M.1: Binary uint256
    h.prev_block_hash = h.prev_block_hash;                // Phase M.1: Populate binary field
    h.merkle_root = s.template read<uint256>();         // Phase M.1: Binary uint256
    h.utreexo_root = s.template read<uint256>();  // Phase M.1: Binary uint256 - MUST come before timestamp!
    h.timestamp = s.template read<uint64_t>();
    h.difficulty = s.template read<uint32_t>();         // Phase 2: Renamed from 'bits'
    h.nonce = s.template read<uint32_t>();
    // Read reserved[12] field
    s.read(h.reserved, 12);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Round-trip verification for BlockHeader serialization
// Call this at startup or in tests to catch serialization regressions.
// ═══════════════════════════════════════════════════════════════════════════════
inline bool VerifyBlockHeaderSerializationRoundTrip() {
    // Construct header with known non-zero values to detect field swaps
    BlockHeader original{};
    original.version = 0x12345678;

    // Use distinct patterns for each uint256 field to detect swaps
    std::memset(original.prev_block_hash.data, 0xAA, 32);
    std::memset(original.merkle_root.data, 0xBB, 32);
    std::memset(original.utreexo_root.data, 0xCC, 32);  // Critical: must not swap with timestamp

    original.timestamp = 1737000000ULL;  // Known reasonable timestamp (2025)
    original.difficulty = 0xDEADBEEF;
    original.nonce = 0xCAFEBABE;
    std::memset(original.reserved, 0x42, 12);  // Non-zero pattern

    // Serialize
    VectorWriter writer;
    Serialize(writer, original);
    std::string serialized = writer.release_string();

    // Verify wire size
    if (serialized.size() != kHeaderWireSize) {
        return false;  // Wrong size
    }

    // Deserialize
    Reader reader(serialized);
    BlockHeader roundtrip{};
    Deserialize(reader, roundtrip);

    // Verify all fields match
    if (roundtrip.version != original.version) return false;
    if (std::memcmp(roundtrip.prev_block_hash.data, original.prev_block_hash.data, 32) != 0) return false;
    if (std::memcmp(roundtrip.merkle_root.data, original.merkle_root.data, 32) != 0) return false;
    if (std::memcmp(roundtrip.utreexo_root.data, original.utreexo_root.data, 32) != 0) return false;
    if (roundtrip.timestamp != original.timestamp) return false;  // CRITICAL: timestamp must match
    if (roundtrip.difficulty != original.difficulty) return false;
    if (roundtrip.nonce != original.nonce) return false;
    if (std::memcmp(roundtrip.reserved, original.reserved, 12) != 0) return false;

    return true;
}

template<typename S>
void Serialize(S& s, const TxInput& txin) {
    s.write(txin.prevout.txid);  // Phase M.0: uint256 is binary, not string
    s.write(txin.prevout.vout);
    s.writeBytes(txin.scriptSig);
    s.write(txin.sequence);
}

template<typename S>
void Deserialize(S& s, TxInput& txin) {
    txin.prevout.txid = TxId(s.template read<uint256>());  // Phase M.4: Wrap in TxId semantic type
    txin.prevout.vout = s.template read<uint32_t>();
    txin.scriptSig = s.readBytes();
    txin.sequence = s.template read<uint32_t>();
}

template<typename S>
void Serialize(S& s, const TxOutput& txout) {
    if (txout.is_confidential) {
        s.write(AmountUna::Zero());  // Confidential marker: value = 0
        s.writeBytes(txout.scriptPubKey);
        s.writeBytes(txout.commitment);
        s.writeBytes(txout.range_proof);
        s.writeBytes(txout.nonce);
    } else {
        s.write(txout.value);
        s.writeBytes(txout.scriptPubKey);
    }
}

template<typename S>
void Deserialize(S& s, TxOutput& txout) {
    // Phase M.6.2: Wrap deserialized value in AmountUna
    uint64_t raw_value = s.template read<uint64_t>();
    txout.value = AmountUna::Una(raw_value);
    txout.scriptPubKey = s.readBytes();

    // CT output detection: if value == 0, try to read confidential fields
    txout.is_confidential = false;
    if (raw_value == 0 && !s.eof() && s.remaining() > 0) {
        size_t saved_pos = s.position();
        try {
            auto commitment = s.readBytes();
            auto range_proof = s.readBytes();
            auto nonce = s.readBytes();

            if (commitment.size() == 33 && nonce.size() == 65) {
                txout.is_confidential = true;
                txout.commitment = std::move(commitment);
                txout.range_proof = std::move(range_proof);
                txout.nonce = std::move(nonce);
            } else {
                s.setPosition(saved_pos);
            }
        } catch (...) {
            s.setPosition(saved_pos);
        }
    }
}

template<typename S>
void Serialize(S& s, const Transaction& tx) {
    const auto bytes = tx.Serialize(TxSerializationMode::WithWitness);
    if (!bytes.empty()) {
        s.write(bytes.data(), bytes.size());
    }
}

template<typename S>
void Deserialize(S& s, Transaction& tx) {
    const size_t saved_pos = s.position();
    std::vector<uint8_t> remaining(s.remaining());
    if (!remaining.empty()) {
        s.read(remaining.data(), remaining.size());
        s.setPosition(saved_pos);
    }

    size_t consumed = 0;
    if (!TransactionSerializer::Deserialize(tx, remaining, consumed)) {
        throw std::runtime_error("Deserialize(Transaction): TransactionSerializer failed");
    }
    s.skip(consumed);
}

// Canonical serializers for HeaderInfo/TipInfo
struct HeaderInfo {
    std::string hash;
    uint32_t height;
    uint64_t work;
    uint32_t timestamp;
    std::string prev_hash;
};

// TipInfo is now defined in storage/tip_info.h

// HeaderInfo serialization (fixed-width little-endian)
bool Serialize(const HeaderInfo& header, std::string& out);
bool Deserialize(std::string_view data, HeaderInfo& header);

// TipInfo serialization (fixed-width little-endian)  
bool Serialize(const TipInfo& tip, std::string& out);
bool Deserialize(std::string_view data, TipInfo& tip);

template<class S>
void Serialize(S& s, const Block& b) {
    Serialize(s, b.header);
    s.writeVarInt(b.vtx.size());
    for (const auto& tx : b.vtx) {
        Serialize(s, tx);
    }

    // Phase 1: Serialize optional Utreexo data
    // Format: 1 byte flag + optional BlockUtreexoData
    if (b.utreexo.has_value()) {
        s.template write<uint8_t>(0x01);  // Flag: has Utreexo data
        std::vector<uint8_t> utreexo_bytes = b.utreexo->serialize();
        s.writeBytes(utreexo_bytes);
    } else {
        s.template write<uint8_t>(0x00);  // Flag: no Utreexo data
    }
}

template<class S>
void Deserialize(S& s, Block& b) {
    Deserialize(s, b.header);
    size_t tx_count = s.readVarInt();
    b.vtx.resize(tx_count);
    for (auto& tx : b.vtx) {
        Deserialize(s, tx);
    }

    // Phase 1: Deserialize optional Utreexo data
    // Format: 1 byte flag + optional BlockUtreexoData
    // Check if there's remaining data for Utreexo field
    if (!s.eof() && s.remaining() > 0) {
        uint8_t utreexo_flag = s.template read<uint8_t>();
        if (utreexo_flag == 0x01) {
            // Has Utreexo data
            std::vector<uint8_t> utreexo_bytes = s.readBytes();
            b.utreexo = consensus::BlockUtreexoData::deserialize(utreexo_bytes);
        } else {
            // No Utreexo data
            b.utreexo = std::nullopt;
        }
    } else {
        // Backward compatibility: no Utreexo data in old blocks
        b.utreexo = std::nullopt;
    }
}

} // namespace dinero
