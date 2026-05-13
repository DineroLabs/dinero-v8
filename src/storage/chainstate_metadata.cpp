#include "storage/chainstate_metadata.h"
#include "common/logger.h"
#include <fstream>
#include <cstring>

namespace dinero {

ChainstateMetadata::ChainstateMetadata(const std::filesystem::path& datadir)
    : datadir_(datadir)
    , metadata_path_(datadir / "chainstate" / "metadata.dat")
{
    // Ensure chainstate directory exists
    std::filesystem::create_directories(datadir_ / "chainstate");
}

ChainstateMetadata::~ChainstateMetadata() = default;

StatusOr<ChainstateMetadata::Metadata> ChainstateMetadata::load() {
    if (!std::filesystem::exists(metadata_path_)) {
        return Status::NotFound;
    }

    // Read file
    std::ifstream file(metadata_path_, std::ios::binary);
    if (!file.is_open()) {
        return Status::fromError("Failed to open metadata file: " + metadata_path_.string());
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read data
    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    file.close();

    if (file_size < 91) {  // Minimum size (magic + version + ... + checksum)
        return Status::fromError("Metadata file too small: " + std::to_string(file_size) + " bytes");
    }

    // Deserialize
    return deserialize(data);
}

Status ChainstateMetadata::save(const Metadata& metadata) {
    // Serialize metadata
    std::vector<uint8_t> data = serialize(metadata);

    // Write to temporary file (atomic write)
    std::filesystem::path temp_path = metadata_path_.string() + ".tmp";

    std::ofstream file(temp_path, std::ios::binary);
    if (!file.is_open()) {
        return Status::fromError("Failed to create temp metadata file: " + temp_path.string());
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();

    if (!file.good()) {
        return Status::fromError("Failed to write metadata to: " + temp_path.string());
    }

    // Atomic rename
    std::error_code ec;
    std::filesystem::rename(temp_path, metadata_path_, ec);
    if (ec) {
        return Status::fromError("Failed to rename temp file: " + ec.message());
    }

    LOG_INFO("💾 Chainstate metadata saved: height=" + std::to_string(metadata.tip.height) +
             ", hash=" + metadata.tip.hash.ToString().substr(0, 16) + "...");

    return Status::Ok;
}

Status ChainstateMetadata::remove() {
    if (!std::filesystem::exists(metadata_path_)) {
        return Status::Ok;  // Already deleted
    }

    std::error_code ec;
    std::filesystem::remove(metadata_path_, ec);
    if (ec) {
        return Status::fromError("Failed to remove metadata file: " + ec.message());
    }

    LOG_INFO("🗑️  Chainstate metadata removed (forces full validation on next startup)");
    return Status::Ok;
}

bool ChainstateMetadata::exists() const {
    return std::filesystem::exists(metadata_path_);
}

std::vector<uint8_t> ChainstateMetadata::serialize(const Metadata& metadata) const {
    std::vector<uint8_t> data;
    data.reserve(91);  // Fixed size

    // Magic (4 bytes)
    data.push_back((MAGIC >> 24) & 0xFF);
    data.push_back((MAGIC >> 16) & 0xFF);
    data.push_back((MAGIC >> 8) & 0xFF);
    data.push_back(MAGIC & 0xFF);

    // Version (4 bytes)
    data.push_back((VERSION >> 24) & 0xFF);
    data.push_back((VERSION >> 16) & 0xFF);
    data.push_back((VERSION >> 8) & 0xFF);
    data.push_back(VERSION & 0xFF);

    // Best block hash (32 bytes)
    std::string hash_str = metadata.tip.hash.ToString();
    for (size_t i = 0; i < 64; i += 2) {
        uint8_t byte = std::stoi(hash_str.substr(i, 2), nullptr, 16);
        data.push_back(byte);
    }

    // Best block height (4 bytes)
    uint32_t height = static_cast<uint32_t>(metadata.tip.height);
    data.push_back((height >> 24) & 0xFF);
    data.push_back((height >> 16) & 0xFF);
    data.push_back((height >> 8) & 0xFF);
    data.push_back(height & 0xFF);

    // Chainwork (32 bytes) - simplified, store as string padded
    std::string work_str = metadata.tip.work.ToString();
    work_str.resize(64, '0');  // Pad to 64 hex chars
    for (size_t i = 0; i < 64; i += 2) {
        uint8_t byte = std::stoi(work_str.substr(i, 2), nullptr, 16);
        data.push_back(byte);
    }

    // Best block timestamp (4 bytes)
    uint32_t timestamp = metadata.tip.timestamp;
    data.push_back((timestamp >> 24) & 0xFF);
    data.push_back((timestamp >> 16) & 0xFF);
    data.push_back((timestamp >> 8) & 0xFF);
    data.push_back(timestamp & 0xFF);

    // IBD state (1 byte)
    data.push_back(metadata.is_ibd ? 1 : 0);

    // Last flush timestamp (8 bytes)
    uint64_t flush_time = metadata.last_flush_time;
    data.push_back((flush_time >> 56) & 0xFF);
    data.push_back((flush_time >> 48) & 0xFF);
    data.push_back((flush_time >> 40) & 0xFF);
    data.push_back((flush_time >> 32) & 0xFF);
    data.push_back((flush_time >> 24) & 0xFF);
    data.push_back((flush_time >> 16) & 0xFF);
    data.push_back((flush_time >> 8) & 0xFF);
    data.push_back(flush_time & 0xFF);

    // Checksum (4 bytes)
    uint32_t checksum = calculateChecksum(data.data(), data.size());
    data.push_back((checksum >> 24) & 0xFF);
    data.push_back((checksum >> 16) & 0xFF);
    data.push_back((checksum >> 8) & 0xFF);
    data.push_back(checksum & 0xFF);

    return data;
}

StatusOr<ChainstateMetadata::Metadata> ChainstateMetadata::deserialize(const std::vector<uint8_t>& data) const {
    if (data.size() < 91) {
        return Status::fromError("Invalid metadata size: " + std::to_string(data.size()));
    }

    size_t offset = 0;

    // Magic (4 bytes)
    uint32_t magic = (static_cast<uint32_t>(data[offset]) << 24) |
                     (static_cast<uint32_t>(data[offset + 1]) << 16) |
                     (static_cast<uint32_t>(data[offset + 2]) << 8) |
                     static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    if (magic != MAGIC) {
        return Status::fromError("Invalid magic: 0x" + std::to_string(magic));
    }

    // Version (4 bytes)
    uint32_t version = (static_cast<uint32_t>(data[offset]) << 24) |
                       (static_cast<uint32_t>(data[offset + 1]) << 16) |
                       (static_cast<uint32_t>(data[offset + 2]) << 8) |
                       static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    if (version != VERSION) {
        return Status::fromError("Unsupported version: " + std::to_string(version));
    }

    Metadata metadata;

    // Best block hash (32 bytes)
    std::string hash_hex;
    for (size_t i = 0; i < 32; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[offset + i]);
        hash_hex += buf;
    }
    metadata.tip.hash = uint256(hash_hex);
    offset += 32;

    // Best block height (4 bytes)
    metadata.tip.height = (static_cast<int>(data[offset]) << 24) |
                          (static_cast<int>(data[offset + 1]) << 16) |
                          (static_cast<int>(data[offset + 2]) << 8) |
                          static_cast<int>(data[offset + 3]);
    offset += 4;

    // Chainwork (32 bytes)
    std::string work_hex;
    for (size_t i = 0; i < 32; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[offset + i]);
        work_hex += buf;
    }
    metadata.tip.work = arith_uint256(work_hex);
    offset += 32;

    // Best block timestamp (4 bytes)
    metadata.tip.timestamp = (static_cast<uint32_t>(data[offset]) << 24) |
                             (static_cast<uint32_t>(data[offset + 1]) << 16) |
                             (static_cast<uint32_t>(data[offset + 2]) << 8) |
                             static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    // IBD state (1 byte)
    metadata.is_ibd = (data[offset] != 0);
    offset += 1;

    // Last flush timestamp (8 bytes)
    metadata.last_flush_time = (static_cast<uint64_t>(data[offset]) << 56) |
                                (static_cast<uint64_t>(data[offset + 1]) << 48) |
                                (static_cast<uint64_t>(data[offset + 2]) << 40) |
                                (static_cast<uint64_t>(data[offset + 3]) << 32) |
                                (static_cast<uint64_t>(data[offset + 4]) << 24) |
                                (static_cast<uint64_t>(data[offset + 5]) << 16) |
                                (static_cast<uint64_t>(data[offset + 6]) << 8) |
                                static_cast<uint64_t>(data[offset + 7]);
    offset += 8;

    // Checksum (4 bytes)
    uint32_t stored_checksum = (static_cast<uint32_t>(data[offset]) << 24) |
                                (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                static_cast<uint32_t>(data[offset + 3]);

    // Verify checksum (excluding the checksum itself)
    uint32_t calculated_checksum = calculateChecksum(data.data(), offset);
    if (stored_checksum != calculated_checksum) {
        return Status::fromError("Checksum mismatch: stored=0x" + std::to_string(stored_checksum) +
                                 ", calculated=0x" + std::to_string(calculated_checksum));
    }

    LOG_INFO("📂 Chainstate metadata loaded: height=" + std::to_string(metadata.tip.height) +
             ", hash=" + metadata.tip.hash.ToString().substr(0, 16) + "..." +
             ", ibd=" + (metadata.is_ibd ? "true" : "false"));

    return metadata;
}

uint32_t ChainstateMetadata::calculateChecksum(const uint8_t* data, size_t size) const {
    // Simple CRC32 (can be improved with hardware-accelerated CRC later)
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

} // namespace dinero
