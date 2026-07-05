#include "storage/chain_db.h"
#include "common/serialization.h"
#include "common/json_adapter.h"
#include "consensus/undo.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "primitives/transaction.h"
#include "primitives/hash_domains.h"
#include "crypto/sha256.h"
#include <rocksdb/slice_transform.h>
#include <rocksdb/env.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// iOS: Disable RocksDB file locking.
//
// RocksDB keeps a process-global static set of locked DB paths.  On iOS the
// app is a single process, but lifecycle events (background → foreground,
// auto-reconnect) can destroy and recreate DaemonApp within the same process.
// If cleanup races or a partial Open() failure leaves the path in the set,
// every subsequent Open() fails with "lock hold by current process" — and
// removing the LOCK *file* doesn't clear the in-memory set.
//
// Safe because iOS guarantees single-process access to the app sandbox.
// ═══════════════════════════════════════════════════════════════════════════════
#if defined(__APPLE__) && TARGET_OS_IOS
namespace {

class NoLockEnv : public rocksdb::EnvWrapper {
public:
    explicit NoLockEnv(rocksdb::Env* base) : EnvWrapper(base) {}

    rocksdb::Status LockFile(const std::string& /*fname*/,
                             rocksdb::FileLock** lock) override {
        // Return a dummy lock — no actual file locking.
        *lock = reinterpret_cast<rocksdb::FileLock*>(new char);
        return rocksdb::Status::OK();
    }

    rocksdb::Status UnlockFile(rocksdb::FileLock* lock) override {
        delete reinterpret_cast<char*>(lock);
        return rocksdb::Status::OK();
    }
};

static rocksdb::Env* getNoLockEnv() {
    static NoLockEnv env(rocksdb::Env::Default());
    return &env;
}

} // anonymous namespace
#endif

// Cross-platform byte swap
#ifdef _MSC_VER
    #define bswap32(x) _byteswap_ulong(x)
#else
    #define bswap32(x) __builtin_bswap32(x)
#endif

namespace dinero {

namespace {

bool LooksLikePersistedHeaderMetadataValue(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    const auto version = static_cast<uint8_t>(value[0]);
    return version == 1 || version == ChainDB::PersistedHeaderMetadata::SCHEMA_VERSION;
}

StatusOr<ChainDB::PersistedHeaderMetadata> DeserializePersistedHeaderMetadataValue(
    const std::string& value
) {
    if (!LooksLikePersistedHeaderMetadataValue(value)) {
        return Status::NotFound;
    }

    try {
        Reader r(value);

        const uint8_t version = r.read<uint8_t>();
        if (version != 1 && version != ChainDB::PersistedHeaderMetadata::SCHEMA_VERSION) {
            return Status::Serialization;
        }

        ChainDB::PersistedHeaderMetadata metadata;
        const std::string parent_hash_hex = r.readString();
        if (parent_hash_hex.size() != 64 ||
            !std::all_of(parent_hash_hex.begin(), parent_hash_hex.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            })) {
            return Status::Serialization;
        }

        metadata.parent_hash = uint256::FromHexUnsafe(parent_hash_hex);
        metadata.height = r.read<int32_t>();
        metadata.chainwork = r.read<arith_uint256>();
        metadata.status_flags = r.read<uint32_t>();

        if (version >= 2) {
            metadata.file_number = r.read<uint32_t>();
            metadata.data_pos = r.read<uint32_t>();
            metadata.data_size = r.read<uint32_t>();
            metadata.undo_file = r.read<uint32_t>();
            metadata.undo_pos = r.read<uint32_t>();
            metadata.undo_size = r.read<uint32_t>();
        }

        if (!r.eof()) {
            return Status::Serialization;
        }

        return metadata;
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

std::string SerializePersistedHeaderMetadataValue(const ChainDB::PersistedHeaderMetadata& metadata) {
    VectorWriter w;
    w.write(ChainDB::PersistedHeaderMetadata::SCHEMA_VERSION);
    w.writeString(metadata.parent_hash.GetHex());
    w.write(static_cast<int32_t>(metadata.height));
    w.write(metadata.chainwork);
    w.write(metadata.status_flags);
    w.write(metadata.file_number);
    w.write(metadata.data_pos);
    w.write(metadata.data_size);
    w.write(metadata.undo_file);
    w.write(metadata.undo_pos);
    w.write(metadata.undo_size);
    return w.release_string();
}

} // namespace

Status ChainDB::init(const std::filesystem::path& dir) {
    close();  // Ensure clean state
    dir_ = dir;  // Remember path for lock recovery

    try {
        std::filesystem::create_directories(dir);
    } catch (const std::exception&) {
        return Status::Io;
    }

    return initAttempt(dir, /*allow_lock_recovery=*/true);
}

Status ChainDB::initAttempt(const std::filesystem::path& dir, bool allow_lock_recovery) {
    auto options = getDefaultOptions();
    auto cf_descriptors = getColumnFamilyDescriptors();

    // DEBUG: Log CF descriptors being used
    std::cerr << "ChainDB::init: Opening DB with " << cf_descriptors.size() << " CF descriptors:" << std::endl;
    for (const auto& cf_desc : cf_descriptors) {
        std::cerr << "  - " << cf_desc.name << std::endl;
    }

    std::vector<rocksdb::ColumnFamilyHandle*> raw_handles;
    rocksdb::DB* db_raw = nullptr;

    // Phase 34.1: Try to open with all CFs, create missing ones if needed
    auto status = rocksdb::DB::Open(options, dir.string(), cf_descriptors, &raw_handles, &db_raw);

    // If DB doesn't exist or CFs are missing, list existing CFs and auto-migrate
    if (!status.ok()) {
        std::cerr << "ChainDB::init: Initial open failed: " << status.ToString() << std::endl;

        // Detect lock conflicts and retry once without mutating lock files.
        // Deleting LOCK can corrupt safety guarantees when another process
        // legitimately owns the database.
        if (allow_lock_recovery && status.ToString().find("lock") != std::string::npos) {
            std::cerr << "ChainDB::init: Lock conflict detected, retrying once without lock-file deletion" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return initAttempt(dir, /*allow_lock_recovery=*/false);
        }

        // Try to list existing column families
        std::vector<std::string> existing_cf_names;
        auto list_status = rocksdb::DB::ListColumnFamilies(options, dir.string(), &existing_cf_names);

        if (list_status.ok() && !existing_cf_names.empty()) {
            std::cerr << "ChainDB::init: Found " << existing_cf_names.size() << " existing CFs:" << std::endl;
            for (const auto& name : existing_cf_names) {
                std::cerr << "  - " << name << std::endl;
            }

            // Build descriptors for existing CFs
            std::vector<rocksdb::ColumnFamilyDescriptor> existing_descriptors;
            for (const auto& cf_name : existing_cf_names) {
                existing_descriptors.emplace_back(cf_name, options);
            }

            std::cerr << "ChainDB::init: Attempting to open with existing CFs..." << std::endl;
            status = rocksdb::DB::Open(options, dir.string(), existing_descriptors, &raw_handles, &db_raw);

            if (!status.ok()) {
                std::cerr << "ChainDB::init: Failed to open with existing CFs: " << status.ToString() << std::endl;

                // Same lock recovery for the fallback open path.
                if (allow_lock_recovery && status.ToString().find("lock") != std::string::npos) {
                    std::cerr << "ChainDB::init: Lock conflict detected on fallback open, retrying once" << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return initAttempt(dir, /*allow_lock_recovery=*/false);
                }
                return convertRocksDBStatus(status);
            }

            std::cerr << "ChainDB::init: Successfully opened DB with existing CFs" << std::endl;
        } else {
            std::cerr << "ChainDB::init: DB does not exist or is corrupted: " << list_status.ToString() << std::endl;
            return convertRocksDBStatus(status);
        }
    }
    
    db_.reset(db_raw);
    
    // Wrap raw handles in RAII containers
    cf_.clear();
    cf_.reserve(raw_handles.size());
    for (auto* h : raw_handles) {
        cf_.emplace_back(CfUPtr(h));
    }

    // Phase 34.1: Verify CF initialization
    std::cerr << "ChainDB::init: Initialized " << cf_.size() << " column families" << std::endl;
    for (size_t i = 0; i < cf_.size(); ++i) {
        std::cerr << "  CF[" << i << "]: " << (cf_[i] ? "valid" : "null") << std::endl;
    }

    if (cf_.size() < 8) {
        std::cerr << "ChainDB::init: ERROR - Expected at least 8 CFs, got " << cf_.size() << std::endl;
        return Status::Internal;
    }
    
    // Initialize schema version if not exists
    int schema_version;
    if (getSchemaVersion(schema_version) != Status::Ok) {
        // Bootstrap token - ONLY used during init()
        ChainWriteToken token;
        rocksdb::WriteBatch batch;
        auto init_status = setSchemaVersion(token, 4, &batch);
        if (init_status != Status::Ok) {
            return init_status;
        }
        return writeBatch(token, std::move(batch), true);
    }

    return Status::Ok;
}

void ChainDB::close() {
    // 1) Ensure no snapshots/iterators remain in any scope here
    // 2) Destroy CF handles FIRST:
    cf_.clear();
    // 3) Then destroy DB:
    db_.reset();
}

Status ChainDB::putBlock(const ChainWriteToken& token, const uint256& hash, const Block& block, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeBlockKey(hash);
    auto value = SerializeToString(block);

    if (wb) {
        wb->Put(cf_[idx_blocks_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_blocks_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

Status ChainDB::deleteBlock(const ChainWriteToken& token, const uint256& hash, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeBlockKey(hash);

    if (wb) {
        wb->Delete(cf_[idx_blocks_].get(), key);
        return Status::Ok;
    } else {
        auto status = db_->Delete(rocksdb::WriteOptions(), cf_[idx_blocks_].get(), key);
        return convertRocksDBStatus(status);
    }
}

StatusOr<Block> ChainDB::getBlock(const uint256& hash) const {
    if (!db_) return Status::Internal;
    
    auto key = makeBlockKey(hash);
    std::string value;
    
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_blocks_].get(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }
    
    Block block;
    auto parse_status = ParseFrom(value, block);
    if (parse_status != Status::Ok) {
        return parse_status;
    }
    
    return std::move(block);
}

Status ChainDB::hasBlock(const uint256& hash) const {
    if (!db_) return Status::Internal;

    auto key = makeBlockKey(hash);
    std::string value;

    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_blocks_].get(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    return convertRocksDBStatus(status);
}

StatusOr<UndoRecord> ChainDB::getUndo(const uint256& hash) const {
    if (!db_) return Status::Internal;

    // Undo records are stored with key "U:<blockhash>" in default column family
    std::string key = "U:" + hash.GetHex();
    std::string value;

    auto status = getRaw(key, value);
    if (status != Status::Ok) {
        return status;
    }

    // Convert string to vector<uint8_t> for deserialization
    std::vector<uint8_t> bytes(value.begin(), value.end());

    try {
        UndoRecord undo = UndoRecord::Deserialize(bytes);
        return std::move(undo);
    } catch (const std::exception& e) {
        return Status::Serialization;
    }
}

Status ChainDB::putUndo(const ChainWriteToken& token, const uint256& hash, const UndoRecord& undo, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    // Undo records are stored with key "U:<blockhash>" in default column family
    std::string key = "U:" + hash.GetHex();

    // Serialize undo record
    std::vector<uint8_t> bytes = undo.Serialize();
    std::string value(bytes.begin(), bytes.end());

    // Write to database
    if (wb) {
        // Add to batch for atomic commit
        wb->Put(key, value);
        return Status::Ok;
    } else {
        // Direct write
        auto status = db_->Put(rocksdb::WriteOptions(), key, value);
        return convertRocksDBStatus(status);
    }
}

Status ChainDB::putHeader(const ChainWriteToken& token, const uint256& hash, const BlockHeader& header, int height, arith_uint256 work, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeHeaderKey(hash);

    // Serialize header + height + work
    VectorWriter w;
    Serialize(w, header);
    w.write(static_cast<uint32_t>(height));
    w.write(work);

    auto value = w.release_string();

    if (wb) {
        wb->Put(cf_[idx_headers_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_headers_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<BlockHeader> ChainDB::getHeader(const uint256& hash) const {
    if (!db_) return Status::Internal;
    
    auto key = makeHeaderKey(hash);
    std::string value;
    
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_headers_].get(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }
    
    try {
        Reader r(value);
        BlockHeader header;
        Deserialize(r, header);
        // Skip height and work for now
        return std::move(header);
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

StatusOr<int> ChainDB::getBlockHeight(const uint256& hash) const {
    if (!db_) return Status::Internal;

    auto key = makeHeaderKey(hash);
    std::string value;

    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_headers_].get(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    try {
        Reader r(value);
        BlockHeader header;
        Deserialize(r, header);
        // Read height after header
        int height = r.read<uint32_t>();
        return height;
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

StatusOr<arith_uint256> ChainDB::getBlockWork(const uint256& hash) const {
    if (!db_) return Status::Internal;

    auto key = makeHeaderKey(hash);
    std::string value;

    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_headers_].get(), key, &value);
    if (status.IsNotFound()) return Status::NotFound;
    if (!status.ok()) return convertRocksDBStatus(status);

    try {
        Reader r(value);
        BlockHeader header;
        Deserialize(r, header);
        r.read<uint32_t>();  // skip height
        return r.read<arith_uint256>();
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase H.3: Minimal Header Metadata Persistence (Restart Recovery)
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::putHeaderMetadata(const ChainWriteToken& token, const uint256& hash,
                                  const PersistedHeaderMetadata& metadata, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization
    (void)token;

    auto key = makeHeaderMetadataKey(hash);
    auto value = SerializePersistedHeaderMetadataValue(metadata);

    if (wb) {
        wb->Put(cf_[idx_headers_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_headers_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

Status ChainDB::putHeaderMetadataPreservingExistingUndo(
    const ChainWriteToken& token,
    const uint256& hash,
    const PersistedHeaderMetadata& metadata,
    rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization
    (void)token;

    PersistedHeaderMetadata merged = metadata;
    auto existing = getHeaderMetadata(hash);
    if (existing.ok()) {
        const auto& prev = existing.value();

        // Do not let a partial relay/side-chain metadata writer regress
        // already-durable status bits. The motivating production failure was:
        // ConnectTip wrote BLOCK_HAVE_UNDO, then a duplicate block was processed
        // as "side-chain" and putHeaderMetadata overwrote the row with status
        // 159 + undo_size=0.
        const bool incoming_has_undo =
            (metadata.status_flags & BLOCK_HAVE_UNDO) != 0 && metadata.undo_size > 0;
        merged.status_flags |= prev.status_flags;

        const bool existing_has_undo =
            (prev.status_flags & BLOCK_HAVE_UNDO) != 0 && prev.undo_size > 0;
        if (!incoming_has_undo && existing_has_undo) {
            merged.status_flags |= BLOCK_HAVE_UNDO;
            merged.undo_file = prev.undo_file;
            merged.undo_pos = prev.undo_pos;
            merged.undo_size = prev.undo_size;
        }
    } else if (existing.status() != Status::NotFound) {
        return existing.status();
    }

    return putHeaderMetadata(token, hash, merged, wb);
}

Status ChainDB::updateHeaderStatus(const ChainWriteToken& token, const uint256& hash,
                                   uint32_t status_flags, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization
    (void)token;

    // Read existing metadata
    auto result = getHeaderMetadata(hash);
    if (!result.ok()) {
        return result.status();
    }

    // Update status flags only. NOTE: this OVERWRITES status_flags wholesale.
    // Callers that only know about a subset of consensus-layer bits MUST
    // use setHeaderStatusBits instead. See chain_db.h for the May 2026
    // HAVE_UNDO-stripping incident that drove this comment.
    PersistedHeaderMetadata metadata = result.value();
    metadata.status_flags = status_flags;

    // Write back
    return putHeaderMetadata(token, hash, metadata, wb);
}

Status ChainDB::setHeaderStatusBits(const ChainWriteToken& token, const uint256& hash,
                                    uint32_t bits_to_set, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization
    (void)token;

    // Read existing metadata. If absent, there's nothing to OR-merge into,
    // and silently inventing a metadata entry from partial bits would be
    // worse than the not-found error (we'd lose parent_hash, height,
    // chainwork, etc.). Caller decides what to do (typically: log + drop).
    auto result = getHeaderMetadata(hash);
    if (!result.ok()) {
        return result.status();
    }

    PersistedHeaderMetadata metadata = result.value();
    const uint32_t before = metadata.status_flags;
    metadata.status_flags |= bits_to_set;
    if (metadata.status_flags == before) {
        // Nothing changed; skip the write to avoid burning fsync budget.
        return Status::Ok;
    }

    return putHeaderMetadata(token, hash, metadata, wb);
}

Status ChainDB::clearHeaderStatusBits(const ChainWriteToken& token, const uint256& hash,
                                      uint32_t bits_to_clear, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization
    (void)token;

    auto result = getHeaderMetadata(hash);
    if (!result.ok()) {
        return result.status();
    }

    PersistedHeaderMetadata metadata = result.value();
    const uint32_t before = metadata.status_flags;
    metadata.status_flags &= ~bits_to_clear;
    if (metadata.status_flags == before) {
        return Status::Ok;
    }

    return putHeaderMetadata(token, hash, metadata, wb);
}

Status ChainDB::updateUndoLocator(const ChainWriteToken& token,
                                  const uint256& hash,
                                  uint32_t undo_file,
                                  uint32_t undo_pos,
                                  uint32_t undo_size,
                                  rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    if (undo_size == 0) return Status::Invalid;

    // token validates authorization
    (void)token;

    auto result = getHeaderMetadata(hash);
    if (!result.ok()) {
        return result.status();
    }

    PersistedHeaderMetadata metadata = result.value();
    metadata.undo_file = undo_file;
    metadata.undo_pos = undo_pos;
    metadata.undo_size = undo_size;
    metadata.status_flags |= BLOCK_HAVE_UNDO;

    return putHeaderMetadata(token, hash, metadata, wb);
}

StatusOr<ChainDB::PersistedHeaderMetadata> ChainDB::getHeaderMetadata(const uint256& hash) const {
    if (!db_) return Status::Internal;

    auto key = makeHeaderMetadataKey(hash);
    std::string value;

    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_headers_].get(), key, &value);
    if (status.IsNotFound()) {
        std::string legacy_value;
        auto legacy_status = db_->Get(rocksdb::ReadOptions(),
                                      cf_[idx_headers_].get(),
                                      makeHeaderKey(hash),
                                      &legacy_value);
        if (legacy_status.IsNotFound()) {
            return Status::NotFound;
        }
        if (!legacy_status.ok()) {
            return convertRocksDBStatus(legacy_status);
        }

        auto legacy_result = DeserializePersistedHeaderMetadataValue(legacy_value);
        if (legacy_result.status() == Status::NotFound) {
            return Status::NotFound;
        }
        return legacy_result;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    return DeserializePersistedHeaderMetadataValue(value);
}

Status ChainDB::forEachHeaderMetadata(std::function<bool(const uint256& hash, const PersistedHeaderMetadata& metadata)> callback) const {
    if (!db_) return Status::Internal;

    auto it = db_->NewIterator(rocksdb::ReadOptions(), cf_[idx_headers_].get());
    if (!it) {
        return Status::Internal;
    }

    std::unordered_set<std::string> seen_hashes;
    bool keep_iterating = true;

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        try {
            std::string key_str = it->key().ToString();
            if (key_str.size() < 1 || key_str[0] != PREFIX_HEADER_META) {
                continue;
            }
            const std::string hash_hex = key_str.substr(1);
            auto metadata_result = DeserializePersistedHeaderMetadataValue(it->value().ToString());
            if (!metadata_result.ok()) {
                continue;
            }

            seen_hashes.insert(hash_hex);
            if (!callback(uint256::FromHexUnsafe(hash_hex), metadata_result.value())) {
                keep_iterating = false;
                break;
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    for (it->SeekToFirst(); keep_iterating && it->Valid(); it->Next()) {
        try {
            std::string key_str = it->key().ToString();
            if (key_str.size() < 1 || key_str[0] != PREFIX_HEADER) {
                continue;
            }

            const std::string hash_hex = key_str.substr(1);
            if (seen_hashes.find(hash_hex) != seen_hashes.end()) {
                continue;
            }

            auto metadata_result = DeserializePersistedHeaderMetadataValue(it->value().ToString());
            if (!metadata_result.ok()) {
                continue;
            }

            seen_hashes.insert(hash_hex);
            if (!callback(uint256::FromHexUnsafe(hash_hex), metadata_result.value())) {
                keep_iterating = false;
                break;
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    delete it;
    return Status::Ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase P.2: CBlockIndex Persistence (Disk Positions + Status)
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::updateBlockIndex(const ChainWriteToken& token, const CBlockIndex* pindex, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    if (!pindex) return Status::Invalid;

    // token validates authorization
    (void)token;

    // Populate metadata from CBlockIndex
    PersistedHeaderMetadata metadata;
    metadata.parent_hash = pindex->prev_hash;
    metadata.height = static_cast<int32_t>(pindex->height);

    // Convert chainwork string to arith_uint256
    metadata.chainwork = ChainworkFromHex(pindex->chainwork);

    metadata.status_flags = pindex->status;

    // Phase P.2: Disk positions from CBlockIndex
    metadata.file_number = pindex->file_number;
    metadata.data_pos = pindex->data_pos;
    metadata.data_size = pindex->data_size;
    metadata.undo_file = pindex->undo_file;
    metadata.undo_pos = pindex->undo_pos;
    metadata.undo_size = pindex->undo_size;

    // Write to database
    return putHeaderMetadata(token, pindex->hash, metadata, wb);
}

Status ChainDB::setTip(const ChainWriteToken& token, const uint256& hash, int height, arith_uint256 work, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    // Serialize tip info
    VectorWriter w;
    w.writeString(hash.GetHex());
    w.write(static_cast<uint32_t>(height));
    w.write(work);
    w.write(static_cast<uint32_t>(std::time(nullptr))); // timestamp

    auto value = w.release_string();

    if (wb) {
        wb->Put(cf_[idx_meta_].get(), KEY_TIP, value);
        return Status::Ok;
    } else {
        // Phase E.1.c: CRITICAL - Tip updates MUST use sync=true
        // Without sync, tip can be lost on power failure → corrupted chain state
        rocksdb::WriteOptions opts;
        opts.sync = true;  // Force fsync to ensure tip is durably written
        auto status = db_->Put(opts, cf_[idx_meta_].get(), KEY_TIP, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<TipInfo> ChainDB::getTip() const {
    if (!db_) return Status::Internal;

    std::string value;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    auto status = db_->Get(read_opts, cf_[idx_meta_].get(), KEY_TIP, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    try {
        Reader r(value);
        TipInfo tip;
        tip.hash = uint256::FromHexUnsafe(r.readString());
        tip.height = r.read<uint32_t>();
        tip.work = r.read<arith_uint256>();
        tip.timestamp = r.read<uint32_t>();
        return std::move(tip);
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Validated Tip Marker (ConnectTip completion marker)
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::setValidatedTip(const ChainWriteToken& token, const uint256& hash, int height, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    VectorWriter w;
    w.writeString(hash.GetHex());
    w.write(static_cast<uint32_t>(height));

    auto value = w.release_string();

    if (wb) {
        wb->Put(cf_[idx_meta_].get(), KEY_VALIDATED_TIP, value);
        return Status::Ok;
    } else {
        rocksdb::WriteOptions opts;
        opts.sync = true;  // Must be durable — controls startup replay
        auto status = db_->Put(opts, cf_[idx_meta_].get(), KEY_VALIDATED_TIP, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<TipInfo> ChainDB::getValidatedTip() const {
    if (!db_) return Status::Internal;

    std::string value;
    rocksdb::ReadOptions read_opts;
    auto status = db_->Get(read_opts, cf_[idx_meta_].get(), KEY_VALIDATED_TIP, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    try {
        Reader r(value);
        TipInfo tip;
        tip.hash = uint256::FromHexUnsafe(r.readString());
        tip.height = r.read<uint32_t>();
        // validated tip doesn't store work/timestamp, zero them
        tip.work = arith_uint256();
        tip.timestamp = 0;
        return std::move(tip);
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase P.2: Prune Height Persistence
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::setPruneHeight(const ChainWriteToken& token, uint32_t height, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    // Serialize prune height (simple uint32_t)
    VectorWriter w;
    w.write(height);
    auto value = w.release_string();

    if (wb) {
        wb->Put(cf_[idx_meta_].get(), KEY_PRUNE_HEIGHT, value);
        return Status::Ok;
    } else {
        // Prune height updates should be sync'd for safety
        rocksdb::WriteOptions opts;
        opts.sync = true;
        auto status = db_->Put(opts, cf_[idx_meta_].get(), KEY_PRUNE_HEIGHT, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<uint32_t> ChainDB::getPruneHeight() const {
    if (!db_) return Status::Internal;

    std::string value;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    auto status = db_->Get(read_opts, cf_[idx_meta_].get(), KEY_PRUNE_HEIGHT, &value);

    if (status.IsNotFound()) {
        // No pruning performed yet - all blocks available from genesis
        return 0u;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    try {
        Reader r(value);
        return r.read<uint32_t>();
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

Status ChainDB::setPruneMode(const ChainWriteToken& token, bool enabled, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    // Serialize prune mode (1 byte: 0=archival, 1=pruned)
    VectorWriter w;
    w.write(static_cast<uint8_t>(enabled ? 1 : 0));
    auto value = w.release_string();

    if (wb) {
        wb->Put(cf_[idx_meta_].get(), KEY_PRUNE_MODE, value);
        return Status::Ok;
    } else {
        // Prune mode is critical - sync write
        rocksdb::WriteOptions opts;
        opts.sync = true;
        auto status = db_->Put(opts, cf_[idx_meta_].get(), KEY_PRUNE_MODE, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<bool> ChainDB::getPruneMode() const {
    if (!db_) return Status::Internal;

    std::string value;
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    auto status = db_->Get(read_opts, cf_[idx_meta_].get(), KEY_PRUNE_MODE, &value);

    if (status.IsNotFound()) {
        // Mode never set - return NotFound to indicate uninitialized
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    try {
        Reader r(value);
        return r.read<uint8_t>() != 0;
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

Status ChainDB::putHeightIndex(const ChainWriteToken& token, int height, const uint256& hash, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeHeightKey(height);

    if (wb) {
        wb->Put(cf_[idx_height_].get(), key, hash.GetHex());
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_height_].get(), key, hash.GetHex());
        return convertRocksDBStatus(status);
    }
}

Status ChainDB::deleteHeightIndex(const ChainWriteToken& token, int height, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    (void)token;

    auto key = makeHeightKey(height);

    if (wb) {
        wb->Delete(cf_[idx_height_].get(), key);
        return Status::Ok;
    } else {
        auto status = db_->Delete(rocksdb::WriteOptions(), cf_[idx_height_].get(), key);
        return convertRocksDBStatus(status);
    }
}

StatusOr<uint256> ChainDB::getBlockHashByHeight(int height) const {
    if (!db_) {
        return Status::Internal;
    }

    auto key = makeHeightKey(height);
    std::string value;

    if (idx_height_ >= static_cast<int>(cf_.size())) {
        return Status::Invalid;
    }

    if (!cf_[idx_height_]) {
        return Status::Invalid;
    }

    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kGet;
    auto status = db_->Get(read_opts, cf_[idx_height_].get(), key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    return uint256::FromHexUnsafe(value); // Hash is stored as hex string
}

// Transaction index operations
Status ChainDB::putTxIndex(const ChainWriteToken& token, const uint256& txid, const uint256& block_hash, uint32_t offset, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeTxIndexKey(txid);

    // Store block_hash + offset as value (block_hash is 64 hex chars, offset is 4 bytes)
    std::string value = block_hash.GetHex();
    value.append(reinterpret_cast<const char*>(&offset), sizeof(offset));

    if (wb) {
        wb->Put(cf_[idx_txindex_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_txindex_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<std::pair<uint256, uint32_t>> ChainDB::getTxLocation(const uint256& txid) const {
    if (!db_) return Status::Internal;

    auto key = makeTxIndexKey(txid);
    std::string value;

    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_txindex_].get(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    // Parse block_hash (first 64 hex chars) + offset (last 4 bytes)
    if (value.size() < sizeof(uint32_t)) {
        return Status::Internal; // Invalid data
    }

    std::string block_hash_str = value.substr(0, value.size() - sizeof(uint32_t));
    uint32_t offset;
    std::memcpy(&offset, value.data() + value.size() - sizeof(uint32_t), sizeof(uint32_t));

    return std::make_pair(uint256::FromHexUnsafe(block_hash_str), offset);
}

Status ChainDB::deleteTxIndex(const ChainWriteToken& token, const uint256& txid, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeTxIndexKey(txid);

    if (wb) {
        wb->Delete(cf_[idx_txindex_].get(), key);
        return Status::Ok;
    } else {
        auto status = db_->Delete(rocksdb::WriteOptions(), cf_[idx_txindex_].get(), key);
        return convertRocksDBStatus(status);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 7: Lightning Transaction Queries (Read-Only)
// ═══════════════════════════════════════════════════════════════════════════

StatusOr<uint64_t> ChainDB::getTransactionHeight(const uint256& txid) const {
    if (!db_) return Status::Internal;

    // Use existing tx index to find block containing this transaction
    auto location_result = getTxLocation(txid);
    if (!location_result.ok()) {
        return Status::NotFound;
    }

    auto [block_hash, tx_offset] = location_result.value();

    // Get the height of the block containing this transaction
    auto height_result = getBlockHeight(block_hash);
    if (!height_result.ok()) {
        return Status::NotFound;
    }

    return static_cast<uint64_t>(height_result.value());
}

StatusOr<Transaction> ChainDB::getTransaction(const uint256& txid) const {
    if (!db_) return Status::Internal;

    // First verify transaction is confirmed (reorg-safe)
    auto height_result = getTransactionHeight(txid);
    if (!height_result.ok()) {
        return Status::NotFound;
    }

    // Get transaction location (block_hash + tx_offset)
    auto location_result = getTxLocation(txid);
    if (!location_result.ok()) {
        return Status::NotFound;
    }

    auto [block_hash, tx_offset] = location_result.value();

    // Fetch the block containing the transaction
    auto block_result = getBlock(block_hash);
    if (!block_result.ok()) {
        return Status::NotFound;
    }

    const Block& block = block_result.value();

    // Linear scan through block transactions to find matching txid
    // This is acceptable - justice/sweep transactions are rare, time-critical events
    for (const auto& tx : block.vtx) {
        TxId computed_txid = TxId::Compute(tx);
        if (computed_txid.AsUint256() == txid) {
            return tx;
        }
    }

    // Transaction index inconsistency - tx index points to block but tx not found
    return Status::NotFound;
}

Status ChainDB::flushForTesting() {
    if (!db_) return Status::Internal;
    return convertRocksDBStatus(db_->Flush(rocksdb::FlushOptions()));
}

Status ChainDB::writeBatch(const ChainWriteToken& token, rocksdb::WriteBatch&& batch, bool sync) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    rocksdb::WriteOptions options;
    options.sync = sync;

    auto status = db_->Write(options, &batch);
    if (status.ok()) {
        consecutive_write_failures_.store(0);
        return Status::Ok;
    }

    // #371: rocksdb latches background errors (e.g. one EBADF during an sst
    // flush) and then fails EVERY subsequent Write() with the saved status —
    // silently, since it logs only once at latch time. Surface the reason,
    // then attempt in-process recovery:
    //  - Resume() clears a manually-recoverable latch (hard errors whose
    //    cause has passed);
    //  - Resume()==Busy means rocksdb's own auto-recovery is running (e.g.
    //    NoSpace via SstFileManager free-space polling) — ride it out for a
    //    bounded window and retry the (unconsumed) batch;
    //  - a fatal latch (the EU1 sst-flush class) is refused by Resume()
    //    immediately → fall through to the loud-failure escalation below.
    std::cerr << "[ChainDB] writeBatch failed: " << status.ToString() << std::endl;
    constexpr int kRecoveryAttempts = 50;  // x100ms = 5s bounded wait
    for (int attempt = 0; attempt < kRecoveryAttempts; ++attempt) {
        const auto resume_status = db_->Resume();
        if (!resume_status.ok() && !resume_status.IsBusy()) {
            std::cerr << "[ChainDB] Resume() cannot recover latched error: "
                      << resume_status.ToString() << std::endl;
            break;
        }
        status = db_->Write(options, &batch);
        if (status.ok()) {
            std::cerr << "[ChainDB] writeBatch recovered after Resume()/recovery wait "
                      << "(attempt " << (attempt + 1) << ")" << std::endl;
            consecutive_write_failures_.store(0);
            return Status::Ok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Unrecovered failure: a node that cannot persist blocks must fail loudly
    // (systemd / fleet watchdog restarts it — a restart is a proven full
    // recovery for this class), never zombie at a frozen tip (EU1 2026-07-04).
    const int failures = consecutive_write_failures_.fetch_add(1) + 1;
    if (failures == kMaxConsecutiveWriteFailures) {
        const std::string reason =
            "ChainDB writeBatch failed " + std::to_string(failures) +
            " consecutive times, Resume() cannot recover: " + status.ToString();
        if (fatal_write_failure_hook_) {
            fatal_write_failure_hook_(reason);
        } else {
            std::cerr << "[ChainDB] FATAL: " << reason
                      << " — exiting so the service manager restarts the node"
                      << std::endl;
            std::exit(1);
        }
    }
    return convertRocksDBStatus(status);
}

Status ChainDB::getRaw(const std::string& key, std::string& value) const {
    if (!db_) return Status::Internal;

    // Use default column family for raw key-value access (undo data)
    auto status = db_->Get(rocksdb::ReadOptions(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    return convertRocksDBStatus(status);
}

// BIP158 GCS block filter operations
Status ChainDB::putBlockFilter(const ChainWriteToken& token, const uint256& hash,
                                const std::vector<uint8_t>& filter_data, uint32_t element_count,
                                rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    auto key = makeBlockFilterKey(hash);

    // Value: element_count (4 bytes LE) + filter_data
    std::string value;
    value.reserve(4 + filter_data.size());
    value.append(reinterpret_cast<const char*>(&element_count), 4);
    value.append(reinterpret_cast<const char*>(filter_data.data()), filter_data.size());

    if (wb) {
        wb->Put(cf_[idx_blocks_].get(), key, value);
        return Status::Ok;
    }
    auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_blocks_].get(), key, value);
    return convertRocksDBStatus(status);
}

Status ChainDB::putBlockFilter(const uint256& hash,
                                const std::vector<uint8_t>& filter_data, uint32_t element_count) {
    if (!db_) return Status::Internal;

    auto key = makeBlockFilterKey(hash);

    std::string value;
    value.reserve(4 + filter_data.size());
    value.append(reinterpret_cast<const char*>(&element_count), 4);
    value.append(reinterpret_cast<const char*>(filter_data.data()), filter_data.size());

    auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_blocks_].get(), key, value);
    return convertRocksDBStatus(status);
}

StatusOr<ChainDB::StoredFilter> ChainDB::getBlockFilter(const uint256& hash) const {
    if (!db_) return Status::Internal;

    auto key = makeBlockFilterKey(hash);
    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_blocks_].get(), key, &value);
    if (status.IsNotFound()) return Status::NotFound;
    if (!status.ok()) return convertRocksDBStatus(status);

    if (value.size() < 4) return Status::Internal;

    StoredFilter result;
    std::memcpy(&result.element_count, value.data(), 4);
    result.data.assign(value.begin() + 4, value.end());
    return result;
}

std::string ChainDB::makeBlockFilterKey(const uint256& hash) const {
    std::string key;
    key.reserve(1 + 64);
    key.push_back(PREFIX_BLOCK_FILTER);
    key.append(hash.GetHex());
    return key;
}

// UTXO operations
Status ChainDB::putCoin(const ChainWriteToken& token, const uint256& txid, uint32_t vout, const Coin& coin, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeUtxoKey(txid, vout);

    // Serialize Coin
    VectorWriter w;
    w.write(coin.amount);
    w.writeString(coin.script_pubkey);
    w.write(static_cast<uint32_t>(coin.height));
    w.write(static_cast<uint8_t>(coin.coinbase ? 1 : 0));
    w.write(static_cast<uint8_t>(coin.is_confidential ? 1 : 0));
    w.writeBytes(coin.commitment);
    auto value = w.release_string();

    if (wb) {
        wb->Put(cf_[idx_utxo_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_utxo_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<Coin> ChainDB::getCoin(const uint256& txid, uint32_t vout) const {
    if (!db_) return Status::Internal;

    auto key = makeUtxoKey(txid, vout);
    std::string value;

    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_utxo_].get(), key, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    try {
        Reader r(value);
        Coin coin;
        coin.amount = r.read<uint64_t>();
        coin.script_pubkey = r.readString();
        coin.height = r.read<uint32_t>();
        coin.coinbase = (r.read<uint8_t>() != 0);
        if (!r.eof()) {
            coin.is_confidential = (r.read<uint8_t>() != 0);
            if (!r.eof()) {
                coin.commitment = r.readBytes();
            }
        }
        return std::move(coin);
    } catch (const std::exception&) {
        return Status::Serialization;
    }
}

StatusOr<Coin> ChainDB::getCoinWithConfidentialFallback(const uint256& txid, uint32_t vout) const {
    auto coin_result = getCoin(txid, vout);
    if (coin_result.status() != Status::Ok) {
        return coin_result;
    }

    Coin coin = coin_result.value();
    if (coin.is_confidential && !coin.commitment.empty()) {
        return coin;
    }

    auto tx_result = getTransaction(txid);
    if (tx_result.status() != Status::Ok) {
        return coin;
    }

    const Transaction& tx = tx_result.value();
    if (vout >= tx.vout.size()) {
        return coin;
    }

    const auto& output = tx.vout[vout];
    if (output.is_confidential && !output.commitment.empty()) {
        coin.is_confidential = true;
        coin.commitment = output.commitment;
    }

    return coin;
}

Status ChainDB::deleteCoin(const ChainWriteToken& token, const uint256& txid, uint32_t vout, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeUtxoKey(txid, vout);

    if (wb) {
        wb->Delete(cf_[idx_utxo_].get(), key);
        return Status::Ok;
    } else {
        auto status = db_->Delete(rocksdb::WriteOptions(), cf_[idx_utxo_].get(), key);
        return convertRocksDBStatus(status);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Utreexo Accumulator Persistence (Phase 2.1)
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::putUtreexoCheckpoint(const ChainWriteToken& token, int height,
                                     const std::vector<uint8_t>& serialized_forest,
                                     rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    // Key: PREFIX_UTREEXO_CHECKPOINT + height (4 bytes, big-endian for lexicographic ordering)
    std::string key;
    key.reserve(5);
    key.push_back(PREFIX_UTREEXO_CHECKPOINT);
    key.push_back((height >> 24) & 0xFF);
    key.push_back((height >> 16) & 0xFF);
    key.push_back((height >> 8) & 0xFF);
    key.push_back(height & 0xFF);

    // Value: serialized forest data (already serialized by UtreexoForest::serialize())
    std::string value(serialized_forest.begin(), serialized_forest.end());

    if (wb) {
        wb->Put(cf_[idx_utreexo_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_utreexo_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<std::vector<uint8_t>> ChainDB::getUtreexoCheckpoint(int height) const {
    if (!db_) return Status::Internal;

    // Key: PREFIX_UTREEXO_CHECKPOINT + height (4 bytes, big-endian)
    std::string key;
    key.reserve(5);
    key.push_back(PREFIX_UTREEXO_CHECKPOINT);
    key.push_back((height >> 24) & 0xFF);
    key.push_back((height >> 16) & 0xFF);
    key.push_back((height >> 8) & 0xFF);
    key.push_back(height & 0xFF);

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_utreexo_].get(), key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    // Convert string to vector<uint8_t>
    return std::vector<uint8_t>(value.begin(), value.end());
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 3: Transition Proof Persistence
// ═════════════════════════════════════════════════════════════════════════════

Status ChainDB::putTransitionProof(const ChainWriteToken& token, int height,
                                    const std::vector<uint8_t>& serialized_proof,
                                    rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Key: PREFIX_TRANSITION_PROOF + height (4 bytes, big-endian)
    std::string key;
    key.reserve(5);
    key.push_back(PREFIX_TRANSITION_PROOF);
    key.push_back((height >> 24) & 0xFF);
    key.push_back((height >> 16) & 0xFF);
    key.push_back((height >> 8) & 0xFF);
    key.push_back(height & 0xFF);

    std::string value(serialized_proof.begin(), serialized_proof.end());

    if (wb) {
        wb->Put(cf_[idx_utreexo_].get(), key, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_utreexo_].get(), key, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<std::vector<uint8_t>> ChainDB::getTransitionProof(int height) const {
    if (!db_) return Status::Internal;

    // Key: PREFIX_TRANSITION_PROOF + height (4 bytes, big-endian)
    std::string key;
    key.reserve(5);
    key.push_back(PREFIX_TRANSITION_PROOF);
    key.push_back((height >> 24) & 0xFF);
    key.push_back((height >> 16) & 0xFF);
    key.push_back((height >> 8) & 0xFF);
    key.push_back(height & 0xFF);

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_utreexo_].get(), key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    return std::vector<uint8_t>(value.begin(), value.end());
}

StatusOr<std::pair<int, std::vector<uint8_t>>> ChainDB::getLatestUtreexoCheckpoint() const {
    if (!db_) return Status::Internal;

    // Iterate backwards from highest height to find latest checkpoint
    // Use reverse iterator starting from PREFIX_UTREEXO_CHECKPOINT + 0xFFFFFFFF
    std::string start_key;
    start_key.reserve(5);
    start_key.push_back(PREFIX_UTREEXO_CHECKPOINT);
    start_key.push_back(0xFF);
    start_key.push_back(0xFF);
    start_key.push_back(0xFF);
    start_key.push_back(0xFF);

    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    // Seek to end of utreexo checkpoint range
    it->SeekForPrev(start_key);

    if (!it->Valid()) {
        return Status::NotFound;
    }

    // Check if key starts with PREFIX_UTREEXO_CHECKPOINT
    auto key = it->key();
    if (key.size() != 5 || key[0] != PREFIX_UTREEXO_CHECKPOINT) {
        return Status::NotFound;
    }

    // Decode height from key (big-endian)
    int height = (static_cast<uint8_t>(key[1]) << 24) |
                 (static_cast<uint8_t>(key[2]) << 16) |
                 (static_cast<uint8_t>(key[3]) << 8) |
                 static_cast<uint8_t>(key[4]);

    // Get value
    auto value = it->value();
    std::vector<uint8_t> serialized_forest(value.data(), value.data() + value.size());

    return std::make_pair(height, std::move(serialized_forest));
}

StatusOr<std::vector<int>> ChainDB::listUtreexoCheckpoints() const {
    if (!db_) return Status::Internal;

    std::vector<int> heights;

    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    // Seek to first utreexo checkpoint
    std::string prefix;
    prefix.push_back(PREFIX_UTREEXO_CHECKPOINT);
    it->Seek(prefix);

    while (it->Valid()) {
        auto key = it->key();

        // Check if still in utreexo checkpoint range
        if (key.size() != 5 || key[0] != PREFIX_UTREEXO_CHECKPOINT) {
            break;
        }

        // Decode height from key (big-endian)
        int height = (static_cast<uint8_t>(key[1]) << 24) |
                     (static_cast<uint8_t>(key[2]) << 16) |
                     (static_cast<uint8_t>(key[3]) << 8) |
                     static_cast<uint8_t>(key[4]);

        heights.push_back(height);
        it->Next();
    }

    return std::move(heights);
}

Status ChainDB::deleteUtreexoCheckpoint(const ChainWriteToken& token, int height,
                                        rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    // Key: PREFIX_UTREEXO_CHECKPOINT + height (4 bytes, big-endian)
    std::string key;
    key.reserve(5);
    key.push_back(PREFIX_UTREEXO_CHECKPOINT);
    key.push_back((height >> 24) & 0xFF);
    key.push_back((height >> 16) & 0xFF);
    key.push_back((height >> 8) & 0xFF);
    key.push_back(height & 0xFF);

    if (wb) {
        wb->Delete(cf_[idx_utreexo_].get(), key);
        return Status::Ok;
    } else {
        auto status = db_->Delete(rocksdb::WriteOptions(), cf_[idx_utreexo_].get(), key);
        return convertRocksDBStatus(status);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Utreexo Checkpoint Integrity (Atomic checkpoint + SHA256 checksum)
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::putUtreexoCheckpointWithChecksum(const ChainWriteToken& token, int height,
                                                  const std::vector<uint8_t>& serialized_forest,
                                                  rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Build checkpoint key: PREFIX_UTREEXO_CHECKPOINT + height (4 bytes, big-endian)
    std::string ckpt_key;
    ckpt_key.reserve(5);
    ckpt_key.push_back(PREFIX_UTREEXO_CHECKPOINT);
    ckpt_key.push_back((height >> 24) & 0xFF);
    ckpt_key.push_back((height >> 16) & 0xFF);
    ckpt_key.push_back((height >> 8) & 0xFF);
    ckpt_key.push_back(height & 0xFF);

    // Build checksum key: PREFIX_UTREEXO_CHECKSUM + height (4 bytes, big-endian)
    std::string csum_key;
    csum_key.reserve(5);
    csum_key.push_back(PREFIX_UTREEXO_CHECKSUM);
    csum_key.push_back((height >> 24) & 0xFF);
    csum_key.push_back((height >> 16) & 0xFF);
    csum_key.push_back((height >> 8) & 0xFF);
    csum_key.push_back(height & 0xFF);

    // Compute SHA256 of the exact serialized bytes
    unsigned char hash[32];
    crypto::CSHA256().Write(serialized_forest.data(), serialized_forest.size()).Finalize(hash);
    std::string checksum(reinterpret_cast<const char*>(hash), 32);

    // Value: serialized forest data
    std::string value(serialized_forest.begin(), serialized_forest.end());

    if (wb) {
        wb->Put(cf_[idx_utreexo_].get(), ckpt_key, value);
        wb->Put(cf_[idx_utreexo_].get(), csum_key, checksum);
        return Status::Ok;
    }

    // Atomic WriteBatch: both checkpoint and checksum written together.
    // Power loss writes both or neither — no window for partial state.
    rocksdb::WriteBatch batch;
    batch.Put(cf_[idx_utreexo_].get(), ckpt_key, value);
    batch.Put(cf_[idx_utreexo_].get(), csum_key, checksum);

    auto status = db_->Write(rocksdb::WriteOptions(), &batch);
    return convertRocksDBStatus(status);
}

Status ChainDB::deleteUtreexoCheckpointWithChecksum(const ChainWriteToken& token, int height) {
    if (!db_) return Status::Internal;
    (void)token;

    // Build both keys
    std::string ckpt_key;
    ckpt_key.reserve(5);
    ckpt_key.push_back(PREFIX_UTREEXO_CHECKPOINT);
    ckpt_key.push_back((height >> 24) & 0xFF);
    ckpt_key.push_back((height >> 16) & 0xFF);
    ckpt_key.push_back((height >> 8) & 0xFF);
    ckpt_key.push_back(height & 0xFF);

    std::string csum_key;
    csum_key.reserve(5);
    csum_key.push_back(PREFIX_UTREEXO_CHECKSUM);
    csum_key.push_back((height >> 24) & 0xFF);
    csum_key.push_back((height >> 16) & 0xFF);
    csum_key.push_back((height >> 8) & 0xFF);
    csum_key.push_back(height & 0xFF);

    // Atomic delete of both
    rocksdb::WriteBatch batch;
    batch.Delete(cf_[idx_utreexo_].get(), ckpt_key);
    batch.Delete(cf_[idx_utreexo_].get(), csum_key);

    auto status = db_->Write(rocksdb::WriteOptions(), &batch);
    return convertRocksDBStatus(status);
}

StatusOr<std::vector<uint8_t>> ChainDB::getUtreexoChecksum(int height) const {
    if (!db_) return Status::Internal;

    // Key: PREFIX_UTREEXO_CHECKSUM + height (4 bytes, big-endian)
    std::string key;
    key.reserve(5);
    key.push_back(PREFIX_UTREEXO_CHECKSUM);
    key.push_back((height >> 24) & 0xFF);
    key.push_back((height >> 16) & 0xFF);
    key.push_back((height >> 8) & 0xFF);
    key.push_back(height & 0xFF);

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_utreexo_].get(), key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    return std::vector<uint8_t>(value.begin(), value.end());
}

Status ChainDB::putUtreexoMeta(const ChainWriteToken& token, const std::string& key,
                                const std::string& value,
                                rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Key: PREFIX_UTREEXO_META + key_string
    std::string db_key;
    db_key.reserve(1 + key.size());
    db_key.push_back(PREFIX_UTREEXO_META);
    db_key.append(key);

    if (wb != nullptr) {
        // Caller is folding this write into an outer atomic batch.
        // Status::Ok always — Put on a WriteBatch only fails on
        // memory exhaustion which rocksdb signals via exception.
        wb->Put(cf_[idx_utreexo_].get(), db_key, value);
        return Status::Ok;
    }

    auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_utreexo_].get(), db_key, value);
    return convertRocksDBStatus(status);
}

StatusOr<std::string> ChainDB::getUtreexoMeta(const std::string& key) const {
    if (!db_) return Status::Internal;

    // Key: PREFIX_UTREEXO_META + key_string
    std::string db_key;
    db_key.reserve(1 + key.size());
    db_key.push_back(PREFIX_UTREEXO_META);
    db_key.append(key);

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_utreexo_].get(), db_key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    return value;
}

// ═══════════════════════════════════════════════════════════════════════════
// Utreexo Forest Recovery — crash-resilient startup
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::putForestTipMarker(const ChainWriteToken& token, const ForestTipMarker& marker,
                                    rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Serialize: height(4) + block_hash(32) + forest_root(32) = 68 bytes
    std::string value;
    value.resize(68);
    auto h = static_cast<uint32_t>(marker.height);
    std::memcpy(&value[0], &h, 4);
    std::memcpy(&value[4], marker.block_hash.data, 32);
    std::memcpy(&value[36], marker.forest_root.data, 32);

    if (wb != nullptr) {
        wb->Put(cf_[idx_meta_].get(), KEY_FOREST_TIP, value);
        return Status::Ok;
    }

    rocksdb::WriteOptions opts;
    opts.sync = true;
    auto status = db_->Put(opts, cf_[idx_meta_].get(), KEY_FOREST_TIP, value);
    return convertRocksDBStatus(status);
}

StatusOr<ChainDB::ForestTipMarker> ChainDB::getForestTipMarker() const {
    if (!db_) return Status::Internal;

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_meta_].get(), KEY_FOREST_TIP, &value);

    if (status.IsNotFound()) return Status::NotFound;
    if (!status.ok()) return convertRocksDBStatus(status);
    if (value.size() != 68) return Status::Corruption;

    ForestTipMarker marker;
    uint32_t h;
    std::memcpy(&h, &value[0], 4);
    marker.height = static_cast<int32_t>(h);
    std::memcpy(marker.block_hash.data, &value[4], 32);
    std::memcpy(marker.forest_root.data, &value[36], 32);
    return marker;
}

Status ChainDB::putShieldedTipMarker(const ChainWriteToken& token,
                                     const ShieldedTipMarker& marker,
                                     rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Serialize: height(4) + block_hash(32) + shielded_root(32)
    //          + tree_size(8) + nullifier_count(8) = 84 bytes
    std::string value;
    value.resize(84);
    auto h = static_cast<uint32_t>(marker.height);
    std::memcpy(&value[0], &h, 4);
    std::memcpy(&value[4], marker.block_hash.data, 32);
    std::memcpy(&value[36], marker.shielded_root.data, 32);
    std::memcpy(&value[68], &marker.tree_size, 8);
    std::memcpy(&value[76], &marker.nullifier_count, 8);

    if (wb != nullptr) {
        wb->Put(cf_[idx_meta_].get(), KEY_SHIELDED_TIP, value);
        return Status::Ok;
    }

    rocksdb::WriteOptions opts;
    opts.sync = true;
    auto status = db_->Put(opts, cf_[idx_meta_].get(), KEY_SHIELDED_TIP, value);
    return convertRocksDBStatus(status);
}

StatusOr<ChainDB::ShieldedTipMarker> ChainDB::getShieldedTipMarker() const {
    if (!db_) return Status::Internal;

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_meta_].get(),
                           KEY_SHIELDED_TIP, &value);

    if (status.IsNotFound()) return Status::NotFound;
    if (!status.ok()) return convertRocksDBStatus(status);
    if (value.size() != 84) return Status::Corruption;

    ShieldedTipMarker marker;
    uint32_t h = 0;
    std::memcpy(&h, &value[0], 4);
    marker.height = static_cast<int32_t>(h);
    std::memcpy(marker.block_hash.data, &value[4], 32);
    std::memcpy(marker.shielded_root.data, &value[36], 32);
    std::memcpy(&marker.tree_size, &value[68], 8);
    std::memcpy(&marker.nullifier_count, &value[76], 8);
    return marker;
}

Status ChainDB::deleteShieldedTipMarker(const ChainWriteToken& token) {
    if (!db_) return Status::Internal;
    (void)token;

    rocksdb::WriteOptions opts;
    opts.sync = true;
    auto status = db_->Delete(opts, cf_[idx_meta_].get(), KEY_SHIELDED_TIP);
    return convertRocksDBStatus(status);
}

// ── Phase 3b nullifier fold-in ─────────────────────────────────────

namespace {

// Mirror of ChainDB::PREFIX_SHIELDED_NULLIFIER — the class member is
// private and these helpers live in an unnamed namespace, so we
// duplicate the byte locally. Keep in lockstep with the header.
constexpr uint8_t kShieldedNullifierPrefix = 'N';

// Build the lexicographically-sortable rocksdb key for a single
// shielded nullifier row. Layout:
//   [PREFIX_SHIELDED_NULLIFIER (1)] [height_be_4 (4)] [nullifier (32)]
// 37 bytes total. Big-endian height ensures prefix iteration yields
// (height ASC, nullifier ASC) order — exactly what
// NullifierSet::SerializeContent expects under DSRH v2.
std::string MakeShieldedNullifierKey(uint32_t block_height, const uint8_t* nullifier_32) {
    std::string key;
    key.resize(37);
    key[0] = static_cast<char>(kShieldedNullifierPrefix);
    key[1] = static_cast<char>((block_height >> 24) & 0xFF);
    key[2] = static_cast<char>((block_height >> 16) & 0xFF);
    key[3] = static_cast<char>((block_height >>  8) & 0xFF);
    key[4] = static_cast<char>( block_height        & 0xFF);
    std::memcpy(&key[5], nullifier_32, 32);
    return key;
}

bool DecodeShieldedNullifierKey(const rocksdb::Slice& key,
                                uint32_t* out_height,
                                const uint8_t** out_nullifier) {
    if (key.size() != 37) return false;
    if (static_cast<uint8_t>(key.data()[0]) != kShieldedNullifierPrefix) return false;
    *out_height =
        (static_cast<uint32_t>(static_cast<uint8_t>(key.data()[1])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(key.data()[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(key.data()[3])) <<  8) |
         static_cast<uint32_t>(static_cast<uint8_t>(key.data()[4]));
    *out_nullifier = reinterpret_cast<const uint8_t*>(key.data() + 5);
    return true;
}

} // namespace

Status ChainDB::putShieldedNullifier(const ChainWriteToken& token,
                                      uint32_t block_height,
                                      const uint8_t nullifier_32[32],
                                      rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    const std::string key = MakeShieldedNullifierKey(block_height, nullifier_32);
    static const std::string kEmpty;

    if (wb != nullptr) {
        wb->Put(cf_[idx_utreexo_].get(), key, kEmpty);
        return Status::Ok;
    }

    rocksdb::WriteOptions opts;
    opts.sync = true;
    auto status = db_->Put(opts, cf_[idx_utreexo_].get(), key, kEmpty);
    return convertRocksDBStatus(status);
}

StatusOr<uint64_t> ChainDB::deleteShieldedNullifiersAboveHeight(
    const ChainWriteToken& token,
    uint32_t height,
    rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Strict-greater-than the cutoff: scan from height + 1.
    if (height == std::numeric_limits<uint32_t>::max()) return uint64_t{0};
    const uint32_t scan_from = height + 1;

    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    // Seek to the first nullifier row at scan_from; rely on big-endian
    // height ordering so everything we care about is contiguous.
    std::string seek_key;
    seek_key.resize(5);
    seek_key[0] = static_cast<char>(kShieldedNullifierPrefix);
    seek_key[1] = static_cast<char>((scan_from >> 24) & 0xFF);
    seek_key[2] = static_cast<char>((scan_from >> 16) & 0xFF);
    seek_key[3] = static_cast<char>((scan_from >>  8) & 0xFF);
    seek_key[4] = static_cast<char>( scan_from        & 0xFF);

    uint64_t deleted = 0;

    if (wb != nullptr) {
        for (it->Seek(seek_key); it->Valid(); it->Next()) {
            const auto k = it->key();
            if (k.size() != 37 ||
                static_cast<uint8_t>(k.data()[0]) != kShieldedNullifierPrefix) {
                break;  // left the prefix
            }
            wb->Delete(cf_[idx_utreexo_].get(), k);
            ++deleted;
        }
        // RocksDB iterators set Valid()=false on either end-of-range
        // or read error. Without status() the caller cannot tell the
        // two apart — a mid-scan I/O error would otherwise return Ok
        // with a partial count.
        if (!it->status().ok()) {
            return convertRocksDBStatus(it->status());
        }
        return deleted;
    }

    rocksdb::WriteBatch local_batch;
    for (it->Seek(seek_key); it->Valid(); it->Next()) {
        const auto k = it->key();
        if (k.size() != 37 ||
            static_cast<uint8_t>(k.data()[0]) != kShieldedNullifierPrefix) {
            break;
        }
        local_batch.Delete(cf_[idx_utreexo_].get(), k);
        ++deleted;
    }
    if (!it->status().ok()) {
        return convertRocksDBStatus(it->status());
    }
    if (deleted == 0) return uint64_t{0};

    rocksdb::WriteOptions opts;
    opts.sync = true;
    const auto status = db_->Write(opts, &local_batch);
    if (!status.ok()) return convertRocksDBStatus(status);
    return deleted;
}

StatusOr<uint64_t> ChainDB::deleteAllShieldedNullifiers(
    const ChainWriteToken& token,
    rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    // Seek to the first nullifier row (bare prefix) and delete every row that
    // carries it — the whole set, all heights.
    std::string seek_key;
    seek_key.push_back(static_cast<char>(kShieldedNullifierPrefix));

    uint64_t deleted = 0;

    if (wb != nullptr) {
        for (it->Seek(seek_key); it->Valid(); it->Next()) {
            const auto k = it->key();
            if (k.size() != 37 ||
                static_cast<uint8_t>(k.data()[0]) != kShieldedNullifierPrefix) {
                break;  // left the prefix
            }
            wb->Delete(cf_[idx_utreexo_].get(), k);
            ++deleted;
        }
        if (!it->status().ok()) {
            return convertRocksDBStatus(it->status());
        }
        return deleted;
    }

    rocksdb::WriteBatch local_batch;
    for (it->Seek(seek_key); it->Valid(); it->Next()) {
        const auto k = it->key();
        if (k.size() != 37 ||
            static_cast<uint8_t>(k.data()[0]) != kShieldedNullifierPrefix) {
            break;
        }
        local_batch.Delete(cf_[idx_utreexo_].get(), k);
        ++deleted;
    }
    if (!it->status().ok()) {
        return convertRocksDBStatus(it->status());
    }
    if (deleted == 0) return uint64_t{0};

    rocksdb::WriteOptions opts;
    opts.sync = true;
    const auto status = db_->Write(opts, &local_batch);
    if (!status.ok()) return convertRocksDBStatus(status);
    return deleted;
}

Status ChainDB::forEachShieldedNullifier(
    const ShieldedNullifierVisitor& visit) const {
    if (!db_) return Status::Internal;
    if (!visit) return Status::Invalid;

    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    std::string seek_key;
    seek_key.push_back(static_cast<char>(kShieldedNullifierPrefix));

    for (it->Seek(seek_key); it->Valid(); it->Next()) {
        uint32_t height = 0;
        const uint8_t* nullifier_ptr = nullptr;
        if (!DecodeShieldedNullifierKey(it->key(), &height, &nullifier_ptr)) {
            // Either malformed or we've left the prefix.
            if (it->key().size() < 1 ||
                static_cast<uint8_t>(it->key().data()[0]) != kShieldedNullifierPrefix) {
                break;
            }
            continue;
        }
        if (!visit(height, nullifier_ptr)) {
            break;
        }
    }
    // RocksDB iterators set Valid()=false on either end-of-range or
    // read error. Surface I/O errors so the caller can fail loud
    // rather than treat a partial scan as a complete one.
    if (!it->status().ok()) {
        return convertRocksDBStatus(it->status());
    }
    return Status::Ok;
}

StatusOr<uint64_t> ChainDB::countShieldedNullifiers() const {
    if (!db_) return Status::Internal;

    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    std::string seek_key;
    seek_key.push_back(static_cast<char>(kShieldedNullifierPrefix));

    uint64_t count = 0;
    for (it->Seek(seek_key); it->Valid(); it->Next()) {
        const auto k = it->key();
        if (k.size() != 37 ||
            static_cast<uint8_t>(k.data()[0]) != kShieldedNullifierPrefix) {
            break;
        }
        ++count;
    }
    if (!it->status().ok()) {
        return convertRocksDBStatus(it->status());
    }
    return count;
}

Status ChainDB::wipeAllUtreexoCheckpoints() {
    if (!db_) return Status::Internal;

    // Iterate all keys in the utreexo CF and delete them
    rocksdb::WriteBatch batch;
    rocksdb::ReadOptions read_opts;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts, cf_[idx_utreexo_].get()));

    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        batch.Delete(cf_[idx_utreexo_].get(), it->key());
        count++;
    }

    // Also delete the forest tip marker from meta CF
    batch.Delete(cf_[idx_meta_].get(), KEY_FOREST_TIP);

    if (count == 0) return Status::Ok;

    rocksdb::WriteOptions opts;
    opts.sync = true;
    auto status = db_->Write(opts, &batch);
    return convertRocksDBStatus(status);
}

Status ChainDB::putRecoveryMarker(const RecoveryMarker& marker) {
    if (!db_) return Status::Internal;

    // Serialize: height(4) + tip_hash(32) + timestamp(8) = 44 bytes
    std::string value;
    value.resize(44);
    auto h = static_cast<uint32_t>(marker.height);
    std::memcpy(&value[0], &h, 4);
    std::memcpy(&value[4], marker.tip_hash.data, 32);
    std::memcpy(&value[36], &marker.timestamp, 8);

    auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_meta_].get(), KEY_RECOVERY, value);
    return convertRocksDBStatus(status);
}

StatusOr<ChainDB::RecoveryMarker> ChainDB::getRecoveryMarker() const {
    if (!db_) return Status::Internal;

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_meta_].get(), KEY_RECOVERY, &value);

    if (status.IsNotFound()) return Status::NotFound;
    if (!status.ok()) return convertRocksDBStatus(status);
    if (value.size() != 44) return Status::Corruption;

    RecoveryMarker marker;
    uint32_t h;
    std::memcpy(&h, &value[0], 4);
    marker.height = static_cast<int32_t>(h);
    std::memcpy(marker.tip_hash.data, &value[4], 32);
    std::memcpy(&marker.timestamp, &value[36], 8);
    return marker;
}

Status ChainDB::getSchemaVersion(int& version) const {
    if (!db_) return Status::Internal;
    
    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_meta_].get(), KEY_SCHEMA_VERSION, &value);
    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }
    
    if (value.size() != sizeof(uint32_t)) {
        return Status::Corruption;
    }
    
    version = *reinterpret_cast<const uint32_t*>(value.data());
    return Status::Ok;
}

Status ChainDB::setSchemaVersion(const ChainWriteToken& token, int version, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    uint32_t version_data = static_cast<uint32_t>(version);
    std::string value(reinterpret_cast<const char*>(&version_data), sizeof(version_data));

    if (wb) {
        wb->Put(cf_[idx_meta_].get(), KEY_SCHEMA_VERSION, value);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_meta_].get(), KEY_SCHEMA_VERSION, value);
        return convertRocksDBStatus(status);
    }
}

StatusOr<std::string> ChainDB::getStats() const {
    if (!db_) return Status::Internal;
    
    auto stats = jj::obj();
    
    // Get RocksDB statistics
    std::string rocksdb_stats;
    if (db_->GetProperty("rocksdb.stats", &rocksdb_stats)) {
        jj::putStr(stats, "rocksdb_stats", rocksdb_stats);
    }
    
    // Get approximate sizes
    uint64_t total_size = 0;
    std::vector<rocksdb::ColumnFamilyHandle*> cfs = {
        cf_[idx_blocks_].get(), 
        cf_[idx_headers_].get(), 
        cf_[idx_height_].get(), 
        cf_[idx_utxo_].get()
    };
    std::vector<std::string> cf_names = {"blocks", "headers", "height", "utxo"};
    
    for (size_t i = 0; i < cfs.size(); ++i) {
        uint64_t size = 0;
        if (db_->GetIntProperty(cfs[i], "rocksdb.total-sst-files-size", &size)) {
            jj::putNum(stats, (cf_names[i] + "_size").c_str(), size);
            total_size += size;
        }
    }
    
    jj::putNum(stats, "total_size_bytes", total_size);
    
    return jj::toString(stats);
}

// Private helper methods
std::string ChainDB::makeBlockKey(const uint256& hash) const {
    std::string key;
    key.reserve(1 + 64);
    key.push_back(PREFIX_BLOCK);
    key.append(hash.GetHex());
    return key;
}

std::string ChainDB::makeHeaderKey(const uint256& hash) const {
    std::string key;
    key.reserve(1 + 64);
    key.push_back(PREFIX_HEADER);
    key.append(hash.GetHex());
    return key;
}

std::string ChainDB::makeHeaderMetadataKey(const uint256& hash) const {
    std::string key;
    key.reserve(1 + 64);
    key.push_back(PREFIX_HEADER_META);
    key.append(hash.GetHex());
    return key;
}

std::string ChainDB::makeHeightKey(int height) const {
    // Use unified height key encoding from serialization.h
    return dinero::KH(static_cast<uint32_t>(height));
}

std::string ChainDB::makeTxIndexKey(const uint256& txid) const {
    std::string key;
    key.reserve(1 + 64);
    key.push_back(PREFIX_TXINDEX);
    key.append(txid.GetHex());
    return key;
}

std::string ChainDB::makeUtxoKey(const uint256& txid, uint32_t vout) const {
    std::string key;
    key.reserve(1 + 32 + 4);  // PREFIX + 32-byte txid + 4-byte vout
    key.push_back(PREFIX_UTXO);

    // Convert hex string txid to raw 32 bytes (Bitcoin Core standard)
    // txid is a 64-character hex string, convert to 32 bytes
    std::string txid_hex = txid.GetHex();
    if (txid_hex.size() != 64) {
        // Invalid txid length - return empty key (will fail lookups)
        return key;
    }

    for (size_t i = 0; i < 64; i += 2) {
        unsigned int byte;
        sscanf(txid_hex.c_str() + i, "%2x", &byte);
        key.push_back(static_cast<char>(byte));
    }

    // Big-endian vout for consistent ordering
    uint32_t be_vout = bswap32(vout);
    key.append(reinterpret_cast<const char*>(&be_vout), sizeof(be_vout));

    return key;
}

Status ChainDB::convertRocksDBStatus(const rocksdb::Status& status) const {
    if (status.ok()) return Status::Ok;
    if (status.IsNotFound()) return Status::NotFound;
    if (status.IsCorruption()) return Status::Corruption;
    if (status.IsIOError()) return Status::Io;
    if (status.IsInvalidArgument()) return Status::Invalid;
    return Status::Internal;
}

rocksdb::Options ChainDB::getDefaultOptions() const {
    rocksdb::Options options;

    // MINIMAL OPTIONS for vendored RocksDB build stability
    // Advanced tuning removed to prevent build-time feature mismatches
    // Performance options can be reintroduced later after mainnet stability

    // Required for DB creation
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    options.error_if_exists = false;  // Allow reopening existing DBs

    if (test_env_) options.env = test_env_;  // test-only seam (#371)

    // Safety checks
    options.paranoid_checks = true;

    // Disable compression (vendored build has no LZ4/Snappy/ZSTD)
    options.compression = rocksdb::kNoCompression;

    // iOS: skip file locking (single-process sandbox, avoids lifecycle lock races)
#if defined(__APPLE__) && TARGET_OS_IOS
    options.env = getNoLockEnv();
#endif

    // DO NOT set BlockBasedTableOptions - use vendored build defaults
    // DO NOT set prefix extractors - not needed for correctness
    // DO NOT set advanced I/O options - may not be compiled in

    // Bound the open-file cache. RocksDB defaults to -1 (unlimited), so a chain
    // with many SST files (e.g. 1200+) keeps ~all of them open, pushing the
    // process FD count past FD_SETSIZE (1024). The RPC server's select() loop
    // then fails ("Select error") on any listen socket whose fd >= 1024, so RPC
    // never comes up and the wallet can't connect. Capping this keeps the FD
    // count bounded as the chain grows. (RocksDB reopens cold SSTs on demand.)
    options.max_open_files = 512;

    return options;
}

rocksdb::ReadOptions ChainDB::getReadOptions() const {
    rocksdb::ReadOptions opts;
    opts.io_activity = rocksdb::Env::IOActivity::kGet;
    return opts;
}

// ═══════════════════════════════════════════════════════════════════════════════
// CONSENSUS-CRITICAL INVARIANT: 8-CF ROCKSDB SCHEMA (APPEND-ONLY, FROZEN)
// ═══════════════════════════════════════════════════════════════════════════════
// Dinero's blockchain database uses exactly 8 RocksDB column families.
// This schema is IMMUTABLE and crash-tested for production reliability:
//
//   1. default  - RocksDB required CF (metadata)
//   2. meta     - Chain metadata (tip, best chain)
//   3. blocks   - Full block data (header + transactions)
//   4. headers  - Block headers only (for headers-first sync)
//   5. height   - Height-to-hash index (fast lookups)
//   6. txindex  - Transaction index (optional, for tx lookup by hash)
//   7. utxo     - UTXO set (unspent transaction outputs)
//   8. utreexo  - Utreexo accumulator state (AFTER-state roots)
//
// PROVEN BY: Hardened soak test (2026-01-08)
//   - 25 SIGKILL crash cycles under active mining
//   - Perfect persistence across all crashes
//   - Zero CF migration failures
//   - Zero reindex required
//
// SCHEMA EVOLUTION RULES:
//   - NEVER remove existing CFs (breaks old nodes)
//   - NEVER reorder CFs (breaks compatibility)
//   - NEW CFs must be APPENDED to the end
//   - Document migration path for any new CFs
//
// See: docs/UTREEXO_PERSISTENCE_AUDIT.md for crash test evidence
// ═══════════════════════════════════════════════════════════════════════════════
std::vector<rocksdb::ColumnFamilyDescriptor> ChainDB::getColumnFamilyDescriptors() const {
    auto options = getDefaultOptions();

    return {
        rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName, options),
        rocksdb::ColumnFamilyDescriptor("meta", options),
        rocksdb::ColumnFamilyDescriptor("blocks", options),
        rocksdb::ColumnFamilyDescriptor("headers", options),
        rocksdb::ColumnFamilyDescriptor("height", options),
        rocksdb::ColumnFamilyDescriptor("txindex", options),
        rocksdb::ColumnFamilyDescriptor("utxo", options),
        rocksdb::ColumnFamilyDescriptor("utreexo", options)  // Phase 2.1: Utreexo accumulator checkpoints
    };
}

Status ChainDB::forEachUTXO(std::function<bool(const uint256& txid, uint32_t vout, const Coin& coin)> callback) const {
    // Phase 34.1: Write diagnostics to file for visibility when daemon forks
    std::ofstream diag("/tmp/chaindb_foreach_utxo.log", std::ios::app);

    if (!db_) {
        diag << "forEachUTXO: DB not initialized" << std::endl;
        diag.close();
        return Status::Internal;
    }

    // Phase 34.1: Defensive check - ensure CF is initialized
    if (cf_.size() <= static_cast<size_t>(idx_utxo_)) {
        diag << "forEachUTXO: UTXO column family not initialized (cf_.size()="
             << cf_.size() << ", idx_utxo_=" << idx_utxo_ << ")" << std::endl;
        diag.close();
        return Status::Internal;
    }

    if (!cf_[idx_utxo_]) {
        diag << "forEachUTXO: UTXO column family handle is null" << std::endl;
        diag.close();
        return Status::Internal;
    }

    // DIAGNOSTIC: Log CF status (will remove after debugging)
    diag << "forEachUTXO: Starting iteration (cf_.size()=" << cf_.size()
         << ", idx_utxo_=" << idx_utxo_ << ")" << std::endl;

    // Phase 34.1: Set io_activity for iterator (required by RocksDB)
    rocksdb::ReadOptions read_opts;
    read_opts.io_activity = rocksdb::Env::IOActivity::kDBIterator;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts, cf_[idx_utxo_].get()));

    // Seek to beginning of UTXO column family
    it->SeekToFirst();

    // Phase 34.1: Check iterator status immediately after Seek (RocksDB best practice)
    if (!it->status().ok()) {
        diag << "forEachUTXO: Iterator SeekToFirst() failed: " << it->status().ToString() << std::endl;
        diag.close();
        return convertRocksDBStatus(it->status());
    }

    diag << "forEachUTXO: Iterator seek successful, Valid=" << (it->Valid() ? "true" : "false") << std::endl;

    while (it->Valid()) {
        auto key = it->key().ToString();
        auto value = it->value().ToString();

        // Parse key: PREFIX_UTXO (1 byte) + txid (32 bytes) + vout (4 bytes big-endian)
        if (key.size() != 37 || key[0] != PREFIX_UTXO) {
            it->Next();
            continue;  // Skip malformed keys
        }

        // Extract txid (bytes 1-32), convert raw bytes to hex string
        std::string txid_hex;
        txid_hex.reserve(64);
        for (size_t i = 1; i <= 32; ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(key[i]));
            txid_hex.append(buf);
        }
        uint256 txid = uint256::FromHexUnsafe(txid_hex);

        // Extract vout (bytes 33-36, big-endian)
        uint32_t be_vout;
        std::memcpy(&be_vout, key.data() + 33, sizeof(be_vout));
        uint32_t vout = bswap32(be_vout);

        // Parse value (same format as getCoin)
        try {
            Reader r(value);
            Coin coin;
            coin.amount = r.read<uint64_t>();
            coin.script_pubkey = r.readString();
            coin.height = r.read<uint32_t>();
            coin.coinbase = (r.read<uint8_t>() != 0);
            if (!r.eof()) {
                coin.is_confidential = (r.read<uint8_t>() != 0);
                if (!r.eof()) {
                    coin.commitment = r.readBytes();
                }
            }

            // Call callback, stop if it returns false
            if (!callback(txid, vout, coin)) {
                break;
            }
        } catch (const std::exception&) {
            // Skip malformed UTXO
        }

        it->Next();
    }

    if (!it->status().ok()) {
        // TEMPORARY DIAGNOSTIC: Log exact RocksDB error
        diag << "forEachUTXO: RocksDB iterator error after iteration: " << it->status().ToString() << std::endl;
        diag.close();
        return convertRocksDBStatus(it->status());
    }

    diag << "forEachUTXO: Iteration completed successfully" << std::endl;
    diag.close();
    return Status::Ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// Block Index Operations (G.3.4/G.3.5 Adapter Support)
// ═══════════════════════════════════════════════════════════════════════════

bool ChainDB::isBlockConnected(const uint256& block_hash) const {
    // Look up block index in global map
    CBlockIndex* pindex = FindBlockIndex(block_hash);
    if (!pindex) {
        return false;
    }
    // A block is "connected" if it has BLOCK_VALID_SCRIPTS and BLOCK_HAVE_UNDO
    // This indicates the block is part of the active chain with UTXO set updated
    return (pindex->status & BLOCK_VALID_SCRIPTS) != 0 &&
           (pindex->status & 256 /* BLOCK_HAVE_UNDO */) != 0;
}

void ChainDB::markBlockConnected(const uint256& block_hash, bool connected) {
    // Look up block index in global map
    CBlockIndex* pindex = FindBlockIndex(block_hash);
    if (!pindex) {
        return;
    }
    if (connected) {
        // Mark as fully validated and having undo data
        pindex->status |= BLOCK_VALID_SCRIPTS;
        pindex->status |= 256; // BLOCK_HAVE_UNDO
    } else {
        // Clear the connected state (during disconnect/reorg)
        pindex->status &= ~BLOCK_VALID_SCRIPTS;
        pindex->status &= ~256; // BLOCK_HAVE_UNDO
    }
}

CBlockIndex* ChainDB::getBlockIndex(const uint256& block_hash) {
    // Delegate to global FindBlockIndex function from consensus/block_index.h
    return FindBlockIndex(block_hash);
}

bool ChainDB::commitBatch() {
    // ChainDB uses writeBatch for atomic operations
    // This method is for flushing any pending writes
    // Since we use RocksDB WriteOptions with sync, writes are already durable
    // For now, this is a no-op since we commit immediately on each batch
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// CSN Reorg Support — spend targets per block (CF7 utreexo)
// ═══════════════════════════════════════════════════════════════════════════

Status ChainDB::putCSNSpendTargets(const ChainWriteToken& token, const uint256& block_hash,
                                   const std::string& serialized_targets,
                                   rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;
    (void)token;

    // Key: PREFIX_CSN_SPEND_TARGETS + block_hash (32 raw bytes)
    std::string db_key;
    db_key.reserve(1 + 32);
    db_key.push_back(PREFIX_CSN_SPEND_TARGETS);
    db_key.append(reinterpret_cast<const char*>(block_hash.begin()), 32);

    if (wb) {
        wb->Put(cf_[idx_utreexo_].get(), db_key, serialized_targets);
        return Status::Ok;
    } else {
        auto status = db_->Put(rocksdb::WriteOptions(), cf_[idx_utreexo_].get(), db_key, serialized_targets);
        return convertRocksDBStatus(status);
    }
}

StatusOr<std::string> ChainDB::getCSNSpendTargets(const uint256& block_hash) const {
    if (!db_) return Status::Internal;

    // Key: PREFIX_CSN_SPEND_TARGETS + block_hash (32 raw bytes)
    std::string db_key;
    db_key.reserve(1 + 32);
    db_key.push_back(PREFIX_CSN_SPEND_TARGETS);
    db_key.append(reinterpret_cast<const char*>(block_hash.begin()), 32);

    std::string value;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_[idx_utreexo_].get(), db_key, &value);

    if (status.IsNotFound()) {
        return Status::NotFound;
    }
    if (!status.ok()) {
        return convertRocksDBStatus(status);
    }

    return value;
}

} // namespace dinero
