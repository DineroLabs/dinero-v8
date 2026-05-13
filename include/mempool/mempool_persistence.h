#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace dinero {

// Forward declarations
struct Transaction;
struct MempoolEntry;

/**
 * @brief Mempool Persistence Format (v0.13.0.2 - Step A)
 *
 * Design Constraints:
 * - Boring, conservative, minimal
 * - Use canonical tx wire bytes (proven in v0.13.0.1)
 * - Simple stream format (length-prefixed blobs)
 * - Do NOT invent complex format
 *
 * File Format:
 * ┌─────────────────────────────────────┐
 * │ Magic (8 bytes): "MEMPOOLV"         │
 * │ Version (4 bytes): 1                │
 * │ Tx Count (varint)                   │
 * ├─────────────────────────────────────┤
 * │ For each transaction:               │
 * │   - Tx bytes length (varint)        │
 * │   - Tx wire bytes (canonical)       │
 * │   - Arrival time (uint64_t)         │
 * │   - Fee (uint64_t)                  │
 * │   - Height (uint32_t)               │
 * └─────────────────────────────────────┘
 *
 * Bitcoin Core lesson: disk format ≠ network format, but tx bytes remain canonical
 */

class MempoolPersistence {
public:
    // ═══════════════════════════════════════════════════════════════════════
    // Serialization Format Constants
    // ═══════════════════════════════════════════════════════════════════════

    static constexpr uint8_t MAGIC[8] = {'M', 'E', 'M', 'P', 'O', 'O', 'L', 'V'};
    static constexpr uint32_t VERSION = 1;
    static constexpr size_t MAX_PERSISTED_TXS = 100000;  // Sanity limit

    // ═══════════════════════════════════════════════════════════════════════
    // Persisted Transaction Entry
    // ═══════════════════════════════════════════════════════════════════════

    struct PersistedEntry {
        std::vector<uint8_t> tx_bytes;  // Canonical wire bytes (proven in v0.13.0.1)
        uint64_t arrival_time;          // Unix timestamp (seconds since epoch)
        uint64_t fee;                   // Transaction fee in una
        uint32_t height;                // Block height when added

        PersistedEntry() : arrival_time(0), fee(0), height(0) {}
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Save / Load Interface
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Save mempool entries to disk (atomic write via .tmp rename)
     *
     * Rules:
     * - Never throw (best effort only)
     * - Never block shutdown forever
     * - Log success/failure
     *
     * @param entries Mempool entries to persist
     * @param filepath Path to save file (e.g., /datadir/mempool.dat)
     * @return true if saved successfully, false otherwise
     */
    static bool save(
        const std::vector<MempoolEntry>& entries,
        const std::string& filepath
    );

    /**
     * Load mempool entries from disk
     *
     * Rules:
     * - Returns empty vector on any failure (corrupt file, missing, etc.)
     * - Never throws
     * - Caller must validate each entry against current policy
     *
     * @param filepath Path to load file
     * @return Vector of persisted entries (may be empty)
     */
    static std::vector<PersistedEntry> load(const std::string& filepath);

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Serialization Helpers (Internal)
    // ═══════════════════════════════════════════════════════════════════════

    static void writeUint32(std::vector<uint8_t>& out, uint32_t value);
    static void writeUint64(std::vector<uint8_t>& out, uint64_t value);
    static void writeVarint(std::vector<uint8_t>& out, uint64_t value);
    static void writeBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data);

    static uint32_t readUint32(const std::vector<uint8_t>& data, size_t& offset);
    static uint64_t readUint64(const std::vector<uint8_t>& data, size_t& offset);
    static uint64_t readVarint(const std::vector<uint8_t>& data, size_t& offset);
    static std::vector<uint8_t> readBytes(const std::vector<uint8_t>& data, size_t& offset);

    // ═══════════════════════════════════════════════════════════════════════
    // Atomic Write Helper
    // ═══════════════════════════════════════════════════════════════════════

    static bool atomicWrite(const std::string& filepath, const std::vector<uint8_t>& data);
};

} // namespace dinero
