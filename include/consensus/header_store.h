#pragma once

/**
 * Phase N.1: Header Storage + Restart Safety
 *
 * This file defines persistent storage for header-only sync.
 *
 * Key Invariants (Phase N.1):
 * - Headers persist separately from full blocks
 * - Best tip survives restart
 * - Competing forks survive restart
 * - No bodies stored with headers
 * - No UTXO state in header storage
 * - Fast loading (headers only, no block deserialization)
 *
 * Storage Layout:
 * - Namespace: "headers/"
 * - Key: block hash (32 bytes, uint256)
 * - Value: serialized HeaderEntry
 * - Special key: "headers/best" → best header hash
 */

#include "consensus/header_chain.h"
#include "primitives/uint256.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>

// Forward declare RocksDB types
namespace rocksdb {
    class DB;
    class ColumnFamilyHandle;
}

namespace dinero {
namespace consensus {

/**
 * @brief Persistent storage for headers
 *
 * Phase N.1: Provides durable storage for header chain that survives restarts.
 *
 * Responsibilities:
 * - Persist headers to disk
 * - Load headers on startup
 * - Maintain best header tip marker
 * - Support fork storage
 *
 * Explicitly NOT responsible for:
 * ❌ Storing full block bodies
 * ❌ Storing UTXO state
 * ❌ Transaction validation
 * ❌ Mempool interaction
 */
class HeaderStore {
public:
    struct SchemaMetadata {
        uint32_t version{0};
        std::string network;
        uint32_t header_size{0};
    };

    /**
     * @brief Construct header store with database path
     *
     * @param db_path Path to RocksDB database
     */
    explicit HeaderStore(const std::string& db_path);
    ~HeaderStore();

    /**
     * @brief Open/create the header database
     *
     * @return true if successful
     */
    bool Open();

    /**
     * @brief Close the database
     */
    void Close();

    /**
     * @brief Store a header
     *
     * Phase N.1: Persists header for restart safety.
     *
     * @param entry Header entry to store
     * @return true if successful
     */
    bool StoreHeader(const HeaderIndexEntry& entry);

    /**
     * @brief Load a header by hash
     *
     * @param hash Block hash
     * @param entry Output parameter for loaded header
     * @return true if found and loaded
     */
    bool LoadHeader(const uint256& hash, HeaderIndexEntry& entry);

    /**
     * @brief Store the best header tip marker
     *
     * @param hash Hash of best header
     * @return true if successful
     */
    bool StoreBestHeader(const uint256& hash);

    /**
     * @brief Load the best header tip marker
     *
     * @param hash Output parameter for best header hash
     * @return true if found
     */
    bool LoadBestHeader(uint256& hash);

    /**
     * @brief Delete the persisted best-header tip marker (for testing)
     *
     * @return true if successful
     */
    bool DeleteBestHeader();

    /**
     * @brief Load all headers from storage
     *
     * Phase N.1: Restart safety - loads entire header tree.
     *
     * @param headers Output vector of all headers
     * @return true if successful
     */
    bool LoadAllHeaders(std::vector<HeaderIndexEntry>& headers);

    /**
     * @brief Get header count in storage
     */
    size_t GetHeaderCount() const;

    /**
     * @brief Delete a header (for testing)
     *
     * @param hash Block hash to delete
     * @return true if successful
     */
    bool DeleteHeader(const uint256& hash);

    /**
     * @brief Clear all headers (for testing)
     *
     * @return true if successful
     */
    bool ClearAll();

    /**
     * @brief True when legacy/truncated header entries were observed on load.
     *
     * Legacy entries predate the frozen 128-byte BlockHeader v1 persistence
     * format. Callers can use this to trigger a local store reset/reseed.
     */
    bool HasLegacyEntries() const { return has_legacy_entries_; }

    /**
     * @brief True when explicit schema metadata exists on disk.
     */
    bool HasSchemaMetadata() const { return persisted_schema_metadata_.has_value(); }

    /**
     * @brief True when the on-disk header store should be quarantined/rebuilt.
     */
    bool NeedsSchemaRecovery() const { return schema_recovery_required_; }

    /**
     * @brief True when explicit schema metadata matches the current runtime.
     */
    bool IsSchemaCompatible() const {
        return persisted_schema_metadata_.has_value() && !schema_recovery_required_;
    }

    /**
     * @brief Human-readable explanation for schema recovery decisions.
     */
    const std::string& GetSchemaRecoveryReason() const { return schema_recovery_reason_; }

    /**
     * @brief Current runtime schema expectation.
     */
    const SchemaMetadata& GetExpectedSchemaMetadata() const { return expected_schema_metadata_; }

    /**
     * @brief Explicit schema metadata read from disk, if any.
     */
    std::optional<SchemaMetadata> GetPersistedSchemaMetadata() const {
        return persisted_schema_metadata_;
    }

private:
    std::string db_path_;
    rocksdb::DB* db_;
    bool is_open_;
    bool has_legacy_entries_{false};
    bool schema_recovery_required_{false};
    std::string schema_recovery_reason_;
    SchemaMetadata expected_schema_metadata_;
    std::optional<SchemaMetadata> persisted_schema_metadata_;

    // Serialization helpers
    std::vector<uint8_t> SerializeHeader(const HeaderIndexEntry& entry);
    bool DeserializeHeader(const std::vector<uint8_t>& data, HeaderIndexEntry& entry);

    // Schema metadata helpers
    bool WriteSchemaMetadata(const SchemaMetadata& metadata);
    bool LoadSchemaMetadata(SchemaMetadata& metadata) const;
    bool HasPersistedHeaderRecords() const;
    bool HasPersistedBestMarker() const;
    bool HasPersistedSchemaKey() const;
    void SetSchemaRecoveryRequired(const std::string& reason);

    // Key helpers
    std::string MakeHeaderKey(const uint256& hash) const;
    std::string MakeBestHeaderKey() const;
    std::string MakeSchemaMetadataKey() const;
};

} // namespace consensus
} // namespace dinero
