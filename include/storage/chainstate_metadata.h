#pragma once

#include "storage/tip_info.h"
#include "common/status.h"
#include <string>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dinero {

/**
 * F.11.12: Chainstate Metadata Persistence
 *
 * Persists critical chainstate information for fast restart.
 * Avoids full revalidation after normal shutdown.
 *
 * Stored in: <datadir>/chainstate/metadata.dat
 *
 * Format (fixed-size record for fast read/write):
 *   [4 bytes] Magic (0xD1CECAFE - "Dinero Chainstate")
 *   [4 bytes] Version (1)
 *   [32 bytes] Best block hash
 *   [4 bytes] Best block height
 *   [32 bytes] Chainwork (arith_uint256)
 *   [4 bytes] Best block timestamp
 *   [1 byte] IBD state (0 = complete, 1 = in progress)
 *   [8 bytes] Last flush timestamp
 *   [4 bytes] Checksum (CRC32)
 *
 * Total: 91 bytes (fixed size for easy updates)
 */

class ChainstateMetadata {
public:
    static constexpr uint32_t MAGIC = 0xD1CECAFE;  // "Dinero Chainstate"
    static constexpr uint32_t VERSION = 1;

    /**
     * Chainstate metadata record
     */
    struct Metadata {
        // Best block (tip) information
        TipInfo tip;

        // IBD state
        bool is_ibd = true;  // True = still syncing, False = fully synced

        // Last flush timestamp (for detecting crashes)
        uint64_t last_flush_time = 0;

        Metadata() = default;
    };

    /**
     * Constructor
     *
     * @param datadir  Data directory containing chainstate/
     */
    explicit ChainstateMetadata(const std::filesystem::path& datadir);

    ~ChainstateMetadata();

    /**
     * Load metadata from disk
     *
     * Returns Ok if metadata exists and is valid.
     * Returns NotFound if metadata doesn't exist (first run).
     * Returns InvalidArgument if metadata is corrupted.
     */
    StatusOr<Metadata> load();

    /**
     * Save metadata to disk
     *
     * Writes metadata atomically with checksum.
     * Safe to call frequently (small fixed-size record).
     */
    Status save(const Metadata& metadata);

    /**
     * Delete metadata (for reindex)
     *
     * Forces full validation on next startup.
     */
    Status remove();

    /**
     * Check if metadata exists
     */
    bool exists() const;

private:
    std::filesystem::path datadir_;
    std::filesystem::path metadata_path_;

    // Helper functions
    std::vector<uint8_t> serialize(const Metadata& metadata) const;
    StatusOr<Metadata> deserialize(const std::vector<uint8_t>& data) const;
    uint32_t calculateChecksum(const uint8_t* data, size_t size) const;
};

} // namespace dinero
