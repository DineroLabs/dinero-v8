#include "consensus/csn_replay_data.h"

#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dinero::consensus {
namespace {

constexpr size_t kHashBytes = 32;
constexpr uint32_t kMaxEntries = 1'000'000;
constexpr char kMagic[] = {'C', 'S', 'N', '2'};

bool SupportedFormat(uint8_t version) {
    return version == 4 || version == 5 || version == 6;
}

bool Skip(const std::string& blob, size_t& offset, size_t size) {
    if (offset > blob.size() || size > blob.size() - offset) {
        return false;
    }
    offset += size;
    return true;
}

bool ReadU32(const std::string& blob, size_t& offset, uint32_t& value) {
    if (offset > blob.size() || blob.size() - offset < 4) {
        return false;
    }
    value = static_cast<uint8_t>(blob[offset]) |
            (static_cast<uint32_t>(static_cast<uint8_t>(blob[offset + 1])) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(blob[offset + 2])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(blob[offset + 3])) << 24);
    offset += 4;
    return true;
}

void AppendU32(std::string& blob, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        blob.push_back(static_cast<char>(value >> shift));
    }
}

// Preflight every length before copying the blob or allocating output vectors.
// Counts alone must not authorize large allocations from a short/corrupt row.
bool CheckSpentRecords(const std::string& blob, size_t offset,
                       uint32_t count, uint8_t version) {
    const size_t minimum_size = 12 + (version >= 5 ? 5 : 0) + (version >= 6 ? 5 : 0);
    if (count > (blob.size() - offset) / minimum_size) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t script_size = 0;
        if (!Skip(blob, offset, 8) || !ReadU32(blob, offset, script_size) ||
            !Skip(blob, offset, script_size)) {
            return false;
        }
        if (version >= 6 && !Skip(blob, offset, 5)) {
            return false;
        }
        if (version >= 5) {
            uint32_t commitment_size = 0;
            if (!Skip(blob, offset, 1) || !ReadU32(blob, offset, commitment_size) ||
                !Skip(blob, offset, commitment_size)) {
                return false;
            }
        }
    }
    return offset == blob.size();
}

void AddSize(size_t& total, size_t added, size_t maximum) {
    if (added > maximum - total) {
        throw std::length_error("CSN replay record is too large");
    }
    total += added;
}

}  // namespace

bool DecodeCsnReplayData(const std::string& blob, CsnReplayData& out) {
    out = {};
    try {
        size_t offset = 0;
        const bool with_metadata = blob.size() >= sizeof(kMagic) &&
            std::memcmp(blob.data(), kMagic, sizeof(kMagic)) == 0;
        if (with_metadata) {
            offset = sizeof(kMagic);
        }

        uint32_t target_count = 0;
        if (!ReadU32(blob, offset, target_count) || target_count > kMaxEntries ||
            target_count > (blob.size() - offset) / kHashBytes) {
            return false;
        }
        const size_t targets_offset = offset;
        offset += static_cast<size_t>(target_count) * kHashBytes;

        uint32_t spent_count = 0;
        uint8_t format_version = 0;
        if (with_metadata) {
            if (!ReadU32(blob, offset, spent_count) || spent_count > kMaxEntries ||
                offset == blob.size()) {
                return false;
            }
            format_version = static_cast<uint8_t>(blob[offset++]);
            if (!SupportedFormat(format_version) ||
                !CheckSpentRecords(blob, offset, spent_count, format_version)) {
                return false;
            }
        } else if (offset != blob.size()) {
            return false;
        }

        CsnReplayData parsed;
        parsed.spend_targets.reserve(target_count);
        for (size_t i = 0; i < target_count; ++i) {
            const size_t start = targets_offset + i * kHashBytes;
            parsed.spend_targets.emplace_back(blob.begin() + start,
                                              blob.begin() + start + kHashBytes);
        }
        if (with_metadata) {
            const std::vector<uint8_t> bytes(blob.begin(), blob.end());
            parsed.spent_outputs.reserve(spent_count);
            for (uint32_t i = 0; i < spent_count; ++i) {
                const size_t before = offset;
                auto spent = SpentOutputData::deserialize(bytes, offset, format_version);
                if (offset <= before || offset > bytes.size()) {
                    return false;
                }
                parsed.spent_outputs.push_back(std::move(spent));
            }
            if (offset != blob.size()) {
                return false;
            }
            parsed.has_spent_outputs = true;
        }
        out = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string SerializeCsnReplayData(
    const std::vector<UtreexoHash>& targets,
    const std::vector<SpentOutputData>& spent_outputs,
    uint8_t format_version) {
    if (!SupportedFormat(format_version)) {
        throw std::invalid_argument("Unsupported CSN replay metadata format");
    }
    if (targets.size() > kMaxEntries || spent_outputs.size() > kMaxEntries) {
        throw std::length_error("CSN replay entry count exceeds limit");
    }

    std::string blob;
    size_t total_size = 13;  // CSN2, target count, spent count, format version.
    for (const auto& target : targets) {
        if (target.size() != kHashBytes) {
            throw std::invalid_argument("CSN replay target must be 32 bytes");
        }
        AddSize(total_size, kHashBytes, blob.max_size());
    }
    for (const auto& spent : spent_outputs) {
        if (spent.scriptPubKey.size() > std::numeric_limits<uint32_t>::max() ||
            (format_version >= 5 && spent.commitment.size() > std::numeric_limits<uint32_t>::max())) {
            throw std::length_error("CSN replay metadata field exceeds wire length");
        }
        AddSize(total_size, 12 + (format_version >= 5 ? 5 : 0) +
                              (format_version >= 6 ? 5 : 0), blob.max_size());
        AddSize(total_size, spent.scriptPubKey.size(), blob.max_size());
        if (format_version >= 5) {
            AddSize(total_size, spent.commitment.size(), blob.max_size());
        }
    }

    blob.reserve(total_size);
    blob.append(kMagic, sizeof(kMagic));
    AppendU32(blob, static_cast<uint32_t>(targets.size()));
    for (const auto& target : targets) {
        blob.append(reinterpret_cast<const char*>(target.data()), target.size());
    }
    AppendU32(blob, static_cast<uint32_t>(spent_outputs.size()));
    blob.push_back(static_cast<char>(format_version));
    for (const auto& spent : spent_outputs) {
        const auto bytes = spent.serialize(format_version);
        blob.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return blob;
}

}  // namespace dinero::consensus
