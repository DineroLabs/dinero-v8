#pragma once

#include "primitives/block.h"
#include "common/status.h"
#include "storage/tip_info.h"
#include "dinero/core/consensus/commitment.h"  // For uint256
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <cstdint>
#include <sstream>
#include <unordered_map>

namespace dinero {

// File position metadata for locating blocks in flat files
// Mirrors Bitcoin Core's CDiskBlockPos
struct FilePosition {
    uint32_t file_number = 0;  // blk00000.dat, blk00001.dat, etc.
    uint64_t offset = 0;         // Byte offset within the file
    uint32_t size = 0;           // Size of the serialized block

    FilePosition() = default;
    FilePosition(uint32_t f, uint64_t o, uint32_t s)
        : file_number(f), offset(o), size(s) {}

    bool operator==(const FilePosition& other) const {
        return file_number == other.file_number &&
               offset == other.offset &&
               size == other.size;
    }

    bool isNull() const {
        return file_number == 0 && offset == 0 && size == 0;
    }
};

// Bitcoin-style flat file block storage
//
// Architecture:
//   blocks/blk00000.dat - Raw block bodies (append-only)
//   blocks/blk00001.dat - ...
//   blocks/index/       - RocksDB with hash → FilePosition mapping
//
// File format (per block):  // Phase E.1.d: Added checksum for corruption detection
//   [4 bytes] Magic bytes (Dinero network magic from chainparams)
//   [4 bytes] Block size (little-endian uint32_t)
//   [N bytes] Serialized block data
//   [4 bytes] Checksum (FNV-1a hash of block data)
//
// Benefits vs RocksDB storage:
//   - Massive scalability (Bitcoin Core handles 500GB+ block data)
//   - Sequential I/O for block writes (append-only)
//   - Required for future pruning support
//   - Lower memory footprint (no large RocksDB values)
//
class BlockStorage {
public:
    BlockStorage() = default;
    ~BlockStorage() { close(); }

    // Disable copy, enable move
    BlockStorage(const BlockStorage&) = delete;
    BlockStorage& operator=(const BlockStorage&) = delete;
    BlockStorage(BlockStorage&& other) noexcept;
    BlockStorage& operator=(BlockStorage&& other) noexcept;

    // Initialization
    //
    // Creates blocks/ directory structure:
    //   blocks/blk00000.dat (initial file)
    //   blocks/index/       (RocksDB for FilePosition index)
    Status init(const std::filesystem::path& datadir);
    void close();

    // Write a block to flat file storage
    //
    // Returns FilePosition where block was written.
    // Thread-safe: Multiple threads can write concurrently.
    // Blocks are written atomically with proper file rotation.
    StatusOr<FilePosition> writeBlock(const uint256& hash, const Block& block);

    // Read a block from flat file storage
    //
    // Requires FilePosition from ChainDB block index.
    // Returns deserialized Block object.
    // Thread-safe: Multiple threads can read concurrently.
    StatusOr<Block> readBlock(const FilePosition& pos) const;

    // Check if a block exists in flat file storage
    Status hasBlock(const FilePosition& pos) const;

    // Write undo data to rev*.dat file
    //
    // Format: [size:4][data:N][checksum:4]
    // Returns FilePosition where undo was written.
    // Must be called BEFORE marking block as CONNECTED.
    // Thread-safe: Multiple threads can write concurrently.
    StatusOr<FilePosition> writeUndo(const uint256& hash, const std::vector<uint8_t>& undo_data);

    // Read undo data from rev*.dat file
    //
    // Requires FilePosition from block index.
    // Returns raw undo data after checksum verification.
    // Failure is FATAL - indicates database corruption.
    // Thread-safe: Multiple threads can read concurrently.
    StatusOr<std::vector<uint8_t>> readUndo(const FilePosition& pos) const;

    // Get current file statistics
    struct StorageStats {
        uint32_t current_file_number = 0;
        uint64_t current_file_size = 0;
        uint64_t total_blocks_written = 0;
        uint64_t total_bytes_written = 0;
    };
    StatusOr<StorageStats> getStats() const;

    // Flush current file to disk
    Status flush();

    // ========================================================================
    // F.7.2: Pruning Gate (NOT IMPLEMENTED - Invariants Enforced Only)
    // ========================================================================
    //
    // This function is a GATE for future pruning implementation.
    // It enforces pruning invariants with hard asserts.
    // Actual pruning (file deletion) is NOT implemented.
    //
    // Future pruning code MUST call this function before attempting to prune
    // any block. This function will assert-fail if pruning invariants are violated.
    //
    // Invariants enforced:
    //   - Block must be prunable according to BlockIndex::isPrunable()
    //   - Block must have BLOCK_HAVE_UNDO flag
    //   - Undo data must be present on disk
    //
    // Returns:
    //   Status::Ok if invariants are satisfied (but NO deletion occurs)
    //   Status::InvalidArgument if block cannot be pruned
    //
    Status checkPruningInvariants(const class BlockIndex* pindex) const;

    // ========================================================================
    // Phase P.2: Physical Block Deletion
    // ========================================================================
    //
    // These functions implement actual block data deletion for pruning.
    // They MUST only be called after checkPruningInvariants() passes.
    //
    // Bitcoin Core approach: Zero-out data in place (don't truncate files).
    // Benefits:
    //   - Avoids file fragmentation
    //   - Preserves file structure for forensics
    //   - Simpler than compaction (can defer to Phase P.3)
    //   - Battle-tested in production
    //
    // Safety:
    //   - All functions are idempotent (safe to call multiple times)
    //   - Flags cleared atomically before return
    //   - Thread-safe (uses existing mutexes)
    //

    // Delete block and undo data for a pruned block
    //
    // This is the main entry point for pruning a single block.
    // It:
    //   1. Checks pruning invariants (via checkPruningInvariants)
    //   2. Zeros out undo data in rev*.dat
    //   3. Zeros out block data in blk*.dat
    //   4. Clears BLOCK_HAVE_DATA and BLOCK_HAVE_UNDO flags
    //   5. Updates storage statistics
    //
    // Returns:
    //   Status::Ok on success
    //   Status::InvalidArgument if pruning invariants fail
    //   Status::Io on file I/O error
    //
    // Thread-safe: Uses write_mutex for statistics update
    Status pruneBlockData(class BlockIndex* pindex);

    // Delete undo data for a block
    //
    // Zeros out undo data in rev*.dat file and clears BLOCK_HAVE_UNDO flag.
    // Called internally by pruneBlockData(), but can be called separately.
    //
    // Returns:
    //   Status::Ok on success
    //   Status::InvalidArgument if undo position invalid
    //   Status::Io on file I/O error
    Status pruneUndoData(class BlockIndex* pindex);

    // ========================================================================
    // Phase 34.8: File-Level Pruning
    // ========================================================================
    //
    // Efficient pruning by deleting whole blk*.dat/rev*.dat files when all
    // blocks in them are below the prune height.
    //

    // Information about a block file for pruning decisions
    struct FileInfo {
        uint32_t file_number = 0;
        uint32_t lowest_height = UINT32_MAX;   // Lowest block height in this file
        uint32_t highest_height = 0;            // Highest block height in this file
        uint32_t block_count = 0;               // Number of blocks in this file
        uint64_t file_size = 0;                 // File size in bytes
        bool can_delete = false;                // True if all blocks below prune height
    };

    // Get information about all block files up to a certain file number
    // Returns vector of FileInfo sorted by file_number
    std::vector<FileInfo> getFileInfo() const;

    // Delete entire block and undo files for the given file number
    // ONLY call when FileInfo::can_delete is true!
    // Returns bytes freed
    StatusOr<uint64_t> deleteFile(uint32_t file_number);

    // Zero out a region in a block/undo file (for boundary blocks)
    // This is exposed for PruneService to handle edge cases
    Status zeroRegion(uint32_t file_number, uint64_t offset, uint32_t size, bool is_undo);

    // Get the path to a block file (for stats/debugging)
    std::filesystem::path getBlockFilePath(uint32_t file_number) const { return getFilePath(file_number); }
    std::filesystem::path getUndoFilePathPublic(uint32_t file_number) const { return getUndoFilePath(file_number); }

    // Configuration constants
    static constexpr uint64_t MAX_FILE_SIZE = 128 * 1024 * 1024;  // 128 MB per file (Bitcoin uses 128MB)

    // Per-chain MAGIC_* constants used to live here. They were a
    // duplicate of the canonical values in src/consensus/chainparams_impl.cpp
    // and were used by P2P header validation, which is a layering violation —
    // block storage shouldn't be the source of P2P magic. BlockStorage::
    // getMagicBytes() already reads dinero::Params().magic at runtime;
    // call that if you need the magic for the active chain.

private:
    // Internal helpers
    Status openFile(uint32_t file_number);
    Status rotateFile();
    std::filesystem::path getFilePath(uint32_t file_number) const;
    uint32_t getMagicBytes() const;

    // Undo file helpers
    Status openUndoFile(uint32_t file_number);
    Status rotateUndoFile();
    std::filesystem::path getUndoFilePath(uint32_t file_number) const;
    uint32_t calculateChecksum(const std::vector<uint8_t>& data) const;
    uint32_t calculateChecksum(const std::string& data) const;  // Phase E.1.d: Block checksum

    // Phase P.2: Pruning helper
    Status zeroOutFileRegion(const std::filesystem::path& file_path, uint64_t offset, uint32_t size);

    // State
    std::filesystem::path blocks_dir_;
    mutable std::mutex write_mutex_;  // Protects write operations
    mutable std::mutex read_mutex_;   // Protects file handle map
    mutable std::mutex undo_write_mutex_;  // Protects undo write operations

    // Current write file (blocks)
    std::unique_ptr<std::ofstream> current_write_file_;
    uint32_t current_file_number_ = 0;
    uint64_t current_file_size_ = 0;

    // Current write file (undo)
    std::unique_ptr<std::ofstream> current_undo_file_;
    uint32_t current_undo_file_number_ = 0;
    uint64_t current_undo_file_size_ = 0;

    // Statistics
    uint64_t total_blocks_written_ = 0;
    uint64_t total_bytes_written_ = 0;

    // Read file handles (cached for performance)
    // Map: file_number → ifstream
    mutable std::unordered_map<uint32_t, std::shared_ptr<std::ifstream>> read_files_;
    mutable std::unordered_map<uint32_t, std::shared_ptr<std::ifstream>> read_undo_files_;
};

} // namespace dinero
