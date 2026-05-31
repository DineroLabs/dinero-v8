// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/utreexo_messages.h"
#include "common/serialization.h"
#include <cstring>

namespace dinero {

// ═════════════════════════════════════════════════════════════════════════════
// GetUtreexoProofMessage Implementation
// ═════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> GetUtreexoProofMessage::serialize() const {
    VectorWriter writer;

    // Serialize block hashes
    writer.writeVarInt(block_hashes.size());
    for (const auto& hash : block_hashes) {
        writer.writeUint256(hash);
    }

    // Serialize flags
    writer.write(flags);

    return writer.data();
}

bool GetUtreexoProofMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.empty()) return false;

    Reader reader(data);

    try {
        // Deserialize block hashes
        uint64_t num_hashes = reader.readVarInt();
        if (num_hashes > MAX_BATCH_SIZE) return false;

        block_hashes.clear();
        block_hashes.reserve(num_hashes);
        for (uint64_t i = 0; i < num_hashes; ++i) {
            block_hashes.push_back(reader.readUint256());
        }

        // Deserialize flags
        flags = reader.read<uint32_t>();

        return true;
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UtreexoProofMessage Implementation
// ═════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> UtreexoProofMessage::serialize() const {
    VectorWriter writer;

    // Serialize block hash
    writer.writeUint256(block_hash);

    // Serialize block height
    writer.write(block_height);

    // Serialize accumulator roots
    writer.write(accumulator_root_before.data(), 32);
    writer.write(accumulator_root_after.data(), 32);

    // Serialize proof data
    auto proof_bytes = proof_data.serialize();
    writer.writeVarInt(proof_bytes.size());
    writer.write(proof_bytes.data(), proof_bytes.size());

    return writer.data();
}

bool UtreexoProofMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32 + 4 + 32 + 32) return false;

    Reader reader(data);

    try {
        // Deserialize block hash
        block_hash = reader.readUint256();

        // Deserialize block height
        block_height = reader.read<uint32_t>();

        // Deserialize accumulator roots
        accumulator_root_before.resize(32);
        reader.read(accumulator_root_before.data(), 32);
        accumulator_root_after.resize(32);
        reader.read(accumulator_root_after.data(), 32);

        // Deserialize proof data
        uint64_t proof_size = reader.readVarInt();
        if (proof_size > MAX_PROOF_SIZE) return false;
        // Bound the allocation to bytes actually present, so a tiny packet
        // claiming a large proof_size can't force a multi-MB transient alloc
        // (DoS amplification) before the read below fails.
        if (proof_size > reader.remaining()) return false;

        std::vector<uint8_t> proof_bytes(proof_size);
        reader.read(proof_bytes.data(), proof_size);

        proof_data = consensus::BlockUtreexoData::deserialize(proof_bytes);

        return true;
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// GetUtreexoHeadersMessage Implementation
// ═════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> GetUtreexoHeadersMessage::serialize() const {
    VectorWriter writer;

    // Serialize version
    writer.write(version);

    // Serialize locator hashes
    writer.writeVarInt(locator_hashes.size());
    for (const auto& hash : locator_hashes) {
        writer.writeUint256(hash);
    }

    // Serialize hash stop
    writer.writeUint256(hash_stop);

    return writer.data();
}

bool GetUtreexoHeadersMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 4 + 1 + 32) return false;

    Reader reader(data);

    try {
        // Deserialize version
        version = reader.read<uint32_t>();

        // Deserialize locator hashes
        uint64_t num_locators = reader.readVarInt();
        if (num_locators > MAX_LOCATOR_SIZE) return false;

        locator_hashes.clear();
        locator_hashes.reserve(num_locators);
        for (uint64_t i = 0; i < num_locators; ++i) {
            locator_hashes.push_back(reader.readUint256());
        }

        // Deserialize hash stop
        hash_stop = reader.readUint256();

        return true;
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UtreexoHeadersMessage Implementation
// ═════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> UtreexoHeadersMessage::serialize() const {
    VectorWriter writer;

    // Serialize headers count
    writer.writeVarInt(headers.size());

    // Serialize each header (128 bytes in Dinero BlockHeader v1 format)
    for (const auto& header : headers) {
        auto header_bytes = header.SerializeForHash();
        writer.write(header_bytes.data(), header_bytes.size());
    }

    return writer.data();
}

bool UtreexoHeadersMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.empty()) return false;

    Reader reader(data);

    try {
        // Deserialize headers count
        uint64_t num_headers = reader.readVarInt();
        if (num_headers > MAX_HEADERS_COUNT) return false;

        headers.clear();
        headers.reserve(num_headers);

        // Deserialize each header (128 bytes each in Dinero BlockHeader v1 format)
        constexpr size_t HEADER_SIZE = 128;  // Dinero BlockHeader v1

        for (uint64_t i = 0; i < num_headers; ++i) {
            // Check if we have enough data remaining
            if (reader.remaining() < HEADER_SIZE) {
                return false;
            }

            BlockHeader header;

            // BlockHeader v1 layout (128 bytes):
            //   0x00 (4 bytes):  version (LE)
            //   0x04 (32 bytes): prev_block_hash (LE)
            //   0x24 (32 bytes): merkle_root (LE)
            //   0x44 (32 bytes): utreexo_root (LE)
            //   0x64 (8 bytes):  timestamp (LE)
            //   0x6C (4 bytes):  difficulty (LE)
            //   0x70 (4 bytes):  nonce (LE)
            //   0x74 (12 bytes): reserved (MUST be zero)

            // version (4 bytes, offset 0x00)
            header.version = reader.read<uint32_t>();

            // prev_block_hash (32 bytes, offset 0x04)
            uint256 prev_hash = reader.readUint256();
            header.prev_block_hash = prev_hash;

            // merkle_root (32 bytes, offset 0x24)
            uint256 merkle_root = reader.readUint256();
            header.merkle_root = merkle_root;

            // utreexo_root (32 bytes, offset 0x44)
            uint256 utreexo_root = reader.readUint256();
            header.utreexo_root = utreexo_root;

            // timestamp (8 bytes, offset 0x64)
            header.timestamp = reader.read<uint64_t>();

            // difficulty (4 bytes, offset 0x6C)
            header.difficulty = reader.read<uint32_t>();

            // nonce (4 bytes, offset 0x70)
            header.nonce = reader.read<uint32_t>();

            // reserved (12 bytes, offset 0x74) - skip for now
            uint8_t reserved[12];
            reader.read(reserved, 12);
            std::memcpy(header.reserved, reserved, 12);

            headers.push_back(header);
        }

        return true;
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UtreexoProofNackMessage Implementation
// ═════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> UtreexoProofNackMessage::serialize() const {
    VectorWriter writer;

    // Reason code (1 byte)
    writer.write(static_cast<uint8_t>(reason));

    // Suggested retry delay (4 bytes)
    writer.write(retry_after_ms);

    // Rejected block hashes
    writer.writeVarInt(block_hashes.size());
    for (const auto& hash : block_hashes) {
        writer.writeUint256(hash);
    }

    return writer.data();
}

bool UtreexoProofNackMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 1 + 4 + 1) return false;  // reason + retry_after + varint(0)

    Reader reader(data);

    try {
        // Reason code
        uint8_t reason_byte = reader.read<uint8_t>();
        if (reason_byte > static_cast<uint8_t>(ProofNackReason::SHUTDOWN)) return false;
        reason = static_cast<ProofNackReason>(reason_byte);

        // Retry delay
        retry_after_ms = reader.read<uint32_t>();

        // Block hashes
        uint64_t num_hashes = reader.readVarInt();
        if (num_hashes > MAX_NACK_HASHES) return false;

        block_hashes.clear();
        block_hashes.reserve(num_hashes);
        for (uint64_t i = 0; i < num_hashes; ++i) {
            block_hashes.push_back(reader.readUint256());
        }

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace dinero
