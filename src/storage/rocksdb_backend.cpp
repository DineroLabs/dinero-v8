#ifndef DIN_ENABLE_ROCKSDB
#  error "This file must not be compiled when DIN_ENABLE_ROCKSDB is OFF"
#endif

#include "storage/rocksdb_backend.h"
#include "storage/rocksdb_config.h" // Phase 6A
#include "primitives/block.h"
#include "wallet/transaction.h"
#include "common/serialization.h"
#include <rocksdb/db.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/table.h>
#include <rocksdb/env.h>
#include <rocksdb/utilities/backup_engine.h>
#include <rocksdb/compression_type.h> // Phase 6A: Compression support
#include <filesystem>
#include <iostream>
#include <thread>

namespace dinero {
namespace storage {

// Define column families once - MUST include default first
static const std::vector<std::string> kColumnFamilies = {
    rocksdb::kDefaultColumnFamilyName, // MUST be first
    "cf_meta", 
    "cf_blocks", 
    "cf_headers", 
    "cf_height", 
    "cf_utxo"
};

// RAII wrapper for column family handles
struct CfDeleter { 
    void operator()(rocksdb::ColumnFamilyHandle* h) const { 
        delete h; 
    } 
};
using CfUniquePtr = std::unique_ptr<rocksdb::ColumnFamilyHandle, CfDeleter>;

// RocksDBWriteBatch implementation
RocksDBWriteBatch::RocksDBWriteBatch() : db_(nullptr) {}

void RocksDBWriteBatch::putBlock(const std::string& hash, const Block& block) {
    // Serialize block and add to batch
    std::string key = makeBlockKey(hash);
    std::string value = block.Serialize();  // Binary block bytes
    batch_.Put(key, value);
}

void RocksDBWriteBatch::deleteBlock(const std::string& hash) {
    std::string key = makeBlockKey(hash);
    batch_.Delete(key);
}

void RocksDBWriteBatch::putTransaction(const std::string& hash, const Transaction& tx) {
    std::string key = makeTransactionKey(hash);
    std::vector<uint8_t> bytes = tx.Serialize(TxSerializationMode::WithWitness);
    rocksdb::Slice value(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    batch_.Put(key, value);
}

void RocksDBWriteBatch::deleteTransaction(const std::string& hash) {
    std::string key = makeTransactionKey(hash);
    batch_.Delete(key);
}

void RocksDBWriteBatch::putUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) {
    std::string key = makeUTXOKey(outpoint);
    rocksdb::Slice value(reinterpret_cast<const char*>(utxo_data.data()), utxo_data.size());
    batch_.Put(key, value);
}

void RocksDBWriteBatch::deleteUTXO(const std::string& outpoint) {
    std::string key = makeUTXOKey(outpoint);
    batch_.Delete(key);
}

void RocksDBWriteBatch::putChainState(const std::string& key, const std::vector<uint8_t>& value) {
    std::string db_key = makeChainStateKey(key);
    rocksdb::Slice db_value(reinterpret_cast<const char*>(value.data()), value.size());
    batch_.Put(db_key, db_value);
}

void RocksDBWriteBatch::deleteChainState(const std::string& key) {
    std::string db_key = makeChainStateKey(key);
    batch_.Delete(db_key);
}

void RocksDBWriteBatch::putMetadata(const std::string& key, const std::vector<uint8_t>& value) {
    std::string db_key = makeMetadataKey(key);
    rocksdb::Slice db_value(reinterpret_cast<const char*>(value.data()), value.size());
    batch_.Put(db_key, db_value);
}

void RocksDBWriteBatch::deleteMetadata(const std::string& key) {
    std::string db_key = makeMetadataKey(key);
    batch_.Delete(db_key);
}

StorageResult RocksDBWriteBatch::commit() {
    return commitSync(false);
}

StorageResult RocksDBWriteBatch::commitSync(bool sync) {
    if (!db_) return StorageResult::IO_ERROR;
    
    rocksdb::WriteOptions options;
    options.sync = sync;
    rocksdb::Status status = db_->Write(options, &batch_);
    
    if (status.ok()) {
        return StorageResult::SUCCESS;
    } else if (status.IsCorruption()) {
        return StorageResult::CORRUPTION;
    } else {
        return StorageResult::IO_ERROR;
    }
}

void RocksDBWriteBatch::clear() {
    batch_.Clear();
}

size_t RocksDBWriteBatch::size() const {
    return batch_.Count();
}

std::string RocksDBWriteBatch::makeBlockKey(const std::string& hash) const {
    return std::string(RocksDBBackend::BLOCK_PREFIX) + hash;
}

std::string RocksDBWriteBatch::makeTransactionKey(const std::string& hash) const {
    return std::string(RocksDBBackend::TRANSACTION_PREFIX) + hash;
}

std::string RocksDBWriteBatch::makeUTXOKey(const std::string& outpoint) const {
    return std::string(RocksDBBackend::UTXO_PREFIX) + outpoint;
}

std::string RocksDBWriteBatch::makeChainStateKey(const std::string& key) const {
    return std::string(RocksDBBackend::CHAIN_STATE_PREFIX) + key;
}

std::string RocksDBWriteBatch::makeMetadataKey(const std::string& key) const {
    return std::string(RocksDBBackend::METADATA_PREFIX) + key;
}

// RocksDBIterator implementation
RocksDBIterator::RocksDBIterator(std::unique_ptr<rocksdb::Iterator> it) 
    : iterator_(std::move(it)) {}

bool RocksDBIterator::isValid() const {
    return iterator_->Valid();
}

void RocksDBIterator::seekToFirst() {
    iterator_->SeekToFirst();
}

void RocksDBIterator::seekToLast() {
    iterator_->SeekToLast();
}

void RocksDBIterator::seek(const std::string& key) {
    iterator_->Seek(key);
}

void RocksDBIterator::next() {
    iterator_->Next();
}

void RocksDBIterator::prev() {
    iterator_->Prev();
}

std::string RocksDBIterator::key() const {
    return iterator_->key().ToString();
}

std::vector<uint8_t> RocksDBIterator::value() const {
    rocksdb::Slice slice = iterator_->value();
    return std::vector<uint8_t>(slice.data(), slice.data() + slice.size());
}

StorageResult RocksDBIterator::status() const {
    rocksdb::Status status = iterator_->status();
    if (status.ok()) return StorageResult::SUCCESS;
    if (status.IsCorruption()) return StorageResult::CORRUPTION;
    return StorageResult::IO_ERROR;
}

// RocksDBBackend implementation
RocksDBBackend::RocksDBBackend()
    : config_(RocksDBConfig::forProduction()) // Phase 6A: Default to production config
{
    configureOptions();
}

RocksDBBackend::RocksDBBackend(const RocksDBConfig& config)
    : config_(config)
{
    configureOptions();
}

RocksDBBackend::~RocksDBBackend() {
    close();
}

void RocksDBBackend::setConfig(const RocksDBConfig& config) {
    if (initialized_.load()) {
        std::cerr << "Cannot change configuration after initialization" << std::endl;
        return;
    }
    config_ = config;
    configureOptions();
}

std::string RocksDBBackend::getConfigSummary() const {
    return config_.toString();
}

bool RocksDBBackend::init(const std::string& path) {
    close();

    rocksdb::Options o;
    o.create_if_missing = true;
    o.create_missing_column_families = true;
    o.IncreaseParallelism(std::max(1u, std::thread::hardware_concurrency()));
    o.OptimizeLevelStyleCompaction();
    o.bytes_per_sync = 1<<20;
    o.use_direct_reads = true;

    rocksdb::BlockBasedTableOptions tbo;
    tbo.block_cache = rocksdb::NewLRUCache(256<<20);
    o.table_factory.reset(NewBlockBasedTableFactory(tbo));

    // Build CF descriptors we expect
    std::vector<rocksdb::ColumnFamilyDescriptor> desc;
    desc.reserve(kColumnFamilies.size());
    for (const auto& n : kColumnFamilies) {
        desc.emplace_back(n, rocksdb::ColumnFamilyOptions{});
    }

    rocksdb::DB* rawdb = nullptr;
    std::vector<rocksdb::ColumnFamilyHandle*> rawcf;
    auto st = rocksdb::DB::Open(o, path, desc, &rawcf, &rawdb);
    if (!st.ok()) {
        std::cerr << "RocksDB open failed: " << st.ToString() << std::endl;
        return false;
    }

    db_.reset(rawdb);
    cf_handles_.clear(); 
    cf_handles_.reserve(rawcf.size());
    for (auto* h : rawcf) {
        cf_handles_.emplace_back(h);
    }
    
    initialized_.store(true);
    return true;
}

StorageResult RocksDBBackend::initialize(const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_.load()) {
        return StorageResult::ALREADY_EXISTS;
    }
    
    try {
        // Create data directory if it doesn't exist
        std::filesystem::create_directories(data_dir);
        
        if (!init(data_dir)) {
            return StorageResult::IO_ERROR;
        }
        
        return StorageResult::SUCCESS;
        
    } catch (const std::exception& e) {
        std::cerr << "RocksDB initialization failed: " << e.what() << std::endl;
        return StorageResult::IO_ERROR;
    }
}

void RocksDBBackend::close() {
    cf_handles_.clear();   // destroy CF handles first (RAII)
    db_.reset();          // then DB
    initialized_.store(false);
}

StorageResult RocksDBBackend::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_.load()) {
        return StorageResult::SUCCESS;
    }
    
    close();
    return StorageResult::SUCCESS;
}

bool RocksDBBackend::isHealthy() const {
    return initialized_.load() && db_ != nullptr;
}

StorageResult RocksDBBackend::putBlock(const std::string& hash, const Block& block) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::vector<uint8_t> serialized = serializeBlock(block);
    std::string key = "b" + hash;  // Simple prefix keying
    
    rocksdb::Slice value(reinterpret_cast<const char*>(serialized.data()), serialized.size());
    auto* cf_blocks = cf_handles_[2];  // cf_blocks is index 2
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions{}, cf_blocks, key, value);
    
    return convertStatus(status);
}

StorageResult RocksDBBackend::getBlock(const std::string& hash, Block& block) const {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = "b" + hash;
    std::string value;
    auto* cf_blocks = cf_handles_[2];
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions{}, cf_blocks, key, &value);
    
    if (status.IsNotFound()) {
        return StorageResult::NOT_FOUND;
    }
    
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    std::vector<uint8_t> data(value.begin(), value.end());
    if (!deserializeBlock(data, block)) {
        return StorageResult::INVALID_DATA;
    }
    
    return StorageResult::SUCCESS;
}

bool RocksDBBackend::hasBlock(const std::string& hash) const {
    if (!isHealthy()) return false;
    
    std::string key = makeKey(BLOCK_PREFIX, hash);
    std::string value;
    
    rocksdb::Status status = db_->Get(read_options_, getColumnFamily(ColumnFamily::BLOCKS), key, &value);
    return status.ok();
}

StorageResult RocksDBBackend::deleteBlock(const std::string& hash) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(BLOCK_PREFIX, hash);
    rocksdb::Status status = db_->Delete(write_options_, getColumnFamily(ColumnFamily::BLOCKS), key);
    
    return convertStatus(status);
}

StorageResult RocksDBBackend::getBlockHeader(const std::string& hash, BlockHeader& header) const {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(BLOCK_HEADER_PREFIX, hash);
    std::string value;
    
    rocksdb::Status status = db_->Get(read_options_, getColumnFamily(ColumnFamily::BLOCKS), key, &value);
    
    if (status.IsNotFound()) {
        return StorageResult::NOT_FOUND;
    }
    
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    std::vector<uint8_t> data(value.begin(), value.end());
    if (!deserializeBlockHeader(data, header)) {
        return StorageResult::INVALID_DATA;
    }
    
    return StorageResult::SUCCESS;
}

StorageResult RocksDBBackend::putTransaction(const std::string& hash, const Transaction& tx) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::vector<uint8_t> serialized = serializeTransaction(tx);
    std::string key = makeKey(TRANSACTION_PREFIX, hash);
    
    rocksdb::Slice value(reinterpret_cast<const char*>(serialized.data()), serialized.size());
    rocksdb::Status status = db_->Put(write_options_, getColumnFamily(ColumnFamily::TRANSACTIONS), key, value);
    
    return convertStatus(status);
}

StorageResult RocksDBBackend::getTransaction(const std::string& hash, Transaction& tx) const {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(TRANSACTION_PREFIX, hash);
    std::string value;
    
    rocksdb::Status status = db_->Get(read_options_, getColumnFamily(ColumnFamily::TRANSACTIONS), key, &value);
    
    if (status.IsNotFound()) {
        return StorageResult::NOT_FOUND;
    }
    
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    std::vector<uint8_t> data(value.begin(), value.end());
    if (!deserializeTransaction(data, tx)) {
        return StorageResult::INVALID_DATA;
    }
    
    return StorageResult::SUCCESS;
}

bool RocksDBBackend::hasTransaction(const std::string& hash) const {
    if (!isHealthy()) return false;
    
    std::string key = makeKey(TRANSACTION_PREFIX, hash);
    std::string value;
    
    rocksdb::Status status = db_->Get(read_options_, getColumnFamily(ColumnFamily::TRANSACTIONS), key, &value);
    return status.ok();
}

StorageResult RocksDBBackend::deleteTransaction(const std::string& hash) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(TRANSACTION_PREFIX, hash);
    rocksdb::Status status = db_->Delete(write_options_, getColumnFamily(ColumnFamily::TRANSACTIONS), key);
    
    return convertStatus(status);
}

StorageResult RocksDBBackend::putUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(UTXO_PREFIX, outpoint);
    rocksdb::Slice value(reinterpret_cast<const char*>(utxo_data.data()), utxo_data.size());
    
    rocksdb::Status status = db_->Put(write_options_, getColumnFamily(ColumnFamily::UTXOS), key, value);
    return convertStatus(status);
}

StorageResult RocksDBBackend::getUTXO(const std::string& outpoint, std::vector<uint8_t>& utxo_data) const {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(UTXO_PREFIX, outpoint);
    std::string value;
    
    rocksdb::Status status = db_->Get(read_options_, getColumnFamily(ColumnFamily::UTXOS), key, &value);
    
    if (status.IsNotFound()) {
        return StorageResult::NOT_FOUND;
    }
    
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    utxo_data.assign(value.begin(), value.end());
    return StorageResult::SUCCESS;
}

bool RocksDBBackend::hasUTXO(const std::string& outpoint) const {
    if (!isHealthy()) return false;
    
    std::string key = makeKey(UTXO_PREFIX, outpoint);
    std::string value;
    
    rocksdb::Status status = db_->Get(read_options_, getColumnFamily(ColumnFamily::UTXOS), key, &value);
    return status.ok();
}

StorageResult RocksDBBackend::deleteUTXO(const std::string& outpoint) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    std::string key = makeKey(UTXO_PREFIX, outpoint);
    rocksdb::Status status = db_->Delete(write_options_, getColumnFamily(ColumnFamily::UTXOS), key);
    
    return convertStatus(status);
}

StorageResult RocksDBBackend::getUTXOsForAddress(const std::string& address, 
                                               std::vector<std::pair<std::string, std::vector<uint8_t>>>& utxos) const {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    // Use iterator to find all UTXOs for address
    std::string prefix = makeKey(ADDRESS_UTXO_PREFIX, address);
    auto iterator = const_cast<RocksDBBackend*>(this)->createIterator(prefix);
    
    utxos.clear();
    iterator->seek(prefix);
    
    while (iterator->isValid()) {
        std::string key = iterator->key();
        if (key.substr(0, prefix.length()) != prefix) {
            break; // No more UTXOs for this address
        }
        
        std::vector<uint8_t> value = iterator->value();
        utxos.emplace_back(key, value);
        
        iterator->next();
    }
    
    return iterator->status();
}

std::unique_ptr<WriteBatch> RocksDBBackend::createWriteBatch() {
    auto batch = std::make_unique<RocksDBWriteBatch>();
    batch->setDB(db_.get());
    return batch;
}

StorageResult RocksDBBackend::writeBatch(WriteBatch& batch) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    auto* rocks_batch = dynamic_cast<RocksDBWriteBatch*>(&batch);
    if (!rocks_batch) return StorageResult::INVALID_DATA;
    
    rocksdb::Status status = db_->Write(write_options_, &rocks_batch->getBatch());
    return convertStatus(status);
}

std::unique_ptr<StorageIterator> RocksDBBackend::createIterator(const std::string& prefix) {
    if (!isHealthy()) return nullptr;
    
    rocksdb::ReadOptions options = read_options_;
    if (!prefix.empty()) {
        // Set prefix for optimization
        options.prefix_same_as_start = true;
    }
    
    auto rocks_it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(options));
    return std::make_unique<RocksDBIterator>(std::move(rocks_it));
}

StorageResult RocksDBBackend::compact() {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    rocksdb::Status status = db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    return convertStatus(status);
}

StorageResult RocksDBBackend::verify() {
    // TODO: Implement database verification (integrity check)
    return StorageResult::SUCCESS;
}

StorageStats RocksDBBackend::getStats() const {
    StorageStats stats{};
    
    if (!isHealthy()) {
        return stats;
    }
    
    stats.backend_type = "RocksDB";
    
    // Get database properties
    uint64_t size;
    if (db_->GetIntProperty("rocksdb.total-sst-files-size", &size)) {
        stats.used_size_bytes = size;
    }
    
    uint64_t block_count;
    if (db_->GetIntProperty("rocksdb.estimate-num-keys", &block_count)) {
        stats.block_count = block_count;
    }
    
    // TODO: Add more detailed statistics
    stats.compression_ratio = 0.7; // Placeholder
    
    return stats;
}

StorageResult RocksDBBackend::putMetadata(const std::string& key, const std::vector<uint8_t>& value) {
    if (!db_) return StorageResult::IO_ERROR;
    
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, 
                                     rocksdb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
    return convertStatus(status);
}

StorageResult RocksDBBackend::getMetadata(const std::string& key, std::vector<uint8_t>& value) const {
    if (!db_) return StorageResult::IO_ERROR;
    
    std::string result;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &result);
    
    if (status.IsNotFound()) {
        return StorageResult::NOT_FOUND;
    }
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    value.assign(result.begin(), result.end());
    return StorageResult::SUCCESS;
}

StorageResult RocksDBBackend::deleteMetadata(const std::string& key) {
    if (!db_) return StorageResult::IO_ERROR;
    
    rocksdb::Status status = db_->Delete(rocksdb::WriteOptions(), key);
    return convertStatus(status);
}

StorageResult RocksDBBackend::putChainState(const std::string& key, const std::vector<uint8_t>& value) {
    if (!db_) return StorageResult::IO_ERROR;
    
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, 
                                     rocksdb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
    return convertStatus(status);
}

StorageResult RocksDBBackend::getChainState(const std::string& key, std::vector<uint8_t>& value) const {
    if (!db_) return StorageResult::IO_ERROR;
    
    std::string result;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &result);
    
    if (status.IsNotFound()) {
        return StorageResult::NOT_FOUND;
    }
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    value.assign(result.begin(), result.end());
    return StorageResult::SUCCESS;
}

StorageResult RocksDBBackend::deleteChainState(const std::string& key) {
    if (!db_) return StorageResult::IO_ERROR;
    
    rocksdb::Status status = db_->Delete(rocksdb::WriteOptions(), key);
    return convertStatus(status);
}

StorageResult RocksDBBackend::backup(const std::string& backup_dir) {
    if (!isHealthy()) return StorageResult::IO_ERROR;
    
    rocksdb::BackupEngine* backup_engine;
    rocksdb::Status status = rocksdb::BackupEngine::Open(
        rocksdb::Env::Default(),
        rocksdb::BackupEngineOptions(backup_dir),
        &backup_engine
    );
    
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    status = backup_engine->CreateNewBackup(db_.get());
    delete backup_engine;
    
    return convertStatus(status);
}

StorageResult RocksDBBackend::restore(const std::string& backup_dir) {
    // TODO: Implement restore functionality
    return StorageResult::SUCCESS;
}

StorageResult RocksDBBackend::setOption(const std::string& key, const std::string& value) {
    // TODO: Implement dynamic option setting
    return StorageResult::SUCCESS;
}

std::string RocksDBBackend::getOption(const std::string& key) const {
    std::string value;
    if (db_ && db_->GetProperty(key, &value)) {
        return value;
    }
    return std::string{};
}

// Private methods
StorageResult RocksDBBackend::initializeColumnFamilies(const std::string& data_dir) {
    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    
    // Default column family (must be first)
    column_families.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
    column_families.emplace_back("blocks", rocksdb::ColumnFamilyOptions());
    column_families.emplace_back("transactions", rocksdb::ColumnFamilyOptions());
    column_families.emplace_back("utxos", rocksdb::ColumnFamilyOptions());
    column_families.emplace_back("chain_state", rocksdb::ColumnFamilyOptions());
    column_families.emplace_back("metadata", rocksdb::ColumnFamilyOptions());
    
    rocksdb::DB* db_ptr = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(rocksdb::DBOptions(options_), data_dir, column_families, &cf_handles_, &db_ptr);
    
    if (status.IsInvalidArgument()) {
        // Column families don't exist, create them
        rocksdb::DB* temp_db;
        status = rocksdb::DB::Open(options_, data_dir, &temp_db);
        if (!status.ok()) {
            return convertStatus(status);
        }
        
        // Create column families
        rocksdb::ColumnFamilyHandle* cf;
        for (size_t i = 1; i < column_families.size(); ++i) {
            status = temp_db->CreateColumnFamily(column_families[i].options, column_families[i].name, &cf);
            if (!status.ok()) {
                delete temp_db;
                return convertStatus(status);
            }
            temp_db->DestroyColumnFamilyHandle(cf);
        }
        
        delete temp_db;
        
        // Reopen with column families
        status = rocksdb::DB::Open(rocksdb::DBOptions(options_), data_dir, column_families, &cf_handles_, &db_ptr);
    }
    
    if (!status.ok()) {
        return convertStatus(status);
    }
    
    db_.reset(db_ptr);
    return StorageResult::SUCCESS;
}

void RocksDBBackend::configureOptions() {
    // ========== Phase 6A: Advanced RocksDB Configuration ==========

    std::string validation_error;
    if (!config_.validate(&validation_error)) {
        std::cerr << "Invalid RocksDB configuration: " << validation_error << std::endl;
        // Fall back to safe defaults
        config_ = RocksDBConfig::forProduction();
    }

    // Basic options
    options_.create_if_missing = true;
    options_.create_missing_column_families = true;

    // ========== Memory Configuration ==========

    // Block cache for hot data (UTXO set, recent blocks)
    size_t cache_size = config_.getBlockCacheSize();
    block_cache_ = rocksdb::NewLRUCache(cache_size);

    // Write buffer (memtable) configuration
    options_.write_buffer_size = config_.getWriteBufferSize();
    options_.max_write_buffer_number = config_.max_write_buffer_number.value_or(4);
    options_.min_write_buffer_number_to_merge = config_.min_write_buffer_number_to_merge.value_or(2);

    // ========== File Management ==========

    options_.max_open_files = config_.getMaxOpenFiles();
    options_.target_file_size_base = config_.getTargetFileSizeBase();
    options_.target_file_size_multiplier = config_.target_file_size_multiplier.value_or(1);
    options_.max_bytes_for_level_base = config_.max_bytes_for_level_base.value_or(512 << 20); // 512MB

    // ========== Compression Strategy ==========

    // Map compression type from config
    if (config_.compression_type == "none") {
        options_.compression = rocksdb::kNoCompression;
    } else if (config_.compression_type == "snappy") {
        options_.compression = rocksdb::kSnappyCompression;
    } else if (config_.compression_type == "lz4") {
        options_.compression = rocksdb::kLZ4Compression;
    } else if (config_.compression_type == "zstd") {
        options_.compression = rocksdb::kZSTD;
    } else {
        // Default to LZ4 for good balance
        options_.compression = rocksdb::kLZ4Compression;
    }

    // Tiered compression: none for L0-L1 (hot data), compress L2+
    if (config_.use_tiered_compression) {
        options_.compression_per_level.resize(7);
        options_.compression_per_level[0] = rocksdb::kNoCompression;  // L0: no compression
        options_.compression_per_level[1] = rocksdb::kNoCompression;  // L1: no compression
        options_.compression_per_level[2] = rocksdb::kLZ4Compression; // L2: LZ4 (fast)
        options_.compression_per_level[3] = rocksdb::kLZ4Compression; // L3: LZ4
        options_.compression_per_level[4] = rocksdb::kLZ4Compression; // L4: LZ4
        options_.compression_per_level[5] = rocksdb::kZSTD;           // L5: ZSTD (cold data)
        options_.compression_per_level[6] = rocksdb::kZSTD;           // L6: ZSTD
    }

    // ZSTD compression level (3 = good balance)
    if (config_.zstd_compression_level.has_value()) {
        rocksdb::CompressionOptions comp_opts;
        comp_opts.level = *config_.zstd_compression_level;
        options_.compression_opts = comp_opts;
    }

    // ========== Level-0 Flush & Compaction Heuristics ==========

    options_.level0_file_num_compaction_trigger = config_.level0_file_num_compaction_trigger.value_or(4);
    options_.level0_slowdown_writes_trigger = config_.level0_slowdown_writes_trigger.value_or(20);
    options_.level0_stop_writes_trigger = config_.level0_stop_writes_trigger.value_or(36);

    // Dynamic level size adjustment for better space amplification
    options_.level_compaction_dynamic_level_bytes = config_.level_compaction_dynamic_level_bytes;

    // ========== Parallelism ==========

    int bg_jobs = config_.getMaxBackgroundJobs();
    options_.max_background_jobs = bg_jobs;
    options_.max_background_flushes = config_.max_background_flushes.value_or(2);
    options_.max_background_compactions = config_.max_background_compactions.value_or(bg_jobs - 2);

    // Optimize for level-style compaction
    options_.OptimizeLevelStyleCompaction(options_.write_buffer_size * options_.max_write_buffer_number);

    // Compaction readahead for sequential I/O
    options_.compaction_readahead_size = config_.compaction_readahead_size_bytes.value_or(2 << 20); // 2MB

    // ========== I/O Optimization ==========

    // Direct I/O (bypasses OS page cache)
    if (config_.use_direct_reads.has_value()) {
        options_.use_direct_reads = *config_.use_direct_reads;
    }

    if (config_.use_direct_io_for_flush_and_compaction.has_value()) {
        options_.use_direct_io_for_flush_and_compaction = *config_.use_direct_io_for_flush_and_compaction;
    }

    // Background sync to avoid large I/O stalls
    options_.bytes_per_sync = config_.bytes_per_sync.value_or(1 << 20);       // 1MB
    options_.wal_bytes_per_sync = config_.wal_bytes_per_sync.value_or(1 << 20); // 1MB

    // ========== Block-Based Table Options ==========

    rocksdb::BlockBasedTableOptions table_options;

    // Block cache (shared across all column families)
    table_options.block_cache = block_cache_;

    // Block size (16KB is good for random blockchain lookups)
    table_options.block_size = config_.block_size_bytes.value_or(16 << 10); // 16KB

    // Cache index and filter blocks in block cache
    table_options.cache_index_and_filter_blocks = config_.cache_index_and_filter_blocks;

    // Pin L0 filters in cache for fast tip access
    table_options.pin_l0_filter_and_index_blocks_in_cache = config_.pin_l0_filter_and_index_blocks_in_cache;

    // Bloom filter for faster point lookups (10 bits = ~1% false positive)
    int bloom_bits = config_.bloom_filter_bits_per_key.value_or(10);
    if (bloom_bits > 0) {
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(bloom_bits, config_.block_based_bloom_filter));
    }

    // Set table factory
    options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    // ========== Statistics & Monitoring ==========

    if (config_.enable_statistics) {
        statistics_ = rocksdb::CreateDBStatistics();
        options_.statistics = statistics_;
    }

    if (config_.stats_dump_period_sec.has_value()) {
        options_.stats_dump_period_sec = *config_.stats_dump_period_sec;
    }

    // ========== Write/Read Options ==========

    // Write options (sync=false for performance, caller can override with commitSync)
    write_options_.sync = false;
    write_options_.disableWAL = false; // Keep WAL for crash recovery

    // Read options
    read_options_.verify_checksums = true;

    // Log configuration summary
    std::cout << "[RocksDB Phase 6A] Configuration applied:\n" << config_.toString() << std::endl;
}

rocksdb::ColumnFamilyHandle* RocksDBBackend::getColumnFamily(ColumnFamily cf) const {
    size_t index = static_cast<size_t>(cf);
    if (index < cf_handles_.size()) {
        return cf_handles_[index];
    }
    return cf_handles_[0]; // Default CF
}

std::string RocksDBBackend::makeKey(const std::string& prefix, const std::string& key) const {
    return prefix + key;
}

StorageResult RocksDBBackend::convertStatus(const rocksdb::Status& status) const {
    if (status.ok()) return StorageResult::SUCCESS;
    if (status.IsNotFound()) return StorageResult::NOT_FOUND;
    if (status.IsCorruption()) return StorageResult::CORRUPTION;
    if (status.IsIOError()) return StorageResult::IO_ERROR;
    if (status.IsInvalidArgument()) return StorageResult::INVALID_DATA;
    return StorageResult::IO_ERROR;
}

// Serialization helpers
std::vector<uint8_t> RocksDBBackend::serializeBlock(const Block& block) const {
    std::string bytes = block.Serialize();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

bool RocksDBBackend::deserializeBlock(const std::vector<uint8_t>& data, Block& block) const {
    auto parsed = Block::Deserialize(data);
    if (!parsed.has_value()) {
        return false;
    }

    block = std::move(parsed.value());
    return true;
}

std::vector<uint8_t> RocksDBBackend::serializeTransaction(const Transaction& tx) const {
    return tx.Serialize(TxSerializationMode::WithWitness);
}

bool RocksDBBackend::deserializeTransaction(const std::vector<uint8_t>& data, Transaction& tx) const {
    return TransactionSerializer::Deserialize(tx, data);
}

std::vector<uint8_t> RocksDBBackend::serializeBlockHeader(const BlockHeader& header) const {
    std::string bytes = header.Serialize();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

bool RocksDBBackend::deserializeBlockHeader(const std::vector<uint8_t>& data, BlockHeader& header) const {
    auto opt = BlockHeader::Deserialize(data.data(), data.size());
    if (!opt.has_value()) {
        return false;
    }
    header = *opt;
    return true;
}

uint64_t RocksDBBackend::getApproximateSize(const std::string& start_key, const std::string& end_key) const {
    if (!db_) {
        return 0;
    }
    
    rocksdb::Range range(start_key, end_key);
    uint64_t size = 0;
    db_->GetApproximateSizes(&range, 1, &size);
    return size;
}

} // namespace storage
} // namespace dinero
