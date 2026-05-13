#include "storage/block_storage.h"
#include "storage/block_index.h"  // F.7.2: For pruning invariants
#include "storage/disk_space_monitor.h"  // Phase E.2.b: Disk space checking
#include "consensus/chainparams.h"
#include <iostream>
#include <cstring>

// Phase E.1.b: fsync() for crash safety
#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
    #define FSYNC(fd) _commit(fd)
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>
    #define FSYNC(fd) fsync(fd)
#endif

namespace dinero {

// ============================================================================
// Phase E.1.b: Crash Safety Helpers
// ============================================================================

/**
 * Fsync a file by path to ensure data is written to disk
 *
 * std::ofstream::flush() only flushes to OS buffers, not disk.
 * On power failure, data can be lost. We MUST fsync after critical writes.
 *
 * Critical writes:
 * - Block data (blk*.dat) - blocks are immutable, losing them = redownload
 * - Undo data (rev*.dat) - losing undo = cannot reorg safely
 *
 * This function flushes the stream first, then opens the file with
 * native OS calls to perform fsync.
 *
 * @param stream  Output stream to flush first
 * @param path    Path to file to fsync
 * @return        Status::Ok on success, Status::Io on failure
 */
static Status fsyncFile(std::ofstream& stream, const std::filesystem::path& path) {
    // First flush C++ stream to OS buffers
    if (stream.is_open()) {
        stream.flush();
        if (!stream.good()) {
            std::cerr << "[BlockStorage] Stream flush failed before fsync" << std::endl;
            return Status::Io;
        }
    }

#ifdef _WIN32
    // Windows: Open file handle and FlushFileBuffers
    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (handle == INVALID_HANDLE_VALUE) {
        std::cerr << "[BlockStorage] Failed to open file for fsync: " << path.string() << std::endl;
        return Status::Io;
    }

    if (!FlushFileBuffers(handle)) {
        std::cerr << "[BlockStorage] FlushFileBuffers failed (Windows fsync): " << path.string() << std::endl;
        CloseHandle(handle);
        return Status::Io;
    }

    CloseHandle(handle);
#else
    // POSIX: Open file and fsync
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        std::cerr << "[BlockStorage] Failed to open file for fsync: " << path.string()
                  << " (" << strerror(errno) << ")" << std::endl;
        return Status::Io;
    }

    if (FSYNC(fd) != 0) {
        std::cerr << "[BlockStorage] fsync() failed: " << path.string()
                  << " (" << strerror(errno) << ")" << std::endl;
        close(fd);
        return Status::Io;
    }

    close(fd);
#endif

    return Status::Ok;
}

// ============================================================================
// Move Operations
// ============================================================================

BlockStorage::BlockStorage(BlockStorage&& other) noexcept {
    std::lock_guard<std::mutex> lock_write(other.write_mutex_);
    std::lock_guard<std::mutex> lock_read(other.read_mutex_);
    std::lock_guard<std::mutex> lock_undo(other.undo_write_mutex_);

    blocks_dir_ = std::move(other.blocks_dir_);
    current_write_file_ = std::move(other.current_write_file_);
    current_file_number_ = other.current_file_number_;
    current_file_size_ = other.current_file_size_;
    current_undo_file_ = std::move(other.current_undo_file_);
    current_undo_file_number_ = other.current_undo_file_number_;
    current_undo_file_size_ = other.current_undo_file_size_;
    total_blocks_written_ = other.total_blocks_written_;
    total_bytes_written_ = other.total_bytes_written_;
    read_files_ = std::move(other.read_files_);
    read_undo_files_ = std::move(other.read_undo_files_);
}

BlockStorage& BlockStorage::operator=(BlockStorage&& other) noexcept {
    if (this != &other) {
        close();

        std::lock_guard<std::mutex> lock_write(other.write_mutex_);
        std::lock_guard<std::mutex> lock_read(other.read_mutex_);
        std::lock_guard<std::mutex> lock_undo(other.undo_write_mutex_);

        blocks_dir_ = std::move(other.blocks_dir_);
        current_write_file_ = std::move(other.current_write_file_);
        current_file_number_ = other.current_file_number_;
        current_file_size_ = other.current_file_size_;
        current_undo_file_ = std::move(other.current_undo_file_);
        current_undo_file_number_ = other.current_undo_file_number_;
        current_undo_file_size_ = other.current_undo_file_size_;
        total_blocks_written_ = other.total_blocks_written_;
        total_bytes_written_ = other.total_bytes_written_;
        read_files_ = std::move(other.read_files_);
        read_undo_files_ = std::move(other.read_undo_files_);
    }
    return *this;
}

// ============================================================================
// Initialization
// ============================================================================

Status BlockStorage::init(const std::filesystem::path& datadir) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    blocks_dir_ = datadir / "blocks";

    // Create blocks directory if it doesn't exist
    if (!std::filesystem::exists(blocks_dir_)) {
        std::error_code ec;
        if (!std::filesystem::create_directories(blocks_dir_, ec)) {
            std::cerr << "[BlockStorage] Failed to create blocks directory: " << ec.message() << std::endl;
            return Status::Io;
        }
    }

    // Find the highest numbered block file and undo file
    current_file_number_ = 0;
    current_undo_file_number_ = 0;
    for (const auto& entry : std::filesystem::directory_iterator(blocks_dir_)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.substr(0, 3) == "blk" && filename.substr(8, 4) == ".dat") {
                try {
                    uint32_t file_num = std::stoul(filename.substr(3, 5));
                    if (file_num > current_file_number_) {
                        current_file_number_ = file_num;
                    }
                } catch (...) {
                    // Skip invalid filenames
                }
            } else if (filename.substr(0, 3) == "rev" && filename.substr(8, 4) == ".dat") {
                try {
                    uint32_t file_num = std::stoul(filename.substr(3, 5));
                    if (file_num > current_undo_file_number_) {
                        current_undo_file_number_ = file_num;
                    }
                } catch (...) {
                    // Skip invalid filenames
                }
            }
        }
    }

    // Open the current file for appending
    auto status = openFile(current_file_number_);
    if (status != Status::Ok) {
        return status;
    }

    std::cout << "[BlockStorage] Initialized with " << current_file_number_ + 1
              << " block files (current: blk"
              << std::setfill('0') << std::setw(5) << current_file_number_
              << ".dat)" << std::endl;

    return Status::Ok;
}

void BlockStorage::close() {
    std::lock_guard<std::mutex> lock_write(write_mutex_);
    std::lock_guard<std::mutex> lock_read(read_mutex_);
    std::lock_guard<std::mutex> lock_undo(undo_write_mutex_);

    // Phase E.1.b: Fsync block file before closing (ensure durability on shutdown)
    if (current_write_file_ && current_write_file_->is_open()) {
        auto status = fsyncFile(*current_write_file_, getFilePath(current_file_number_));
        if (status != Status::Ok) {
            std::cerr << "[BlockStorage] WARNING: Failed to fsync block file on close" << std::endl;
        }
        current_write_file_->close();
    }

    // Phase E.1.b: Fsync undo file before closing (ensure durability on shutdown)
    if (current_undo_file_ && current_undo_file_->is_open()) {
        auto status = fsyncFile(*current_undo_file_, getUndoFilePath(current_undo_file_number_));
        if (status != Status::Ok) {
            std::cerr << "[BlockStorage] WARNING: Failed to fsync undo file on close" << std::endl;
        }
        current_undo_file_->close();
    }

    current_write_file_.reset();
    current_undo_file_.reset();

    // Close all read file handles
    read_files_.clear();
    read_undo_files_.clear();
}

// ============================================================================
// Write Operations
// ============================================================================

StatusOr<FilePosition> BlockStorage::writeBlock(const uint256& hash, const Block& block) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (!current_write_file_ || !current_write_file_->is_open()) {
        std::cerr << "[BlockStorage] Block storage not initialized" << std::endl;
        return Status::Io;
    }

    // Serialize the block
    std::string serialized = block.Serialize();
    uint32_t block_size = static_cast<uint32_t>(serialized.size());

    // Phase E.2.b: Check disk space before writing
    // Each block takes: 4 (magic) + 4 (size) + block_size + 4 (checksum) bytes
    uint32_t record_size = 12 + block_size;
    {
        std::filesystem::path datadir = blocks_dir_.parent_path();
        storage::DiskSpaceMonitor disk_monitor(datadir);

        if (!disk_monitor.canWrite(record_size)) {
            auto disk_info = disk_monitor.checkDiskSpace();
            std::cerr << "[BlockStorage] CRITICAL: Insufficient disk space to write block "
                      << hash.GetHex().substr(0, 16) << "...\n";
            std::cerr << "   Block size: " << (record_size / 1024.0) << " KB\n";
            std::cerr << "   Available: " << (disk_info.available_bytes / (1024.0 * 1024.0 * 1024.0)) << " GB\n";
            std::cerr << "   Status: " << storage::DiskSpaceStatusToString(disk_info.status) << "\n";
            std::cerr << "   Free up disk space or enable pruning.\n";
            return Status::Io;
        }
    }

    // Phase E.1.d: Calculate checksum for corruption detection
    uint32_t checksum = calculateChecksum(serialized);

    // Check if we need to rotate to a new file (record_size already calculated above)
    if (current_file_size_ + record_size > MAX_FILE_SIZE) {
        auto rotate_status = rotateFile();
        if (rotate_status != Status::Ok) {
            return rotate_status;
        }
    }

    // Remember the position where we'll write this block
    FilePosition pos(current_file_number_, current_file_size_, block_size);

    // Write magic bytes (network identifier)
    uint32_t magic = getMagicBytes();
    current_write_file_->write(reinterpret_cast<const char*>(&magic), 4);
    if (!current_write_file_->good()) {
        std::cerr << "[BlockStorage] Failed to write magic bytes" << std::endl;
        return Status::Io;
    }

    // Write block size (little-endian)
    current_write_file_->write(reinterpret_cast<const char*>(&block_size), 4);
    if (!current_write_file_->good()) {
        std::cerr << "[BlockStorage] Failed to write block size" << std::endl;
        return Status::Io;
    }

    // Write serialized block data
    current_write_file_->write(serialized.data(), block_size);
    if (!current_write_file_->good()) {
        std::cerr << "[BlockStorage] Failed to write block data" << std::endl;
        return Status::Io;
    }

    // Phase E.1.d: Write checksum for corruption detection
    current_write_file_->write(reinterpret_cast<const char*>(&checksum), 4);
    if (!current_write_file_->good()) {
        std::cerr << "[BlockStorage] Failed to write block checksum" << std::endl;
        return Status::Io;
    }

    // Update state
    current_file_size_ += record_size;
    total_blocks_written_++;
    total_bytes_written_ += record_size;

    // Phase E.1.b: Fsync to disk for durability (CRITICAL for blockchain data)
    // Without fsync, blocks can be lost on power failure → redownload from scratch
    auto fsync_status = fsyncFile(*current_write_file_, getFilePath(current_file_number_));
    if (fsync_status != Status::Ok) {
        std::cerr << "[BlockStorage] CRITICAL: Failed to fsync block " << hash.GetHex().substr(0, 16)
                  << "... (height unknown, file=" << current_file_number_ << ")" << std::endl;
        return fsync_status;
    }

    return pos;
}

// ============================================================================
// Read Operations
// ============================================================================

StatusOr<Block> BlockStorage::readBlock(const FilePosition& pos) const {
    if (pos.isNull()) {
        std::cerr << "[BlockStorage] Cannot read from null FilePosition" << std::endl;
        return Status::Invalid;
    }

    const auto path = getFilePath(pos.file_number);
    std::lock_guard<std::mutex> lock(read_mutex_);

    // Get or create file handle for this file number
    std::shared_ptr<std::ifstream> file;
    auto it = read_files_.find(pos.file_number);
    if (it != read_files_.end()) {
        file = it->second;
    } else {
        // Open new file handle
        if (!std::filesystem::exists(path)) {
            std::cerr << "[BlockStorage] Block file does not exist: " << path.string() << std::endl;
            return Status::NotFound;
        }

        file = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!file->is_open()) {
            std::cerr << "[BlockStorage] Failed to open block file: " << path.string() << std::endl;
            return Status::Io;
        }
        read_files_[pos.file_number] = file;
    }

    // Validate expected on-disk range before read:
    // [magic:4][size:4][data:N][checksum:4] = pos.offset + 12 + pos.size
    const uint64_t required_end =
        static_cast<uint64_t>(pos.offset) + 12ULL + static_cast<uint64_t>(pos.size);
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        std::cerr << "[BlockStorage] Failed to stat block file: " << path.string()
                  << " (" << ec.message() << ")" << std::endl;
        return Status::NotFound;
    }
    if (file_size < required_end) {
        std::cerr << "[BlockStorage] Block position out of range: file=" << path.string()
                  << " size=" << file_size
                  << " required_end=" << required_end << std::endl;
        return Status::NotFound;
    }

    // Clear sticky fail/eof bits before seeking on shared read handles.
    file->clear();

    // Seek to the block position (skip magic + size header)
    file->seekg(pos.offset + 8, std::ios::beg);
    if (!file->good()) {
        std::cerr << "[BlockStorage] Failed to seek to block position" << std::endl;
        return Status::Io;
    }

    // Read block data
    std::string block_data(pos.size, '\0');
    file->read(&block_data[0], pos.size);
    if (!file->good()) {
        std::cerr << "[BlockStorage] Failed to read block data" << std::endl;
        return Status::Io;
    }

    // Phase E.1.d: Read and verify checksum for corruption detection
    uint32_t stored_checksum;
    file->read(reinterpret_cast<char*>(&stored_checksum), 4);
    if (!file->good()) {
        std::cerr << "[BlockStorage] Failed to read block checksum" << std::endl;
        return Status::Io;
    }

    uint32_t calculated_checksum = calculateChecksum(block_data);
    if (stored_checksum != calculated_checksum) {
        std::cerr << "[BlockStorage] FATAL: Block checksum mismatch (corruption detected)" << std::endl;
        std::cerr << "  File: " << getFilePath(pos.file_number).string() << std::endl;
        std::cerr << "  Offset: " << pos.offset << std::endl;
        std::cerr << "  Expected: 0x" << std::hex << stored_checksum << std::endl;
        std::cerr << "  Calculated: 0x" << calculated_checksum << std::dec << std::endl;
        return Status::Corruption;
    }

    // Deserialize block from Dinero wire format (Block::Serialize output)
    auto parsed = Block::Deserialize(reinterpret_cast<const uint8_t*>(block_data.data()), block_data.size());
    if (!parsed.has_value()) {
        std::cerr << "[BlockStorage] Block deserialization failed (invalid block bytes)" << std::endl;
        return Status::Serialization;
    }

    return std::move(parsed.value());
}

Status BlockStorage::hasBlock(const FilePosition& pos) const {
    if (pos.isNull()) {
        return Status::NotFound;
    }

    auto path = getFilePath(pos.file_number);
    if (!std::filesystem::exists(path)) {
        return Status::NotFound;
    }

    // Phase E.1.d: Check if file is large enough to contain this block
    // Format: [magic:4][size:4][data:N][checksum:4] = 12 + block_size
    std::error_code ec;
    auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Status::NotFound;
    }
    if (file_size < pos.offset + 12 + pos.size) {
        return Status::NotFound;
    }

    return Status::Ok;
}

// ============================================================================
// Statistics
// ============================================================================

StatusOr<BlockStorage::StorageStats> BlockStorage::getStats() const {
    std::lock_guard<std::mutex> lock(write_mutex_);

    StorageStats stats;
    stats.current_file_number = current_file_number_;
    stats.current_file_size = current_file_size_;
    stats.total_blocks_written = total_blocks_written_;
    stats.total_bytes_written = total_bytes_written_;

    return stats;
}

Status BlockStorage::flush() {
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (current_write_file_ && current_write_file_->is_open()) {
        // Phase E.1.b: Fsync block file to ensure durability
        auto status = fsyncFile(*current_write_file_, getFilePath(current_file_number_));
        if (status != Status::Ok) {
            std::cerr << "[BlockStorage] Failed to fsync during flush()" << std::endl;
            return status;
        }
        return Status::Ok;
    }

    std::cerr << "[BlockStorage] No file open to flush" << std::endl;
    return Status::Io;
}

// ============================================================================
// Internal Helpers
// ============================================================================

Status BlockStorage::openFile(uint32_t file_number) {
    auto path = getFilePath(file_number);

    // Open in append mode (create if doesn't exist)
    current_write_file_ = std::make_unique<std::ofstream>(
        path, std::ios::binary | std::ios::app
    );

    if (!current_write_file_->is_open()) {
        std::cerr << "[BlockStorage] Failed to open block file: " << path.string() << std::endl;
        return Status::Io;
    }

    // Get current file size
    if (std::filesystem::exists(path)) {
        current_file_size_ = std::filesystem::file_size(path);
    } else {
        current_file_size_ = 0;
    }

    return Status::Ok;
}

Status BlockStorage::rotateFile() {
    // Phase E.1.b: Fsync and close current file before rotation
    if (current_write_file_ && current_write_file_->is_open()) {
        auto fsync_status = fsyncFile(*current_write_file_, getFilePath(current_file_number_));
        if (fsync_status != Status::Ok) {
            std::cerr << "[BlockStorage] Failed to fsync before file rotation" << std::endl;
            return fsync_status;
        }
        current_write_file_->close();
    }

    // Increment file number
    current_file_number_++;
    current_file_size_ = 0;

    std::cout << "[BlockStorage] Rotating to new file: blk"
              << std::setfill('0') << std::setw(5) << current_file_number_
              << ".dat" << std::endl;

    // Open new file
    return openFile(current_file_number_);
}

std::filesystem::path BlockStorage::getFilePath(uint32_t file_number) const {
    std::ostringstream oss;
    oss << "blk" << std::setfill('0') << std::setw(5) << file_number << ".dat";
    return blocks_dir_ / oss.str();
}

uint32_t BlockStorage::getMagicBytes() const {
    return Params().magic;
}

// ============================================================================
// Undo Data Persistence (rev*.dat)
// ============================================================================

StatusOr<FilePosition> BlockStorage::writeUndo(const uint256& hash, const std::vector<uint8_t>& undo_data) {
    std::lock_guard<std::mutex> lock(undo_write_mutex_);

    // Open undo file if not already open
    if (!current_undo_file_ || !current_undo_file_->is_open()) {
        auto status = openUndoFile(current_undo_file_number_);
        if (status != Status::Ok) {
            return status;
        }
    }

    // Phase E.2.b: Check disk space before writing undo data
    // Undo record takes: 4 (size) + data.size() + 4 (checksum) bytes
    uint64_t record_size = 8 + undo_data.size();
    {
        std::filesystem::path datadir = blocks_dir_.parent_path();
        storage::DiskSpaceMonitor disk_monitor(datadir);

        if (!disk_monitor.canWrite(record_size)) {
            auto disk_info = disk_monitor.checkDiskSpace();
            std::cerr << "[BlockStorage] CRITICAL: Insufficient disk space to write undo data for block "
                      << hash.GetHex().substr(0, 16) << "...\n";
            std::cerr << "   Undo size: " << (record_size / 1024.0) << " KB\n";
            std::cerr << "   Available: " << (disk_info.available_bytes / (1024.0 * 1024.0 * 1024.0)) << " GB\n";
            std::cerr << "   Status: " << storage::DiskSpaceStatusToString(disk_info.status) << "\n";
            std::cerr << "   Free up disk space or enable pruning.\n";
            return Status::Io;
        }
    }

    // Check if we need to rotate to a new undo file (128MB limit)
    if (current_undo_file_size_ + undo_data.size() + 8 > MAX_FILE_SIZE) {
        auto rotate_status = rotateUndoFile();
        if (rotate_status != Status::Ok) {
            return rotate_status;
        }
    }

    // Record starting position
    FilePosition pos;
    pos.file_number = current_undo_file_number_;
    pos.offset = current_undo_file_size_;
    pos.size = static_cast<uint32_t>(undo_data.size());

    // Calculate checksum (double SHA-256, first 4 bytes)
    uint32_t checksum = calculateChecksum(undo_data);

    // Write: [size:4][data:N][checksum:4]
    uint32_t size = static_cast<uint32_t>(undo_data.size());
    current_undo_file_->write(reinterpret_cast<const char*>(&size), 4);
    current_undo_file_->write(reinterpret_cast<const char*>(undo_data.data()), undo_data.size());
    current_undo_file_->write(reinterpret_cast<const char*>(&checksum), 4);

    if (!current_undo_file_->good()) {
        std::cerr << "[BlockStorage] Failed to write undo data" << std::endl;
        return Status::Io;
    }

    // Phase E.1.b: Fsync to ensure durability (CRITICAL for crash-safe reorgs)
    // Without fsync, undo data can be lost on power failure → cannot reorg safely
    auto fsync_status = fsyncFile(*current_undo_file_, getUndoFilePath(current_undo_file_number_));
    if (fsync_status != Status::Ok) {
        std::cerr << "[BlockStorage] CRITICAL: Failed to fsync undo data for block "
                  << hash.GetHex().substr(0, 16) << "... (file=" << current_undo_file_number_ << ")" << std::endl;
        return fsync_status;
    }

    // Update state
    current_undo_file_size_ += undo_data.size() + 8;  // size(4) + data + checksum(4)

    return pos;
}

StatusOr<std::vector<uint8_t>> BlockStorage::readUndo(const FilePosition& pos) const {
    std::lock_guard<std::mutex> lock(read_mutex_);

    // Open undo file if not cached
    if (read_undo_files_.find(pos.file_number) == read_undo_files_.end()) {
        auto file_path = getUndoFilePath(pos.file_number);
        if (!std::filesystem::exists(file_path)) {
            std::cerr << "[BlockStorage] FATAL: Undo file missing: " << file_path << std::endl;
            return Status::NotFound;
        }

        auto file = std::make_shared<std::ifstream>(file_path, std::ios::binary);
        if (!file->is_open()) {
            std::cerr << "[BlockStorage] FATAL: Failed to open undo file: " << file_path << std::endl;
            return Status::Io;
        }

        read_undo_files_[pos.file_number] = file;
    }

    const auto file_path = getUndoFilePath(pos.file_number);
    auto& file = read_undo_files_[pos.file_number];

    // Validate expected range for [size:4][data:N][checksum:4].
    const uint64_t required_end =
        static_cast<uint64_t>(pos.offset) + 8ULL + static_cast<uint64_t>(pos.size);
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        std::cerr << "[BlockStorage] FATAL: Failed to stat undo file: "
                  << file_path.string() << " (" << ec.message() << ")" << std::endl;
        return Status::NotFound;
    }
    if (file_size < required_end) {
        std::cerr << "[BlockStorage] FATAL: Undo position out of range (file="
                  << file_path.string() << ", size=" << file_size
                  << ", required_end=" << required_end << ")" << std::endl;
        return Status::NotFound;
    }

    // Clear sticky fail/eof bits before seeking on shared read handles.
    file->clear();

    // Seek to position
    file->seekg(pos.offset);
    if (file->fail()) {
        std::cerr << "[BlockStorage] FATAL: Failed to seek in undo file" << std::endl;
        return Status::Io;
    }

    // Read size
    uint32_t size;
    file->read(reinterpret_cast<char*>(&size), 4);
    if (file->fail() || size != pos.size) {
        std::cerr << "[BlockStorage] FATAL: Undo data size mismatch (expected "
                  << pos.size << ", got " << size << ")" << std::endl;
        return Status::Corruption;
    }

    // Read data
    std::vector<uint8_t> undo_data(size);
    file->read(reinterpret_cast<char*>(undo_data.data()), size);
    if (file->fail()) {
        std::cerr << "[BlockStorage] FATAL: Failed to read undo data" << std::endl;
        return Status::Io;
    }

    // Read checksum
    uint32_t stored_checksum;
    file->read(reinterpret_cast<char*>(&stored_checksum), 4);
    if (file->fail()) {
        std::cerr << "[BlockStorage] FATAL: Failed to read undo checksum" << std::endl;
        return Status::Io;
    }

    // Verify checksum
    uint32_t calculated_checksum = calculateChecksum(undo_data);
    if (stored_checksum != calculated_checksum) {
        std::cerr << "[BlockStorage] FATAL: Undo checksum mismatch (corruption detected)" << std::endl;
        return Status::Corruption;
    }

    return undo_data;
}

Status BlockStorage::openUndoFile(uint32_t file_number) {
    auto file_path = getUndoFilePath(file_number);

    // Check if file exists
    bool file_exists = std::filesystem::exists(file_path);

    // Open file in append mode
    current_undo_file_ = std::make_unique<std::ofstream>(
        file_path,
        std::ios::binary | std::ios::app
    );

    if (!current_undo_file_->is_open()) {
        std::cerr << "[BlockStorage] Failed to open undo file: " << file_path << std::endl;
        return Status::Io;
    }

    // If file exists, get current size; otherwise start at 0
    if (file_exists) {
        current_undo_file_size_ = std::filesystem::file_size(file_path);
    } else {
        current_undo_file_size_ = 0;
    }

    return Status::Ok;
}

Status BlockStorage::rotateUndoFile() {
    // Phase E.1.b: Fsync and close current undo file before rotation
    if (current_undo_file_ && current_undo_file_->is_open()) {
        auto fsync_status = fsyncFile(*current_undo_file_, getUndoFilePath(current_undo_file_number_));
        if (fsync_status != Status::Ok) {
            std::cerr << "[BlockStorage] Failed to fsync before undo file rotation" << std::endl;
            return fsync_status;
        }
        current_undo_file_->close();
    }

    // Increment file number
    current_undo_file_number_++;
    current_undo_file_size_ = 0;

    std::cout << "[BlockStorage] Rotating to new undo file: rev"
              << std::setfill('0') << std::setw(5) << current_undo_file_number_
              << ".dat" << std::endl;

    // Open new undo file
    return openUndoFile(current_undo_file_number_);
}

std::filesystem::path BlockStorage::getUndoFilePath(uint32_t file_number) const {
    std::ostringstream oss;
    oss << "rev" << std::setfill('0') << std::setw(5) << file_number << ".dat";
    return blocks_dir_ / oss.str();
}

uint32_t BlockStorage::calculateChecksum(const std::vector<uint8_t>& data) const {
    // Simplified checksum: FNV-1a hash
    // In production, this should use actual SHA-256 from crypto library
    // For now, use a simple hash to avoid adding dependencies

    uint32_t hash = 0x811c9dc5;  // FNV-1a initial value
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 0x01000193;  // FNV-1a prime
    }

    return hash;
}

// Phase E.1.d: String overload for block data checksums
uint32_t BlockStorage::calculateChecksum(const std::string& data) const {
    uint32_t hash = 0x811c9dc5;  // FNV-1a initial value
    for (char byte : data) {
        hash ^= static_cast<uint8_t>(byte);
        hash *= 0x01000193;  // FNV-1a prime
    }
    return hash;
}

// ============================================================================
// F.7.2: Pruning Gate (Invariants Only - No Deletion)
// ============================================================================

Status BlockStorage::checkPruningInvariants(const class BlockIndex* pindex) const {
    // This is a GATE function that future pruning code MUST call.
    // It enforces pruning invariants with HARD checks.
    // Actual pruning (file deletion) is NOT implemented here.

    if (!pindex) {
        return Status::Invalid;
    }

    // INVARIANT 1: Block must have BLOCK_HAVE_UNDO flag set
    // This ensures metadata consistency - we don't prune blocks without undo tracking
    if (!(pindex->status & BlockIndex::BLOCK_HAVE_UNDO)) {
        std::cerr << "[BlockStorage] PRUNING GATE: Block " << pindex->hash.GetHex()
                  << " does not have BLOCK_HAVE_UNDO flag - CANNOT PRUNE" << std::endl;
        return Status::Invalid;
    }

    // INVARIANT 2: Undo position must be valid (non-null)
    // This ensures we have a valid file position for undo data
    if (pindex->undo_pos.isNull()) {
        std::cerr << "[BlockStorage] PRUNING GATE: Block " << pindex->hash.GetHex()
                  << " has null undo position - CANNOT PRUNE" << std::endl;
        return Status::Invalid;
    }

    // INVARIANT 3: Undo data must actually be available on disk
    // This is the final safety check - verify the file exists and is readable
    auto undo_check = hasBlock(pindex->undo_pos);
    if (undo_check != Status::Ok) {
        std::cerr << "[BlockStorage] PRUNING GATE: Block " << pindex->hash.GetHex()
                  << " undo data not available on disk - CANNOT PRUNE" << std::endl;
        return Status::Invalid;
    }

    // All storage-level invariants satisfied
    // Note: Chain-level invariants (depth, ancestry) are checked by BlockIndex::isPrunable()
    // Future pruning code should call BOTH functions before pruning
    return Status::Ok;
}

// ============================================================================
// Phase P.2: Physical Block Deletion
// ============================================================================

Status BlockStorage::zeroOutFileRegion(const std::filesystem::path& file_path,
                                       uint64_t offset, uint32_t size) {
    if (size == 0) {
        return Status::Ok;  // Nothing to zero
    }

    // Open file for read/write (don't truncate)
    std::fstream file(file_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[BlockStorage] Failed to open file for zeroing: " << file_path << std::endl;
        return Status::Io;
    }

    // Seek to offset
    file.seekp(offset);
    if (!file.good()) {
        std::cerr << "[BlockStorage] Failed to seek to offset " << offset << " in " << file_path << std::endl;
        return Status::Io;
    }

    // Write zeros in chunks (64KB at a time for efficiency)
    constexpr uint32_t ZERO_CHUNK_SIZE = 65536;  // 64KB
    std::vector<uint8_t> zero_buffer(ZERO_CHUNK_SIZE, 0);

    uint32_t bytes_remaining = size;
    while (bytes_remaining > 0) {
        uint32_t chunk_size = std::min(bytes_remaining, ZERO_CHUNK_SIZE);
        file.write(reinterpret_cast<const char*>(zero_buffer.data()), chunk_size);

        if (!file.good()) {
            std::cerr << "[BlockStorage] Failed to write zeros at offset " << offset << std::endl;
            return Status::Io;
        }

        bytes_remaining -= chunk_size;
    }

    // Flush to ensure data is written
    file.flush();
    if (!file.good()) {
        std::cerr << "[BlockStorage] Failed to flush after zeroing" << std::endl;
        return Status::Io;
    }

    return Status::Ok;
}

Status BlockStorage::pruneUndoData(BlockIndex* pindex) {
    if (!pindex) {
        return Status::Invalid;
    }

    // Safety check: must have undo data flag
    if (!(pindex->status & BlockIndex::BLOCK_HAVE_UNDO)) {
        // Already pruned or never had undo data
        return Status::Ok;
    }

    // Safety check: undo position must be valid
    if (pindex->undo_pos.isNull()) {
        std::cerr << "[BlockStorage] Cannot prune undo: null undo position for block "
                  << pindex->hash.GetHex().substr(0, 16) << "..." << std::endl;
        return Status::Invalid;
    }

    // Get undo file path
    std::filesystem::path undo_file_path = getUndoFilePath(pindex->undo_pos.file_number);

    // Zero out the undo data region
    // Format: [size:4][data:N][checksum:4], so total = 8 + undo_data_size
    uint32_t total_undo_size = 8 + pindex->undo_pos.size;
    auto zero_status = zeroOutFileRegion(undo_file_path, pindex->undo_pos.offset, total_undo_size);

    if (zero_status != Status::Ok) {
        std::cerr << "[BlockStorage] Failed to zero undo data for block "
                  << pindex->hash.GetHex().substr(0, 16) << "..." << std::endl;
        return zero_status;
    }

    // Clear BLOCK_HAVE_UNDO flag
    pindex->status &= ~BlockIndex::BLOCK_HAVE_UNDO;

    return Status::Ok;
}

Status BlockStorage::pruneBlockData(BlockIndex* pindex) {
    if (!pindex) {
        return Status::Invalid;
    }

    // SAFETY GATE: Check all pruning invariants
    auto invariant_status = checkPruningInvariants(pindex);
    if (invariant_status != Status::Ok) {
        std::cerr << "[BlockStorage] Pruning invariants failed for block "
                  << pindex->hash.GetHex().substr(0, 16) << "..." << std::endl;
        return invariant_status;
    }

    // Track bytes for stats
    uint64_t bytes_pruned = 0;

    // Prune undo data first
    uint32_t undo_size = pindex->undo_pos.size;
    auto undo_status = pruneUndoData(pindex);
    if (undo_status != Status::Ok) {
        return undo_status;
    }
    bytes_pruned += (8 + undo_size);  // undo data + overhead

    // Prune block data
    if (pindex->status & BlockIndex::BLOCK_HAVE_DATA) {
        // Safety check: block position must be valid
        if (pindex->file_pos.isNull()) {
            std::cerr << "[BlockStorage] Cannot prune block: null file position for block "
                      << pindex->hash.GetHex().substr(0, 16) << "..." << std::endl;
            return Status::Invalid;
        }

        // Get block file path
        std::filesystem::path block_file_path = getFilePath(pindex->file_pos.file_number);

        // Phase E.1.d: Zero out the block data region (including checksum)
        // Format: [magic:4][size:4][data:N][checksum:4], so total = 12 + block_size
        uint32_t total_block_size = 12 + pindex->file_pos.size;
        auto zero_status = zeroOutFileRegion(block_file_path, pindex->file_pos.offset, total_block_size);

        if (zero_status != Status::Ok) {
            std::cerr << "[BlockStorage] Failed to zero block data for block "
                      << pindex->hash.GetHex().substr(0, 16) << "..." << std::endl;
            return zero_status;
        }

        bytes_pruned += total_block_size;

        // Clear BLOCK_HAVE_DATA flag
        pindex->status &= ~BlockIndex::BLOCK_HAVE_DATA;
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        total_bytes_written_ -= bytes_pruned;  // Adjust total accounting
    }

    std::cout << "[BlockStorage] Pruned block " << pindex->hash.GetHex().substr(0, 16)
              << "... (height=" << pindex->height << ", freed=" << bytes_pruned << " bytes)" << std::endl;

    return Status::Ok;
}

// Phase P.2: Prune undo data using CBlockIndex disk positions
// Current architecture: Blocks in RocksDB, Undo in flat files
// This function only prunes the undo data (flat file zero-out)
// Block data deletion from RocksDB handled by ChainDB
//
// TODO: This function is not declared in block_storage.h - add declaration if needed
// Commented out for now to fix build
/*
Status BlockStorage::pruneUndoDataFromCBlockIndex(const uint256& block_hash,
                                                   uint32_t undo_file_num,
                                                   uint32_t undo_offset,
                                                   uint32_t undo_data_size,
                                                   uint32_t height) {
    // Safety check: undo position must be valid
    if (undo_file_num == 0 || undo_data_size == 0) {
        // No undo data or already pruned
        return Status::Ok;
    }

    // Get undo file path
    std::filesystem::path undo_file_path = getUndoFilePath(undo_file_num);

    // Zero out the undo data region
    // Format: [size:4][data:N][checksum:4], so total = 8 + undo_data_size
    uint32_t total_undo_size = 8 + undo_data_size;
    auto undo_status = zeroOutFileRegion(undo_file_path, undo_offset, total_undo_size);
    if (undo_status != Status::Ok) {
        std::cerr << "[BlockStorage] Failed to zero undo data for block "
                  << block_hash.GetHex().substr(0, 16) << "..." << std::endl;
        return undo_status;
    }

    std::cout << "[BlockStorage] Pruned undo data for block " << block_hash.GetHex().substr(0, 16)
              << "... (height=" << height << ", freed=" << total_undo_size << " bytes)" << std::endl;

    return Status::Ok;
}
*/

// ============================================================================
// Phase 34.8: File-Level Pruning
// ============================================================================

std::vector<BlockStorage::FileInfo> BlockStorage::getFileInfo() const {
    std::vector<FileInfo> result;
    std::lock_guard<std::mutex> lock(read_mutex_);

    // Scan all block files that exist
    for (uint32_t file_num = 0; file_num <= current_file_number_; ++file_num) {
        std::filesystem::path blk_path = getFilePath(file_num);
        if (!std::filesystem::exists(blk_path)) {
            continue;
        }

        FileInfo info;
        info.file_number = file_num;

        // Get file size
        std::error_code ec;
        info.file_size = std::filesystem::file_size(blk_path, ec);
        if (ec) {
            info.file_size = 0;
        }

        // TODO: To get lowest/highest height, we'd need to scan the file
        // or maintain metadata. For now, use file number as a proxy
        // (lower file numbers = older blocks = lower heights)
        // Real implementation would track this in ChainDB

        result.push_back(info);
    }

    return result;
}

StatusOr<uint64_t> BlockStorage::deleteFile(uint32_t file_number) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    // Cannot delete the current write file
    if (file_number >= current_file_number_) {
        return StatusOr<uint64_t>(Status::Invalid);
    }

    std::filesystem::path blk_path = getFilePath(file_number);
    std::filesystem::path rev_path = getUndoFilePath(file_number);

    uint64_t bytes_freed = 0;
    std::error_code ec;

    // Get file sizes before deletion
    if (std::filesystem::exists(blk_path)) {
        bytes_freed += std::filesystem::file_size(blk_path, ec);
    }
    if (std::filesystem::exists(rev_path)) {
        bytes_freed += std::filesystem::file_size(rev_path, ec);
    }

    // Delete block file
    if (std::filesystem::exists(blk_path)) {
        if (!std::filesystem::remove(blk_path, ec)) {
            std::cerr << "[BlockStorage] Failed to delete " << blk_path.string()
                      << ": " << ec.message() << std::endl;
            return StatusOr<uint64_t>(Status::Io);
        }
        std::cout << "[BlockStorage] Deleted " << blk_path.filename().string() << std::endl;
    }

    // Delete undo file
    if (std::filesystem::exists(rev_path)) {
        if (!std::filesystem::remove(rev_path, ec)) {
            std::cerr << "[BlockStorage] Failed to delete " << rev_path.string()
                      << ": " << ec.message() << std::endl;
            return StatusOr<uint64_t>(Status::Io);
        }
        std::cout << "[BlockStorage] Deleted " << rev_path.filename().string() << std::endl;
    }

    // Clear cached file handles
    read_files_.erase(file_number);
    read_undo_files_.erase(file_number);

    return StatusOr<uint64_t>(bytes_freed);
}

Status BlockStorage::zeroRegion(uint32_t file_number, uint64_t offset, uint32_t size, bool is_undo) {
    std::filesystem::path file_path = is_undo ?
        getUndoFilePath(file_number) : getFilePath(file_number);

    return zeroOutFileRegion(file_path, offset, size);
}

} // namespace dinero
