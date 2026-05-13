#include "common/blockchain_db.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <rocksdb/iterator.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/options.h>

namespace Dinero {
namespace Common {

// Single RocksDB with Column Families - TRUE ATOMICITY
bool BlockchainDB::initialize(const std::string& path) {
    if (initialized) {
        std::cerr << "❌ BlockchainDB already initialized" << std::endl;
        return false;
    }
    
    base_path = path;
    std::filesystem::create_directories(base_path);
    std::string db_path = base_path + "/blockchain.db";
    
    try {
        // Configure crash-safe database options
        rocksdb::DBOptions db_options;
        db_options.create_if_missing = true;
        db_options.create_missing_column_families = true;
        db_options.wal_recovery_mode = rocksdb::WALRecoveryMode::kPointInTimeRecovery;
        db_options.use_fsync = true;  // Force fsync for durability
        db_options.max_background_jobs = 4;
        db_options.IncreaseParallelism();
        
        // Configure column family options
        rocksdb::ColumnFamilyOptions cf_options;
        cf_options.OptimizeForPointLookup(1024);
        
        // Define column families
        std::vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors = {
            {rocksdb::kDefaultColumnFamilyName, cf_options},  // metadata: best_tip, state_hash, height
            {"cf_blocks", cf_options},                        // block data
            {"cf_chainstate", cf_options},                    // UTXO set
            {"cf_index", cf_options},                         // block indices
            {"cf_undo", cf_options},                          // undo records
            {"cf_txstore", cf_options},                       // transaction store
            {"cf_wallet", cf_options}                         // wallet data
        };
        
        // Open database with column families
        rocksdb::Status status = rocksdb::DB::Open(db_options, db_path, cf_descriptors, &cf_handles, &db);
        
        if (!status.ok()) {
            std::cerr << "❌ Failed to open database with column families: " << status.ToString() << std::endl;
            return false;
        }
        
        // Assign column family handles
        if (cf_handles.size() != 7) {
            std::cerr << "❌ Unexpected number of column family handles: " << cf_handles.size() << std::endl;
            return false;
        }
        
        cf_default = cf_handles[0];     // default (metadata)
        cf_blocks = cf_handles[1];      // blocks
        cf_chainstate = cf_handles[2];  // chainstate (UTXO)
        cf_index = cf_handles[3];       // index
        cf_undo = cf_handles[4];        // undo
        cf_txstore = cf_handles[5];     // txstore
        cf_wallet = cf_handles[6];      // wallet
        
        initialized = true;
        std::cout << "✅ BlockchainDB initialized with Column Families: " << db_path << std::endl;
        std::cout << "   📊 Column Families: default, blocks, chainstate, index, undo, txstore, wallet" << std::endl;
        std::cout << "   🔒 Crash-safe: WAL recovery + fsync enabled" << std::endl;
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Exception during database initialization: " << e.what() << std::endl;
        return false;
    }
}

void BlockchainDB::shutdown() {
    if (!initialized) return;
    
    std::cout << "🛑 Shutting down BlockchainDB..." << std::endl;
    
    // Close column family handles
    for (auto* handle : cf_handles) {
        if (handle) {
            delete handle;
        }
    }
    cf_handles.clear();
    
    // Close database
    if (db) {
        delete db;
        db = nullptr;
    }
    
    // Reset handles
    cf_default = nullptr;
    cf_blocks = nullptr;
    cf_chainstate = nullptr;
    cf_index = nullptr;
    cf_undo = nullptr;
    cf_txstore = nullptr;
    cf_wallet = nullptr;
    
    initialized = false;
    std::cout << "✅ BlockchainDB shutdown complete" << std::endl;
}

// TRUE ATOMIC BLOCK COMMIT - Single WriteBatch across all Column Families
BlockchainDB::BlockCommitResult BlockchainDB::commitBlock(
    const std::string& blockhash,
    uint32_t height,
    const std::string& block_data,
    const std::string& index_data,
    const std::vector<UTXOData>& utxos_to_create,
    const std::vector<std::string>& utxos_to_spend,
    const UndoRecord& undo_record
) {
    BlockCommitResult result;
    result.success = false;
    result.blockhash = blockhash;
    result.height = height;
    result.utxos_created = 0;
    result.utxos_spent = 0;
    
    if (!initialized || !db) {
        result.error = "Database not initialized";
        return result;
    }
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    try {
        // Fault injection for testing (regtest only)
        triggerFaultInjection("before-atomic-batch");
        
        // Create single atomic WriteBatch across ALL column families
        rocksdb::WriteBatch batch;
        
        // 1. Store block data (cf_blocks)
        std::string block_key = "block:" + std::to_string(height);
        std::string block_hash_key = "block_hash:" + blockhash;
        batch.Put(cf_blocks, block_key, block_data);
        batch.Put(cf_blocks, block_hash_key, block_data);
        
        // 2. Store block index (cf_index)
        std::string index_key = "index:" + std::to_string(height);
        std::string index_hash_key = "index_hash:" + blockhash;
        batch.Put(cf_index, index_key, index_data);
        batch.Put(cf_index, index_hash_key, index_data);
        
        // 3. Store undo record (cf_undo)
        std::string undo_key = "undo:" + blockhash;
        batch.Put(cf_undo, undo_key, undo_record.toJSON());
        
        // Fault injection point
        triggerFaultInjection("after-undo-written");
        
        // 4. Remove spent UTXOs (cf_chainstate)
        for (const std::string& outpoint : utxos_to_spend) {
            std::string utxo_key = "utxo:" + outpoint;
            batch.Delete(cf_chainstate, utxo_key);
            result.utxos_spent++;
        }
        
        // 5. Create new UTXOs (cf_chainstate)
        for (const UTXOData& utxo : utxos_to_create) {
            std::string utxo_key = "utxo:" + utxo.outpoint;
            batch.Put(cf_chainstate, utxo_key, utxo.toJSON());
            result.utxos_created++;
        }
        
        // Fault injection point
        triggerFaultInjection("after-utxo-before-besttip");
        
        // 6. Update best block pointers (cf_default) - LAST operation makes block "official"
        batch.Put(cf_default, "best_height", std::to_string(height));
        batch.Put(cf_default, "best_hash", blockhash);
        
        // Create state checkpoint every 10 blocks
        if (height % 10 == 0) {
            StateCheckpoint checkpoint;
            checkpoint.height = height;
            checkpoint.blockhash = blockhash;
            checkpoint.state_hash = calculateStateHash(height);
            checkpoint.timestamp = std::time(nullptr);
            checkpoint.utxo_count = result.utxos_created; // Simplified
            checkpoint.block_count = height + 1;
            
            batch.Put(cf_default, "checkpoint:" + std::to_string(height), checkpoint.toJSON());
        }
        
        // ATOMIC COMMIT - All or nothing across ALL column families
        rocksdb::WriteOptions wo;
        wo.sync = true;        // Force fsync of WAL
        wo.disableWAL = false; // We want WAL for recovery
        
        rocksdb::Status status = db->Write(wo, &batch);
        if (!status.ok()) {
            result.error = "Atomic write failed: " + status.ToString();
            return result;
        }
        
        result.success = true;
        std::cout << "✅ Block " << height << " committed atomically across all CFs (" 
                  << result.utxos_created << " UTXOs created, " 
                  << result.utxos_spent << " UTXOs spent)" << std::endl;
        
    } catch (const std::exception& e) {
        result.error = "Exception during atomic commit: " + std::string(e.what());
    }
    
    return result;
}

// State checkpoint operations
std::string BlockchainDB::StateCheckpoint::toJSON() const {
    Json::Value root;
    root["height"] = height;
    root["blockhash"] = blockhash;
    root["state_hash"] = state_hash;
    root["timestamp"] = static_cast<uint64_t>(timestamp);
    root["utxo_count"] = static_cast<uint64_t>(utxo_count);
    root["block_count"] = static_cast<uint64_t>(block_count);
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

BlockchainDB::StateCheckpoint BlockchainDB::StateCheckpoint::fromJSON(const std::string& json) {
    StateCheckpoint checkpoint;
    
    try {
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> json_reader(reader.newCharReader());
        Json::Value root;
        std::string errors;
        
        if (json_reader->parse(json.c_str(), json.c_str() + json.length(), &root, &errors)) {
            checkpoint.height = root.isMember("height") ? root["height"].asUInt() : 0;
            checkpoint.blockhash = root.isMember("blockhash") ? root["blockhash"].asString() : "";
            checkpoint.state_hash = root.isMember("state_hash") ? root["state_hash"].asString() : "";
            checkpoint.timestamp = root.isMember("timestamp") ? root["timestamp"].asUInt() : 0;
            checkpoint.utxo_count = root.isMember("utxo_count") ? root["utxo_count"].asUInt() : 0;
            checkpoint.block_count = root.isMember("block_count") ? root["block_count"].asUInt() : 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception parsing StateCheckpoint JSON: " << e.what() << std::endl;
    }
    
    return checkpoint;
}

std::string BlockchainDB::DatabaseHealth::toJSON() const {
    Json::Value root;
    root["ok"] = ok;
    root["height"] = height;
    root["state_hash"] = state_hash;
    root["recovered_blocks"] = recovered_blocks;
    root["last_error"] = last_error;
    root["last_checkpoint_time"] = static_cast<uint64_t>(last_checkpoint_time);
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

// Calculate state hash for integrity validation
std::string BlockchainDB::calculateStateHash(uint32_t height) {
    if (!initialized || !db) return "";
    
    // Simple state hash: H(height || utxo_count || block_count)
    // In production, this would sample UTXO keys/values
    std::stringstream ss;
    ss << height;
    
    // Count UTXOs (simplified)
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions(), cf_chainstate);
    size_t utxo_count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        if (it->key().ToString().substr(0, 5) == "utxo:") {
            utxo_count++;
        }
    }
    delete it;
    
    ss << ":" << utxo_count << ":" << (height + 1);
    
    // Simple hash (in production, use SHA256)
    std::hash<std::string> hasher;
    size_t hash_val = hasher(ss.str());
    
    std::stringstream hash_ss;
    hash_ss << std::hex << hash_val;
    return hash_ss.str();
}

// Database health monitoring
BlockchainDB::DatabaseHealth BlockchainDB::getDatabaseHealth() {
    DatabaseHealth health;
    health.ok = false;
    health.height = 0;
    health.recovered_blocks = 0;
    health.last_checkpoint_time = 0;
    
    if (!initialized || !db) {
        health.last_error = "Database not initialized";
        return health;
    }
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    try {
        // Get current height
        std::string height_str;
        rocksdb::Status status = db->Get(rocksdb::ReadOptions(), cf_default, "best_height", &height_str);
        if (status.ok()) {
            health.height = std::stoul(height_str);
        }
        
        // Calculate current state hash
        health.state_hash = calculateStateHash(health.height);
        
        // Get latest checkpoint
        StateCheckpoint checkpoint = getLatestStateCheckpoint();
        health.last_checkpoint_time = checkpoint.timestamp;
        
        health.ok = true;
        
    } catch (const std::exception& e) {
        health.last_error = e.what();
    }
    
    return health;
}

// Fault injection for testing (regtest only)
static std::string g_fault_injection_point;

void BlockchainDB::setFaultInjection(const std::string& fault_point) {
    g_fault_injection_point = fault_point;
}

void BlockchainDB::triggerFaultInjection(const std::string& point) {
    if (g_fault_injection_point == point) {
        std::cout << "💥 FAULT INJECTION: Triggering crash at " << point << std::endl;
        _exit(137); // Simulate crash
    }
}

} // namespace Common
} // namespace Dinero
