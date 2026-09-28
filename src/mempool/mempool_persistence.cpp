#include "mempool/mempool_persistence.h"
#include "daemon/mempool.h"
#include "wallet/transaction.h"
#include "common/logger.h"
#include <fstream>
#include <filesystem>
#include <cstring>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Serialization Helpers (Bitcoin Core style - little endian)
// ═══════════════════════════════════════════════════════════════════════════

void MempoolPersistence::writeUint32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

void MempoolPersistence::writeUint64(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
    out.push_back((value >> 32) & 0xff);
    out.push_back((value >> 40) & 0xff);
    out.push_back((value >> 48) & 0xff);
    out.push_back((value >> 56) & 0xff);
}

void MempoolPersistence::writeVarint(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0xfd) {
        out.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(value & 0xff);
        out.push_back((value >> 8) & 0xff);
    } else if (value <= 0xffffffff) {
        out.push_back(0xfe);
        writeUint32(out, static_cast<uint32_t>(value));
    } else {
        out.push_back(0xff);
        writeUint64(out, value);
    }
}

void MempoolPersistence::writeBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    writeVarint(out, data.size());
    out.insert(out.end(), data.begin(), data.end());
}

uint32_t MempoolPersistence::readUint32(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset > data.size() || data.size() - offset < 4) {
        throw std::runtime_error("readUint32: insufficient data");
    }
    uint32_t value = data[offset] |
                     (data[offset + 1] << 8) |
                     (data[offset + 2] << 16) |
                     (data[offset + 3] << 24);
    offset += 4;
    return value;
}

uint64_t MempoolPersistence::readUint64(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset > data.size() || data.size() - offset < 8) {
        throw std::runtime_error("readUint64: insufficient data");
    }
    uint64_t value = static_cast<uint64_t>(data[offset]) |
                     (static_cast<uint64_t>(data[offset + 1]) << 8) |
                     (static_cast<uint64_t>(data[offset + 2]) << 16) |
                     (static_cast<uint64_t>(data[offset + 3]) << 24) |
                     (static_cast<uint64_t>(data[offset + 4]) << 32) |
                     (static_cast<uint64_t>(data[offset + 5]) << 40) |
                     (static_cast<uint64_t>(data[offset + 6]) << 48) |
                     (static_cast<uint64_t>(data[offset + 7]) << 56);
    offset += 8;
    return value;
}

uint64_t MempoolPersistence::readVarint(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("readVarint: insufficient data");
    }

    uint8_t first = data[offset++];
    if (first < 0xfd) {
        return first;
    } else if (first == 0xfd) {
        if (offset > data.size() || data.size() - offset < 2) {
            throw std::runtime_error("readVarint: insufficient data for 0xfd");
        }
        uint64_t value = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        return value;
    } else if (first == 0xfe) {
        return readUint32(data, offset);
    } else { // 0xff
        return readUint64(data, offset);
    }
}

std::vector<uint8_t> MempoolPersistence::readBytes(const std::vector<uint8_t>& data, size_t& offset) {
    uint64_t size = readVarint(data, offset);
    if (offset > data.size() || size > data.size() - offset) {
        throw std::runtime_error("readBytes: insufficient data");
    }

    std::vector<uint8_t> result(data.begin() + offset, data.begin() + offset + size);
    offset += size;
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Save Mempool (STEP B - Hooked into daemon shutdown)
// ═══════════════════════════════════════════════════════════════════════════

bool MempoolPersistence::save(
    const std::vector<MempoolEntry>& entries,
    const std::string& filepath
) {
    try {
        g_logger.info("Saving mempool to " + filepath);

        if (entries.size() > MAX_PERSISTED_TXS) return false;
        std::vector<uint8_t> data;

        // Magic bytes (8 bytes): "MEMPOOLV"
        for (size_t i = 0; i < 8; ++i) {
            data.push_back(MAGIC[i]);
        }

        // Version (4 bytes): 1
        writeUint32(data, VERSION);

        // Transaction count (varint)
        writeVarint(data, entries.size());

        // For each transaction
        for (const auto& entry : entries) {
            // Serialize transaction to canonical wire bytes (proven in v0.13.0.1)
            std::vector<uint8_t> tx_bytes = entry.tx.Serialize();
            // Nine length bytes plus the existing twenty metadata bytes.
            if (tx_bytes.size() > MAX_PERSISTED_FILE_BYTES - 29 ||
                data.size() > MAX_PERSISTED_FILE_BYTES - 29 - tx_bytes.size())
                return false;

            // Tx bytes length + tx bytes
            writeBytes(data, tx_bytes);

            // Preserve the existing opaque steady-clock field; not recovery authority.
            auto arrival_time_unix = std::chrono::duration_cast<std::chrono::seconds>(
                entry.time.time_since_epoch()
            ).count();
            writeUint64(data, arrival_time_unix);

            // Fee
            writeUint64(data, entry.fee);

            // Height
            writeUint32(data, entry.height);
        }

        // Atomic write (.tmp → rename)
        if (!atomicWrite(filepath, data)) {
            g_logger.error("Failed to write mempool file: " + filepath);
            return false;
        }

        g_logger.info("Mempool saved: " + std::to_string(entries.size()) + " transactions, " +
                     std::to_string(data.size()) + " bytes");
        return true;

    } catch (const std::exception& e) {
        // Never throw - best effort only
        g_logger.error("Exception saving mempool: " + std::string(e.what()));
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Load Mempool (STEP C - Deserialized and revalidated on startup)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<MempoolPersistence::PersistedEntry> MempoolPersistence::load(const std::string& filepath) {
    std::error_code error;
    const bool exists = std::filesystem::exists(filepath, error);
    if (error) throw std::runtime_error("Mempool file availability unknown");
    if (!exists) throw MissingFile();
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) throw std::runtime_error("Mempool file unavailable");
    const auto end = file.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > MAX_PERSISTED_FILE_BYTES)
        throw std::runtime_error("Mempool file exceeds read budget");
    const auto size = static_cast<size_t>(end);
    std::vector<uint8_t> data(size);
    file.seekg(0, std::ios::beg);
    if (!file || !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)) ||
        file.gcount() != static_cast<std::streamsize>(size) ||
        file.peek() != std::char_traits<char>::eof() || file.bad())
        throw std::runtime_error("Incomplete mempool file read");
    if (data.size() < 8 || std::memcmp(data.data(), MAGIC, 8) != 0)
        throw std::runtime_error("Invalid mempool file magic");
    size_t offset = 8;
    if (readUint32(data, offset) != VERSION)
        throw std::runtime_error("Unsupported mempool file version");
    const auto count = readVarint(data, offset);
    if (count > MAX_PERSISTED_TXS)
        throw std::runtime_error("Mempool file transaction count exceeds budget");
    std::vector<PersistedEntry> entries;
    entries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        PersistedEntry entry;
        entry.tx_bytes = readBytes(data, offset);
        entry.arrival_time = readUint64(data, offset);
        entry.fee = readUint64(data, offset);
        entry.height = readUint32(data, offset);
        entries.push_back(std::move(entry));
    }
    if (offset != data.size()) throw std::runtime_error("Trailing mempool file bytes");
    return entries;
}

// ═══════════════════════════════════════════════════════════════════════════
// Atomic Write (Bitcoin Core pattern: write to .tmp, then rename)
// ═══════════════════════════════════════════════════════════════════════════

bool MempoolPersistence::atomicWrite(const std::string& filepath, const std::vector<uint8_t>& data) {
    try {
        std::string tmp_path = filepath + ".tmp";

        // Write to temporary file
        std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }

        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.flush();
        file.close();

        if (!file.good()) {
            std::filesystem::remove(tmp_path);
            return false;
        }

        // Atomic rename (.tmp → final)
        std::filesystem::rename(tmp_path, filepath);

        return true;

    } catch (const std::exception& e) {
        g_logger.error("Atomic write failed: " + std::string(e.what()));
        return false;
    }
}

} // namespace dinero
