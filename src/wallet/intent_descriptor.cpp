#include "wallet/intent_descriptor.h"
#include "crypto/tagged_hash.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace dinero {

using crypto::TaggedHashArray;

static void WriteUint32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

static void WriteUint64LE(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
    out.push_back((value >> 32) & 0xff);
    out.push_back((value >> 40) & 0xff);
    out.push_back((value >> 48) & 0xff);
    out.push_back((value >> 56) & 0xff);
}

static uint32_t ReadUint32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t ReadUint64LE(const uint8_t* p) {
    return static_cast<uint64_t>(p[0]) |
           (static_cast<uint64_t>(p[1]) << 8) |
           (static_cast<uint64_t>(p[2]) << 16) |
           (static_cast<uint64_t>(p[3]) << 24) |
           (static_cast<uint64_t>(p[4]) << 32) |
           (static_cast<uint64_t>(p[5]) << 40) |
           (static_cast<uint64_t>(p[6]) << 48) |
           (static_cast<uint64_t>(p[7]) << 56);
}

std::vector<uint8_t> IntentDescriptor::Serialize() const {
    std::vector<uint8_t> out;
    // Fixed overhead: 32 + 8 + 8 + 4 + 1 = 53 bytes minimum
    out.reserve(53 + purpose_tag.size());

    // recipient_hash (32 bytes)
    out.insert(out.end(), recipient_hash.begin(), recipient_hash.end());

    // amount (8 bytes LE)
    WriteUint64LE(out, amount);

    // max_fee (8 bytes LE)
    WriteUint64LE(out, max_fee);

    // expiry_height (4 bytes LE)
    WriteUint32LE(out, expiry_height);

    // purpose_tag: length (1 byte) + data (truncated to 64 bytes)
    size_t tag_len = std::min(purpose_tag.size(), size_t{64});
    out.push_back(static_cast<uint8_t>(tag_len));
    if (tag_len > 0) {
        out.insert(out.end(), purpose_tag.begin(), purpose_tag.begin() + tag_len);
    }

    return out;
}

std::array<uint8_t, 32> IntentDescriptor::ComputeExtCommitment() const {
    auto serialized = Serialize();
    return TaggedHashArray("dinero/intent/v1", serialized);
}

IntentDescriptor IntentDescriptor::Deserialize(const std::vector<uint8_t>& data) {
    // Minimum size: 32 + 8 + 8 + 4 + 1 = 53 bytes
    if (data.size() < 53) {
        throw std::runtime_error("IntentDescriptor: data too short");
    }

    IntentDescriptor desc;
    size_t offset = 0;

    // recipient_hash (32 bytes)
    std::copy(data.begin() + offset, data.begin() + offset + 32, desc.recipient_hash.begin());
    offset += 32;

    // amount (8 bytes LE)
    desc.amount = ReadUint64LE(data.data() + offset);
    offset += 8;

    // max_fee (8 bytes LE)
    desc.max_fee = ReadUint64LE(data.data() + offset);
    offset += 8;

    // expiry_height (4 bytes LE)
    desc.expiry_height = ReadUint32LE(data.data() + offset);
    offset += 4;

    // purpose_tag: length (1 byte) + data
    uint8_t tag_len = data[offset];
    offset += 1;

    if (tag_len > 64) {
        throw std::runtime_error("IntentDescriptor: purpose_tag too long");
    }

    if (offset + tag_len > data.size()) {
        throw std::runtime_error("IntentDescriptor: data truncated");
    }

    desc.purpose_tag = std::string(data.begin() + offset, data.begin() + offset + tag_len);

    return desc;
}

} // namespace dinero
