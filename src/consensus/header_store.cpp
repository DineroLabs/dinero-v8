/**
 * Phase N.1: Header Storage Implementation
 *
 * Persistent storage for header-only sync with restart safety.
 */

#include "consensus/header_store.h"
#include "consensus/chainparams.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/iterator.h>
#include <rocksdb/env.h>
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <sstream>
#include <system_error>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace dinero {
namespace consensus {

namespace {
    // Namespace prefix for headers
    const std::string HEADER_PREFIX = "h/";
    const std::string BEST_HEADER_KEY = "best";
    const std::string SCHEMA_METADATA_KEY = "schema";
    constexpr size_t kLegacyPersistedHeaderSize = 112;
    constexpr size_t kFullPersistedHeaderSize = 128;
    constexpr uint32_t kCurrentHeaderStoreSchemaVersion = 3;
    constexpr int kOpenRetryCount = 3;
    constexpr int kOpenRetryDelayMs = 50;

    bool IsLockConflict(const rocksdb::Status& status) {
        if (status.ok()) {
            return false;
        }
        const std::string msg = status.ToString();
        return msg.find("lock") != std::string::npos ||
               msg.find("Resource temporarily unavailable") != std::string::npos;
    }

    bool IsEmbeddedSandboxPath(const std::string& path) {
        return path.find("/Library/Containers/") != std::string::npos ||
               path.find("/var/mobile/Containers/") != std::string::npos;
    }

#if defined(__APPLE__)
    class HeaderStoreNoLockEnv : public rocksdb::EnvWrapper {
    public:
        explicit HeaderStoreNoLockEnv(rocksdb::Env* base) : EnvWrapper(base) {}

        rocksdb::Status LockFile(const std::string& /*fname*/,
                                 rocksdb::FileLock** lock) override {
            *lock = reinterpret_cast<rocksdb::FileLock*>(new char);
            return rocksdb::Status::OK();
        }

        rocksdb::Status UnlockFile(rocksdb::FileLock* lock) override {
            delete reinterpret_cast<char*>(lock);
            return rocksdb::Status::OK();
        }
    };

    rocksdb::Env* GetHeaderStoreNoLockEnv() {
        static HeaderStoreNoLockEnv env(rocksdb::Env::Default());
        return &env;
    }
#endif

    void ConfigureEmbeddedLockBehavior(rocksdb::Options& options, const std::string& db_path) {
#if defined(__APPLE__)
        if (IsEmbeddedSandboxPath(db_path)) {
            options.env = GetHeaderStoreNoLockEnv();
        }
#else
        (void)options;
        (void)db_path;
#endif
    }

    // Serialization helpers
    void WriteUint32LE(std::vector<uint8_t>& buf, uint32_t value) {
        buf.push_back(value & 0xff);
        buf.push_back((value >> 8) & 0xff);
        buf.push_back((value >> 16) & 0xff);
        buf.push_back((value >> 24) & 0xff);
    }

    uint32_t ReadUint32LE(const uint8_t* data, size_t& offset) {
        uint32_t value = data[offset] |
                        (data[offset + 1] << 8) |
                        (data[offset + 2] << 16) |
                        (data[offset + 3] << 24);
        offset += 4;
        return value;
    }

    [[maybe_unused]] void WriteUint64LE(std::vector<uint8_t>& buf, uint64_t value) {
        for (int i = 0; i < 8; i++) {
            buf.push_back((value >> (i * 8)) & 0xff);
        }
    }

    [[maybe_unused]] uint64_t ReadUint64LE(const uint8_t* data, size_t& offset) {
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) {
            value |= (static_cast<uint64_t>(data[offset + i]) << (i * 8));
        }
        offset += 8;
        return value;
    }

    void WriteUint256(std::vector<uint8_t>& buf, const uint256& hash) {
        buf.insert(buf.end(), hash.data, hash.data + 32);
    }

    uint256 ReadUint256(const uint8_t* data, size_t& offset) {
        uint256 hash;
        std::memcpy(hash.data, data + offset, 32);
        offset += 32;
        return hash;
    }

    void WriteArithUint256(std::vector<uint8_t>& buf, const arith_uint256& chainwork) {
        // Store as hex string for simplicity (Phase M.0: Inline at storage boundary)
        const auto hex = chainwork.GetHex();
        WriteUint32LE(buf, hex.size());
        buf.insert(buf.end(), hex.begin(), hex.end());
    }

    arith_uint256 ReadArithUint256(const uint8_t* data, size_t& offset) {
        uint32_t len = ReadUint32LE(data, offset);
        std::string hex(reinterpret_cast<const char*>(data + offset), len);
        offset += len;
        return ChainworkFromHex(hex);
    }

    HeaderStore::SchemaMetadata CurrentSchemaMetadata() {
        HeaderStore::SchemaMetadata metadata;
        metadata.version = kCurrentHeaderStoreSchemaVersion;
        metadata.network = Params().name;
        metadata.header_size = static_cast<uint32_t>(kFullPersistedHeaderSize);
        return metadata;
    }

    bool SchemaMetadataMatches(const HeaderStore::SchemaMetadata& lhs,
                               const HeaderStore::SchemaMetadata& rhs) {
        return lhs.version == rhs.version &&
               lhs.network == rhs.network &&
               lhs.header_size == rhs.header_size;
    }

    std::string SchemaMetadataToString(const HeaderStore::SchemaMetadata& metadata) {
        std::ostringstream oss;
        oss << "version=" << metadata.version
            << ",network=" << metadata.network
            << ",header_size=" << metadata.header_size;
        return oss.str();
    }
}

// ============================================================================
// HeaderStore Implementation
// ============================================================================

HeaderStore::HeaderStore(const std::string& db_path)
    : db_path_(db_path)
    , db_(nullptr)
    , is_open_(false)
    , has_legacy_entries_(false)
{
}

HeaderStore::~HeaderStore() {
    Close();
}

bool HeaderStore::Open() {
    if (is_open_) {
        return true;  // Already open
    }

    has_legacy_entries_ = false;
    schema_recovery_required_ = false;
    schema_recovery_reason_.clear();
    expected_schema_metadata_ = CurrentSchemaMetadata();
    persisted_schema_metadata_.reset();

    rocksdb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;
    ConfigureEmbeddedLockBehavior(options, db_path_);

#if defined(__APPLE__)
    const bool no_lock_env = (options.env == GetHeaderStoreNoLockEnv());
    if (no_lock_env) {
        std::cout << "[HeaderStore] Using NoLockEnv for embedded path: " << db_path_ << std::endl;
    }
#endif

    rocksdb::DB* db_ptr = nullptr;
    rocksdb::Status status;
    bool removed_lock_file = false;
    for (int attempt = 0; attempt < kOpenRetryCount; ++attempt) {
        status = rocksdb::DB::Open(options, db_path_, &db_ptr);
        if (status.ok()) {
            break;
        }

        if (!IsLockConflict(status) || attempt + 1 >= kOpenRetryCount) {
            break;
        }

        // Embedded lifecycle race: stale LOCK file can survive a crash/restart cycle.
        if (!removed_lock_file) {
            const std::string lock_path = db_path_ + "/LOCK";
            std::error_code ec;
            if (std::filesystem::exists(lock_path, ec) && !ec) {
                std::filesystem::remove(lock_path, ec);
                if (!ec) {
                    removed_lock_file = true;
                    std::cerr << "[HeaderStore] Removed stale LOCK file, retrying open: "
                              << lock_path << std::endl;
                }
            }
        }

        std::cerr << "[HeaderStore] Lock conflict while opening database, retrying once: "
                  << status.ToString() << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(kOpenRetryDelayMs));
    }

    if (!status.ok()) {
        std::cerr << "[HeaderStore] Failed to open database: " << status.ToString() << std::endl;
        return false;
    }

    db_ = db_ptr;
    is_open_ = true;

    const bool has_header_records = HasPersistedHeaderRecords();
    const bool has_best_marker = HasPersistedBestMarker();
    const bool has_schema_key = HasPersistedSchemaKey();

    SchemaMetadata persisted_metadata;
    if (LoadSchemaMetadata(persisted_metadata)) {
        persisted_schema_metadata_ = persisted_metadata;
        if (!SchemaMetadataMatches(persisted_metadata, expected_schema_metadata_)) {
            if (has_header_records || has_best_marker) {
                SetSchemaRecoveryRequired(
                    "schema mismatch (persisted " + SchemaMetadataToString(persisted_metadata) +
                    ", expected " + SchemaMetadataToString(expected_schema_metadata_) + ")");
            } else if (!WriteSchemaMetadata(expected_schema_metadata_)) {
                std::cerr << "[HeaderStore] Failed to rewrite schema metadata" << std::endl;
                Close();
                return false;
            }
        }
    } else if (has_schema_key) {
        if (has_header_records || has_best_marker) {
            SetSchemaRecoveryRequired("schema metadata is unreadable");
        } else if (!WriteSchemaMetadata(expected_schema_metadata_)) {
            std::cerr << "[HeaderStore] Failed to initialize schema metadata" << std::endl;
            Close();
            return false;
        }
    } else if (!has_header_records && !has_best_marker) {
        if (!WriteSchemaMetadata(expected_schema_metadata_)) {
            std::cerr << "[HeaderStore] Failed to initialize schema metadata" << std::endl;
            Close();
            return false;
        }
    }

    if (schema_recovery_required_) {
        std::cerr << "[HeaderStore] Schema recovery required: "
                  << schema_recovery_reason_ << std::endl;
    }

    return true;
}

void HeaderStore::Close() {
    if (db_) {
        // Phase N.1: Explicit flush before shutdown
        // RocksDB 8.x requires flush to make data visible to iterators after reopen
        rocksdb::FlushOptions flush_opts;
        flush_opts.wait = true;
        db_->Flush(flush_opts);

        delete db_;
        db_ = nullptr;
        is_open_ = false;
    }
}

bool HeaderStore::StoreHeader(const HeaderIndexEntry& entry) {
    if (!is_open_) {
        return false;
    }

    // Serialize header
    std::vector<uint8_t> data = SerializeHeader(entry);

    // Store with hash as key
    std::string key = MakeHeaderKey(entry.hash);
    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Put(
        write_opts,
        key,
        rocksdb::Slice(reinterpret_cast<const char*>(data.data()), data.size())
    );

    return status.ok();
}

bool HeaderStore::LoadHeader(const uint256& hash, HeaderIndexEntry& entry) {
    if (!is_open_) {
        return false;
    }

    std::string key = MakeHeaderKey(hash);
    std::string value;

    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    rocksdb::Status status = db_->Get(read_opts, key, &value);
    if (!status.ok()) {
        return false;
    }

    // Deserialize
    std::vector<uint8_t> data(value.begin(), value.end());
    return DeserializeHeader(data, entry);
}

bool HeaderStore::StoreBestHeader(const uint256& hash) {
    if (!is_open_) {
        return false;
    }

    std::string key = MakeBestHeaderKey();
    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Put(
        write_opts,
        key,
        rocksdb::Slice(reinterpret_cast<const char*>(hash.data), 32)
    );

    return status.ok();
}

bool HeaderStore::LoadBestHeader(uint256& hash) {
    if (!is_open_) {
        return false;
    }

    std::string key = MakeBestHeaderKey();
    std::string value;

    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    rocksdb::Status status = db_->Get(read_opts, key, &value);
    if (!status.ok() || value.size() != 32) {
        return false;
    }

    std::memcpy(hash.data, value.data(), 32);
    return true;
}

bool HeaderStore::DeleteBestHeader() {
    if (!is_open_) {
        return false;
    }

    std::string key = MakeBestHeaderKey();
    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Delete(write_opts, key);
    return status.ok();
}

bool HeaderStore::LoadAllHeaders(std::vector<HeaderIndexEntry>& headers) {
    if (!is_open_) {
        return false;
    }

    headers.clear();

    if (schema_recovery_required_) {
        return false;
    }

    // Iterate over all headers
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kDBIterator;
    rocksdb::Iterator* it = db_->NewIterator(read_opts);
    size_t deserialize_failures = 0;

    for (it->Seek(HEADER_PREFIX); it->Valid(); it->Next()) {
        std::string key = it->key().ToString();

        // Stop if we've gone past the header prefix
        if (key.compare(0, HEADER_PREFIX.size(), HEADER_PREFIX) != 0) {
            break;
        }

        // Skip metadata keys.
        if (key == MakeBestHeaderKey() || key == MakeSchemaMetadataKey()) {
            continue;
        }

        // Deserialize header
        std::string value = it->value().ToString();
        std::vector<uint8_t> data(value.begin(), value.end());

        HeaderIndexEntry entry;
        if (DeserializeHeader(data, entry)) {
            headers.push_back(entry);
        } else {
            ++deserialize_failures;
            SetSchemaRecoveryRequired("invalid persisted header entry encountered during scan");
        }
    }

    delete it;

    if (deserialize_failures > 0) {
        return false;
    }

    if (!persisted_schema_metadata_.has_value() && !headers.empty() && !has_legacy_entries_) {
        if (!WriteSchemaMetadata(expected_schema_metadata_)) {
            SetSchemaRecoveryRequired("failed to persist schema metadata for existing header store");
            return false;
        }
    }

    return true;
}

size_t HeaderStore::GetHeaderCount() const {
    if (!is_open_) {
        return 0;
    }

    size_t count = 0;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kDBIterator;
    rocksdb::Iterator* it = db_->NewIterator(read_opts);

    for (it->Seek(HEADER_PREFIX); it->Valid(); it->Next()) {
        std::string key = it->key().ToString();
        if (key.compare(0, HEADER_PREFIX.size(), HEADER_PREFIX) != 0) {
            break;
        }
        if (key != MakeBestHeaderKey() && key != MakeSchemaMetadataKey()) {
            count++;
        }
    }

    delete it;
    return count;
}

bool HeaderStore::DeleteHeader(const uint256& hash) {
    if (!is_open_) {
        return false;
    }

    std::string key = MakeHeaderKey(hash);
    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Delete(write_opts, key);
    return status.ok();
}

bool HeaderStore::ClearAll() {
    if (!is_open_) {
        return false;
    }

    // Delete all headers
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kDBIterator;
    rocksdb::Iterator* it = db_->NewIterator(read_opts);

    rocksdb::WriteOptions write_opts;

    for (it->Seek(HEADER_PREFIX); it->Valid(); it->Next()) {
        std::string key = it->key().ToString();
        if (key.compare(0, HEADER_PREFIX.size(), HEADER_PREFIX) != 0) {
            break;
        }
        db_->Delete(write_opts, key);
    }

    delete it;
    has_legacy_entries_ = false;
    schema_recovery_required_ = false;
    schema_recovery_reason_.clear();
    if (expected_schema_metadata_.version == 0) {
        expected_schema_metadata_ = CurrentSchemaMetadata();
    }
    return WriteSchemaMetadata(expected_schema_metadata_);
}

// ============================================================================
// Private Methods
// ============================================================================

std::vector<uint8_t> HeaderStore::SerializeHeader(const HeaderIndexEntry& entry) {
    std::vector<uint8_t> buf;

    // Phase N.1: Serialize header entry for persistent storage
    // Format:
    // - hash (32 bytes)
    // - prev_hash (32 bytes)
    // - height (4 bytes)
    // - chainwork (variable, length-prefixed)
    // - BlockHeader fields:
    //   - version (4 bytes)
    //   - prevBlockHash (length-prefixed string)
    //   - merkleRoot (length-prefixed string)
    //   - time (4 bytes)
    //   - bits (4 bytes)
    //   - nonce (4 bytes)
    //   - utreexoCommitment (length-prefixed string)

    WriteUint256(buf, entry.hash);
    WriteUint256(buf, entry.prev_hash);
    WriteUint32LE(buf, entry.height);
    WriteArithUint256(buf, entry.chainwork);

    // Persist the full consensus header bytes so restart-state exactly matches
    // the 128-byte BlockHeader v1 hashing/persistence layout.
    const std::string header_bytes = entry.header.Serialize();
    buf.insert(buf.end(), header_bytes.begin(), header_bytes.end());

    return buf;
}

bool HeaderStore::DeserializeHeader(const std::vector<uint8_t>& data, HeaderIndexEntry& entry) {
    if (data.size() < 72) {  // hash + prev + height + empty chainwork
        return false;
    }

    size_t offset = 0;

    entry.hash = ReadUint256(data.data(), offset);
    entry.prev_hash = ReadUint256(data.data(), offset);
    entry.height = ReadUint32LE(data.data(), offset);
    entry.chainwork = ReadArithUint256(data.data(), offset);

    const size_t remaining = data.size() - offset;

    if (remaining == kFullPersistedHeaderSize) {
        auto header_opt = BlockHeader::Deserialize(data.data() + offset, remaining);
        if (!header_opt) {
            return false;
        }
        entry.header = *header_opt;
        offset += remaining;
    } else if (remaining == kLegacyPersistedHeaderSize) {
        has_legacy_entries_ = true;

        // Legacy Phase N persistence truncated the frozen consensus header to:
        // version(4) + prev(32) + merkle(32) + timestamp(4) + difficulty(4) +
        // nonce(4) + utreexo(32). Reconstruct the missing v1 fields defensively.
        entry.header.version = ReadUint32LE(data.data(), offset);

        if (offset + 32 > data.size()) return false;
        std::memcpy(entry.header.prev_block_hash.data, data.data() + offset, 32);
        offset += 32;

        if (offset + 32 > data.size()) return false;
        std::memcpy(entry.header.merkle_root.data, data.data() + offset, 32);
        offset += 32;

        entry.header.timestamp = static_cast<uint64_t>(ReadUint32LE(data.data(), offset));
        entry.header.difficulty = ReadUint32LE(data.data(), offset);
        entry.header.nonce = ReadUint32LE(data.data(), offset);

        if (offset + 32 > data.size()) return false;
        std::memcpy(entry.header.utreexo_root.data, data.data() + offset, 32);
        offset += 32;

        entry.header.ZeroReserved();
    } else {
        return false;
    }

    // Parent linkage is rebuilt on load (not serialized)
    entry.parent = nullptr;

    // Persisted identity should agree with the reconstructed consensus header.
    return entry.header.GetHash() == entry.hash &&
           entry.header.prev_block_hash == entry.prev_hash;
}

std::string HeaderStore::MakeHeaderKey(const uint256& hash) const {
    // Key format: "h/<hash>"
    std::string key = HEADER_PREFIX;
    key.append(reinterpret_cast<const char*>(hash.data), 32);
    return key;
}

std::string HeaderStore::MakeBestHeaderKey() const {
    return HEADER_PREFIX + BEST_HEADER_KEY;
}

bool HeaderStore::WriteSchemaMetadata(const SchemaMetadata& metadata) {
    if (!is_open_) {
        return false;
    }

    std::vector<uint8_t> buf;
    WriteUint32LE(buf, metadata.version);
    WriteUint32LE(buf, metadata.header_size);
    WriteUint32LE(buf, static_cast<uint32_t>(metadata.network.size()));
    buf.insert(buf.end(), metadata.network.begin(), metadata.network.end());

    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Put(
        write_opts,
        MakeSchemaMetadataKey(),
        rocksdb::Slice(reinterpret_cast<const char*>(buf.data()), buf.size())
    );
    if (!status.ok()) {
        return false;
    }

    persisted_schema_metadata_ = metadata;
    return true;
}

bool HeaderStore::LoadSchemaMetadata(SchemaMetadata& metadata) const {
    if (!is_open_) {
        return false;
    }

    std::string value;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    const rocksdb::Status status = db_->Get(read_opts, MakeSchemaMetadataKey(), &value);
    if (!status.ok()) {
        return false;
    }

    if (value.size() < 12) {
        return false;
    }

    const auto* raw = reinterpret_cast<const uint8_t*>(value.data());
    size_t offset = 0;
    metadata.version = ReadUint32LE(raw, offset);
    metadata.header_size = ReadUint32LE(raw, offset);
    const uint32_t network_len = ReadUint32LE(raw, offset);
    if (offset + network_len != value.size()) {
        return false;
    }

    metadata.network.assign(value.data() + offset, network_len);
    return true;
}

bool HeaderStore::HasPersistedHeaderRecords() const {
    if (!is_open_) {
        return false;
    }

    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kDBIterator;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts));
    const std::string best_key = MakeBestHeaderKey();
    const std::string schema_key = MakeSchemaMetadataKey();

    for (it->Seek(HEADER_PREFIX); it->Valid(); it->Next()) {
        const std::string key = it->key().ToString();
        if (key.compare(0, HEADER_PREFIX.size(), HEADER_PREFIX) != 0) {
            break;
        }
        if (key != best_key && key != schema_key) {
            return true;
        }
    }

    return false;
}

bool HeaderStore::HasPersistedBestMarker() const {
    if (!is_open_) {
        return false;
    }

    std::string value;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    return db_->Get(read_opts, MakeBestHeaderKey(), &value).ok();
}

bool HeaderStore::HasPersistedSchemaKey() const {
    if (!is_open_) {
        return false;
    }

    std::string value;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    return db_->Get(read_opts, MakeSchemaMetadataKey(), &value).ok();
}

void HeaderStore::SetSchemaRecoveryRequired(const std::string& reason) {
    if (!schema_recovery_required_) {
        schema_recovery_required_ = true;
        schema_recovery_reason_ = reason;
    }
}

std::string HeaderStore::MakeSchemaMetadataKey() const {
    return HEADER_PREFIX + SCHEMA_METADATA_KEY;
}

} // namespace consensus
} // namespace dinero
