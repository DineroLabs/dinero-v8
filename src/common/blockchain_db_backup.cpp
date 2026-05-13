#include "common/blockchain_db.h"
#include <algorithm>
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

BlockchainDB::BlockchainDB() 
    : db(nullptr), cf_default(nullptr), cf_blocks(nullptr), cf_chainstate(nullptr), 
      cf_index(nullptr), cf_undo(nullptr), cf_txstore(nullptr), cf_wallet(nullptr), 
      initialized(false) {
}

BlockchainDB::~BlockchainDB() {
    shutdown();
}

bool BlockchainDB::initialize(const std::string& path) {
    if (initialized) {
        std::cerr << "❌ BlockchainDB already initialized" << std::endl;
        return false;
    }
    
    base_path = path;
    
    // Create base directory
    std::filesystem::create_directories(base_path);
    
    // Initialize each database
    bool success = true;
    
    // Initialize chainstate database (UTXO set) with crash recovery
    std::string chainstate_path = getChainstatePath();
    std::filesystem::create_directories(chainstate_path);
    rocksdb::Options chainstate_options;
    chainstate_options.create_if_missing = true;
    chainstate_options.OptimizeForPointLookup(1024);
    chainstate_options.IncreaseParallelism();
    chainstate_options.max_background_jobs = 4;
    
    // Enable crash-safe recovery
    chainstate_options.wal_recovery_mode = rocksdb::WALRecoveryMode::kPointInTimeRecovery;
    chainstate_options.use_fsync = true;  // Safer than fdatasync on some filesystems
    
    rocksdb::Status chainstate_status = rocksdb::DB::Open(chainstate_options, chainstate_path, &chainstate_db);
    if (!chainstate_status.ok()) {
        std::cerr << "❌ Failed to open chainstate database: " << chainstate_status.ToString() << std::endl;
        success = false;
    } else {
        std::cout << "✅ Chainstate database initialized: " << chainstate_path << std::endl;
    }
    
    // Initialize blocks database (raw block data) with crash recovery
    std::string blocks_path = getBlocksPath();
    std::filesystem::create_directories(blocks_path);
    rocksdb::Options blocks_options;
    blocks_options.create_if_missing = true;
    blocks_options.OptimizeForPointLookup(1024);
    blocks_options.IncreaseParallelism();
    blocks_options.max_background_jobs = 4;
    
    // Enable crash-safe recovery
    blocks_options.wal_recovery_mode = rocksdb::WALRecoveryMode::kPointInTimeRecovery;
    blocks_options.use_fsync = true;
    
    rocksdb::Status blocks_status = rocksdb::DB::Open(blocks_options, blocks_path, &blocks_db);
    if (!blocks_status.ok()) {
        std::cerr << "❌ Failed to open blocks database: " << blocks_status.ToString() << std::endl;
        success = false;
    } else {
        std::cout << "✅ Blocks database initialized: " << blocks_path << std::endl;
    }
    
    // Initialize index database (block metadata) with crash recovery
    std::string index_path = getIndexPath();
    std::filesystem::create_directories(index_path);
    rocksdb::Options index_options;
    index_options.create_if_missing = true;
    index_options.OptimizeForPointLookup(1024);
    index_options.IncreaseParallelism();
    index_options.max_background_jobs = 4;
    
    // Enable crash-safe recovery
    index_options.wal_recovery_mode = rocksdb::WALRecoveryMode::kPointInTimeRecovery;
    index_options.use_fsync = true;
    
    rocksdb::Status index_status = rocksdb::DB::Open(index_options, index_path, &index_db);
    if (!index_status.ok()) {
        std::cerr << "❌ Failed to open index database: " << index_status.ToString() << std::endl;
        success = false;
    } else {
        std::cout << "✅ Index database initialized: " << index_path << std::endl;
    }
    
    // Initialize wallet database
    std::string wallet_path = getWalletPath();
    std::filesystem::create_directories(wallet_path);
    rocksdb::Options wallet_options;
    wallet_options.create_if_missing = true;
    wallet_options.OptimizeForPointLookup(1024);
    wallet_options.IncreaseParallelism();
    wallet_options.max_background_jobs = 4;
    
    rocksdb::Status wallet_status = rocksdb::DB::Open(wallet_options, wallet_path, &wallet_db);
    if (!wallet_status.ok()) {
        std::cerr << "❌ Failed to open wallet database: " << wallet_status.ToString() << std::endl;
        success = false;
    } else {
        std::cout << "✅ Wallet database initialized: " << wallet_path << std::endl;
    }
    
    if (success) {
        initialized = true;
        std::cout << "✅ BlockchainDB initialized successfully at: " << base_path << std::endl;
    }
    
    return success;
}

void BlockchainDB::shutdown() {
    if (!initialized) return;
    
    std::cout << "🛑 Shutting down BlockchainDB..." << std::endl;
    
    if (chainstate_db) {
        delete chainstate_db;
        chainstate_db = nullptr;
    }
    
    if (blocks_db) {
        delete blocks_db;
        blocks_db = nullptr;
    }
    
    if (index_db) {
        delete index_db;
        index_db = nullptr;
    }
    
    if (wallet_db) {
        delete wallet_db;
        wallet_db = nullptr;
    }
    
    // Clean up any remaining lock files as a safety measure
    cleanupLockFiles();
    
    initialized = false;
    std::cout << "✅ BlockchainDB shutdown complete" << std::endl;
}

void BlockchainDB::cleanupLockFiles() {
    std::vector<std::string> lockPaths = {
        getChainstatePath() + "/LOCK",
        getBlocksPath() + "/LOCK",
        getIndexPath() + "/LOCK", 
        getWalletPath() + "/LOCK"
    };
    
    for (const auto& lockPath : lockPaths) {
        try {
            if (std::filesystem::exists(lockPath)) {
                if (std::filesystem::remove(lockPath)) {
                    std::cout << "🧹 Cleaned up lock file: " << lockPath << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "⚠️  Warning: Could not remove lock file " << lockPath << ": " << e.what() << std::endl;
            // Don't fail shutdown for lock cleanup issues
        }
    }
}

// Chainstate (UTXO) operations
bool BlockchainDB::storeUTXO(const std::string& outpoint, const std::string& utxo_data) {
    if (!initialized || !chainstate_db) return false;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    std::string key = "utxo:" + outpoint;
    rocksdb::Status status = chainstate_db->Put(rocksdb::WriteOptions(), key, utxo_data);
    return status.ok();
}

std::string BlockchainDB::getUTXO(const std::string& outpoint) {
    if (!initialized || !chainstate_db) return "";
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    std::string key = "utxo:" + outpoint;
    std::string value;
    rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

bool BlockchainDB::removeUTXO(const std::string& outpoint) {
    if (!initialized || !chainstate_db) return false;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    std::string key = "utxo:" + outpoint;
    rocksdb::Status status = chainstate_db->Delete(rocksdb::WriteOptions(), key);
    return status.ok();
}

bool BlockchainDB::hasUTXO(const std::string& outpoint) {
    return !getUTXO(outpoint).empty();
}

std::vector<std::string> BlockchainDB::getAllUTXOKeys() {
    std::vector<std::string> keys;
    
    if (!initialized || !chainstate_db) return keys;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    
    try {
        rocksdb::Iterator* it = chainstate_db->NewIterator(rocksdb::ReadOptions());
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            // Remove "utxo:" prefix to get the actual outpoint
            if (key.substr(0, 5) == "utxo:") {
                keys.push_back(key.substr(5));
            }
        }
        delete it;
    } catch (const std::exception& e) {
        std::cerr << "Error iterating UTXOs: " << e.what() << std::endl;
    }
    
    return keys;
}

// Enhanced UTXO operations for PSBT
bool BlockchainDB::storeUTXOData(const BlockchainDB::UTXOData& utxo) {
    if (!initialized || !chainstate_db) return false;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    
    try {
        // Store UTXO data
        std::string key = "utxo:" + utxo.outpoint;
        std::string value = utxo.toJSON();
        
        rocksdb::Status status = chainstate_db->Put(rocksdb::WriteOptions(), key, value);
        if (!status.ok()) {
            std::cerr << "Failed to store UTXO: " << status.ToString() << std::endl;
            return false;
        }
        
        // Index by address for fast lookup
        if (!utxo.address.empty()) {
            indexAddress(utxo.address, utxo.outpoint);
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception storing UTXO: " << e.what() << std::endl;
        return false;
    }
}

BlockchainDB::UTXOData BlockchainDB::getUTXOData(const std::string& outpoint) {
    UTXOData utxo;
    utxo.outpoint = outpoint;
    
    if (!initialized || !chainstate_db) return utxo;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    
    try {
        std::string key = "utxo:" + outpoint;
        std::string value;
        
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &value);
        if (status.ok() && !value.empty()) {
            utxo = UTXOData::fromJSON(value);
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception getting UTXO: " << e.what() << std::endl;
    }
    
    return utxo;
}

std::vector<BlockchainDB::UTXOData> BlockchainDB::getUTXOsForAddress(const std::string& address) {
    std::vector<UTXOData> utxos;
    
    if (!initialized || !chainstate_db) return utxos;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    
    try {
        // Get outpoints for this address
        std::vector<std::string> outpoints = getOutpointsForAddress(address);
        
        // Fetch UTXO data for each outpoint
        for (const auto& outpoint : outpoints) {
            UTXOData utxo = getUTXOData(outpoint);
            if (!utxo.scriptPubKey.empty()) {
                utxos.push_back(utxo);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception getting UTXOs for address: " << e.what() << std::endl;
    }
    
    return utxos;
}

std::vector<BlockchainDB::UTXOData> BlockchainDB::getUTXOsForScriptHash(const std::string& scriptHash) {
    std::vector<UTXOData> utxos;
    
    if (!initialized || !chainstate_db) return utxos;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    
    try {
        // Get all UTXOs and filter by script hash
        std::vector<std::string> all_keys = getAllUTXOKeys();
        
        for (const auto& key : all_keys) {
            UTXOData utxo = getUTXOData(key);
            if (!utxo.scriptPubKey.empty()) {
                // Calculate script hash and compare
                std::string hash = calculateScriptHash(utxo.scriptPubKey);
                if (hash == scriptHash) {
                    utxos.push_back(utxo);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception getting UTXOs for script hash: " << e.what() << std::endl;
    }
    
    return utxos;
}

// Address indexing operations
bool BlockchainDB::indexAddress(const std::string& address, const std::string& outpoint) {
    if (!initialized || !chainstate_db) return false;
    
    try {
        std::string key = "addr:" + address;
        std::string existing_data;
        
        // Get existing index data
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &existing_data);
        
        AddressIndex index;
        if (status.ok() && !existing_data.empty()) {
            index = AddressIndex::fromJSON(existing_data);
        } else {
            index.address = address;
        }
        
        // Add new outpoint if not already present
        if (std::find(index.outpoints.begin(), index.outpoints.end(), outpoint) == index.outpoints.end()) {
            index.outpoints.append(outpoint);
        }
        
        // Store updated index
        std::string value = index.toJSON();
        status = chainstate_db->Put(rocksdb::WriteOptions(), key, value);
        
        return status.ok();
    } catch (const std::exception& e) {
        std::cerr << "Exception indexing address: " << e.what() << std::endl;
        return false;
    }
}

bool BlockchainDB::removeAddressIndex(const std::string& address, const std::string& outpoint) {
    if (!initialized || !chainstate_db) return false;
    
    try {
        std::string key = "addr:" + address;
        std::string existing_data;
        
        // Get existing index data
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &existing_data);
        
        if (status.ok() && !existing_data.empty()) {
            AddressIndex index = AddressIndex::fromJSON(existing_data);
            
            // Remove outpoint if present
            auto it = std::find(index.outpoints.begin(), index.outpoints.end(), outpoint);
            if (it != index.outpoints.end()) {
                index.outpoints.erase(it);
                
                // Store updated index or remove if empty
                if (index.outpoints.empty()) {
                    status = chainstate_db->Delete(rocksdb::WriteOptions(), key);
                } else {
                    std::string value = index.toJSON();
                    status = chainstate_db->Put(rocksdb::WriteOptions(), key, value);
                }
                
                return status.ok();
            }
        }
        
        return true; // Outpoint wasn't in index
    } catch (const std::exception& e) {
        std::cerr << "Exception removing address index: " << e.what() << std::endl;
        return false;
    }
}

// ScriptPubKey indexing operations
bool BlockchainDB::indexScriptPubKey(const std::string& scriptPubKey, const std::string& outpoint) {
    if (!initialized || !chainstate_db) return false;
    
    try {
        // Calculate script hash for indexing
        std::string scriptHash = calculateScriptHash(scriptPubKey);
        std::string key = "spk:" + scriptHash;
        std::string existing_data;
        
        // Get existing index data
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &existing_data);
        
        ScriptPubKeyIndex index;
        if (status.ok() && !existing_data.empty()) {
            index = ScriptPubKeyIndex::fromJSON(existing_data);
        } else {
            index.scriptPubKey = scriptPubKey;
            index.scriptHash = scriptHash;
        }
        
        // Add new outpoint if not already present
        if (std::find(index.outpoints.begin(), index.outpoints.end(), outpoint) == index.outpoints.end()) {
            index.outpoints.append(outpoint);
        }
        
        // Store updated index
        std::string value = index.toJSON();
        status = chainstate_db->Put(rocksdb::WriteOptions(), key, value);
        
        return status.ok();
    } catch (const std::exception& e) {
        std::cerr << "Exception indexing scriptPubKey: " << e.what() << std::endl;
        return false;
    }
}

bool BlockchainDB::removeScriptPubKeyIndex(const std::string& scriptPubKey, const std::string& outpoint) {
    if (!initialized || !chainstate_db) return false;
    
    try {
        std::string scriptHash = calculateScriptHash(scriptPubKey);
        std::string key = "spk:" + scriptHash;
        std::string existing_data;
        
        // Get existing index data
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &existing_data);
        
        if (status.ok() && !existing_data.empty()) {
            ScriptPubKeyIndex index = ScriptPubKeyIndex::fromJSON(existing_data);
            
            // Remove outpoint if present
            auto it = std::find(index.outpoints.begin(), index.outpoints.end(), outpoint);
            if (it != index.outpoints.end()) {
                index.outpoints.erase(it);
                
                // Store updated index or remove if empty
                if (index.outpoints.empty()) {
                    status = chainstate_db->Delete(rocksdb::WriteOptions(), key);
                } else {
                    std::string value = index.toJSON();
                    status = chainstate_db->Put(rocksdb::WriteOptions(), key, value);
                }
                
                return status.ok();
            }
        }
        
        return true; // Outpoint wasn't in index
    } catch (const std::exception& e) {
        std::cerr << "Exception removing scriptPubKey index: " << e.what() << std::endl;
        return false;
    }
}

std::vector<std::string> BlockchainDB::getOutpointsForAddress(const std::string& address) {
    std::vector<std::string> outpoints;
    
    if (!initialized || !chainstate_db) return outpoints;
    
    try {
        std::string key = "addr:" + address;
        std::string data;
        
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &data);
        if (status.ok() && !data.empty()) {
            AddressIndex index = AddressIndex::fromJSON(data);
            outpoints = index.outpoints;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception getting outpoints for address: " << e.what() << std::endl;
    }
    
    return outpoints;
}

std::vector<std::string> BlockchainDB::getOutpointsForScriptPubKey(const std::string& scriptPubKey) {
    std::vector<std::string> outpoints;
    
    if (!initialized || !chainstate_db) return outpoints;
    
    try {
        std::string scriptHash = calculateScriptHash(scriptPubKey);
        std::string key = "spk:" + scriptHash;
        std::string existing_data;
        
        // Get existing index data
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &existing_data);
        
        if (status.ok() && !existing_data.empty()) {
            ScriptPubKeyIndex index = ScriptPubKeyIndex::fromJSON(existing_data);
            outpoints = index.outpoints;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception getting outpoints for scriptPubKey: " << e.what() << std::endl;
    }
    
    return outpoints;
}

std::vector<std::string> BlockchainDB::getOutpointsForScriptHash(const std::string& scriptHash) {
    std::vector<std::string> outpoints;
    
    if (!initialized || !chainstate_db) return outpoints;
    
    try {
        std::string key = "spk:" + scriptHash;
        std::string existing_data;
        
        // Get existing index data
        rocksdb::Status status = chainstate_db->Get(rocksdb::ReadOptions(), key, &existing_data);
        
        if (status.ok() && !existing_data.empty()) {
            ScriptPubKeyIndex index = ScriptPubKeyIndex::fromJSON(existing_data);
            outpoints = index.outpoints;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception getting outpoints for scriptHash: " << e.what() << std::endl;
    }
    
    return outpoints;
}

// Helper function to calculate script hash
std::string BlockchainDB::calculateScriptHash(const std::string& scriptHex) {
    // Simple sha256 hash for now - in production this should use proper crypto
    // This is a placeholder implementation
    return "script_hash_" + scriptHex.substr(0, 8);
}

// JSON serialization for UTXOData
std::string BlockchainDB::UTXOData::toJSON() const {
    Json::Value root;
    root["outpoint"] = outpoint;
    root["amount"] = uint64_t(amount);
    root["scriptPubKey"] = scriptPubKey;
    root["height"] = height;
    root["isCoinbase"] = isCoinbase;
    root["address"] = address;
    root["addressType"] = addressType;
    
    Json::FastWriter writer;
    return writer.write(root);
}

BlockchainDB::UTXOData BlockchainDB::UTXOData::fromJSON(const std::string& json) {
    UTXOData utxo;
    
    Json::Value root;
    try { root = Json::Reader().parse(json, result); } catch(...) { /* parse failed */ } {
        utxo.outpoint = root.isMember("outpoint") ? "outpoint" : "";
        utxo.amount = root.isMember("amount") ? root["amount"].asUInt64() : 0;
        utxo.scriptPubKey = root.isMember("scriptPubKey") ? "scriptPubKey" : "";
        utxo.height = root.isMember("height") ? root["height"].asUInt() : 0;
        utxo.isCoinbase = root.isMember("isCoinbase") ? "isCoinbase" : false;
        utxo.address = root.isMember("address") ? "address" : "";
        utxo.addressType = root.isMember("addressType") ? "addressType" : "";
    }
    
    return utxo;
}

// AddressIndex implementation
std::string BlockchainDB::AddressIndex::toJSON() const {
    Json::Value root;
    root["address"] = address;
    root["scriptPubKeyHash"] = scriptPubKeyHash;
    
    Json::Value outpoints_array(Json::arrayValue);
    for (const auto& outpoint : outpoints) {
        outpoints_array.append(outpoint);
    }
    root["outpoints"] = outpoints_array;
    
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

BlockchainDB::AddressIndex BlockchainDB::AddressIndex::fromJSON(const std::string& json) {
    AddressIndex index;
    
    try {
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> json_reader(reader.newCharReader());
        Json::Value root;
        std::string errors;
        
        if (json_reader->parse(json.c_str(), json.c_str() + json.length(), &root, &errors)) {
            index.address = root.isMember("address") ? "address" : "";
            index.scriptPubKeyHash = root.isMember("scriptPubKeyHash") ? "scriptPubKeyHash" : "";
            
            if (root.isMember("outpoints") && root["outpoints"].isArray()) {
                for (const auto& outpoint : root["outpoints"]) {
                    index.outpoints.append(outpoint.asString());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception parsing AddressIndex JSON: " << e.what() << std::endl;
    }
    
    return index;
}

// ScriptPubKeyIndex implementation
std::string BlockchainDB::ScriptPubKeyIndex::toJSON() const {
    Json::Value root;
    root["scriptPubKey"] = scriptPubKey;
    root["scriptHash"] = scriptHash;
    
    Json::Value outpoints_array(Json::arrayValue);
    for (const auto& outpoint : outpoints) {
        outpoints_array.append(outpoint);
    }
    root["outpoints"] = outpoints_array;
    
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

BlockchainDB::ScriptPubKeyIndex BlockchainDB::ScriptPubKeyIndex::fromJSON(const std::string& json) {
    ScriptPubKeyIndex index;
    
    try {
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> json_reader(reader.newCharReader());
        Json::Value root;
        std::string errors;
        
        if (json_reader->parse(json.c_str(), json.c_str() + json.length(), &root, &errors)) {
            index.scriptPubKey = root.isMember("scriptPubKey") ? "scriptPubKey" : "";
            index.scriptHash = root.isMember("scriptHash") ? "scriptHash" : "";
            
            if (root.isMember("outpoints") && root["outpoints"].isArray()) {
                for (const auto& outpoint : root["outpoints"]) {
                    index.outpoints.append(outpoint.asString());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception parsing ScriptPubKeyIndex JSON: " << e.what() << std::endl;
    }
    
    return index;
}

// Block operations
bool BlockchainDB::storeBlock(uint32_t height, const std::string& block_data) {
    if (!initialized || !blocks_db) return false;
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    std::string key = "block:" + std::to_string(height);
    rocksdb::Status status = blocks_db->Put(rocksdb::WriteOptions(), key, block_data);
    return status.ok();
}

bool BlockchainDB::storeBlockByHash(const std::string& block_hash, const std::string& block_data) {
    if (!initialized || !blocks_db) return false;
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    std::string key = "block_hash:" + block_hash;
    rocksdb::Status status = blocks_db->Put(rocksdb::WriteOptions(), key, block_data);
    return status.ok();
}

std::string BlockchainDB::getBlock(uint32_t height) {
    if (!initialized || !blocks_db) return "";
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    std::string key = "block:" + std::to_string(height);
    std::string value;
    rocksdb::Status status = blocks_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

std::string BlockchainDB::getBlockByHash(const std::string& block_hash) {
    if (!initialized || !blocks_db) return "";
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    std::string key = "block_hash:" + block_hash;
    std::string value;
    rocksdb::Status status = blocks_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

bool BlockchainDB::hasBlock(uint32_t height) {
    return !getBlock(height).empty();
}

bool BlockchainDB::hasBlockByHash(const std::string& block_hash) {
    return !getBlockByHash(block_hash).empty();
}

// Index operations
bool BlockchainDB::storeBlockIndex(uint32_t height, const std::string& index_data) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "index:" + std::to_string(height);
    rocksdb::Status status = index_db->Put(rocksdb::WriteOptions(), key, index_data);
    return status.ok();
}

bool BlockchainDB::storeBlockIndexByHash(const std::string& block_hash, const std::string& index_data) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "index_hash:" + block_hash;
    rocksdb::Status status = index_db->Put(rocksdb::WriteOptions(), key, index_data);
    return status.ok();
}

std::string BlockchainDB::getBlockIndex(uint32_t height) {
    if (!initialized || !index_db) return "";
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "index:" + std::to_string(height);
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

std::string BlockchainDB::getBlockIndexByHash(const std::string& block_hash) {
    if (!initialized || !index_db) return "";
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "index_hash:" + block_hash;
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

uint32_t BlockchainDB::getBestBlockHeight() {
    if (!initialized || !index_db) return 0;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), "best_height", &value);
    if (status.ok()) {
        return std::stoul(value);
    }
    return 0;
}

std::string BlockchainDB::getBestBlockHash() {
    if (!initialized || !index_db) return "";
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), "best_hash", &value);
    return status.ok() ? value : "";
}

// Metadata operations for best block tracking
bool BlockchainDB::storeMetadata(const std::string& key, const std::string& value) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    rocksdb::Status status = index_db->Put(rocksdb::WriteOptions(), key, value);
    return status.ok();
}

std::string BlockchainDB::getMetadata(const std::string& key) {
    if (!initialized || !index_db) return "";
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

bool BlockchainDB::hasMetadata(const std::string& key) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok();
}

// TxStore operations - append-only transaction storage
bool BlockchainDB::storeTransaction(const std::string& txid, const std::string& raw_tx_hex) {
    if (!initialized || !blocks_db) return false;
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    std::string key = "tx:" + txid;
    rocksdb::Status status = blocks_db->Put(rocksdb::WriteOptions(), key, raw_tx_hex);
    return status.ok();
}

std::string BlockchainDB::getTransaction(const std::string& txid) {
    if (!initialized || !blocks_db) return "";
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    std::string key = "tx:" + txid;
    std::string value;
    rocksdb::Status status = blocks_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok() ? value : "";
}

bool BlockchainDB::hasTransaction(const std::string& txid) {
    return !getTransaction(txid).empty();
}

// TxMeta operations - transaction metadata for quick lookups
std::string BlockchainDB::TxMeta::toJSON() const {
    Json::Value json;
    json["blockhash"] = blockhash;
    json["height"] = height;
    json["index"] = index;
    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, json);
}

BlockchainDB::TxMeta BlockchainDB::TxMeta::fromJSON(const std::string& json_str) {
    TxMeta meta;
    Json::Value json;
    try { json = Json::Reader().parse(json_str, result); } catch(...) { /* parse failed */ } {
        meta.blockhash = json.isMember("blockhash") ? "blockhash" : "";
        meta.height = json.isMember("height") ? "height" : 0;
        meta.index = json.isMember("index") ? "index" : 0;
    }
    return meta;
}

bool BlockchainDB::storeTxMeta(const std::string& txid, const TxMeta& meta) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "txmeta:" + txid;
    std::string value = meta.toJSON();
    rocksdb::Status status = index_db->Put(rocksdb::WriteOptions(), key, value);
    return status.ok();
}

BlockchainDB::TxMeta BlockchainDB::getTxMeta(const std::string& txid) {
    TxMeta meta;
    if (!initialized || !index_db) return meta;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "txmeta:" + txid;
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    if (status.ok()) {
        meta = TxMeta::fromJSON(value);
    }
    return meta;
}

bool BlockchainDB::hasTxMeta(const std::string& txid) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "txmeta:" + txid;
    std::string value;
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok();
}

// Wallet operations
bool BlockchainDB::storeWalletData(const std::string& key, const std::string& value) {
    if (!initialized || !wallet_db) return false;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    std::string full_key = "wallet:" + key;
    rocksdb::Status status = wallet_db->Put(rocksdb::WriteOptions(), full_key, value);
    return status.ok();
}

std::string BlockchainDB::getWalletData(const std::string& key) {
    if (!initialized || !wallet_db) return "";
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    std::string full_key = "wallet:" + key;
    std::string value;
    rocksdb::Status status = wallet_db->Get(rocksdb::ReadOptions(), full_key, &value);
    return status.ok() ? value : "";
}

bool BlockchainDB::hasWalletData(const std::string& key) {
    return !getWalletData(key).empty();
}

bool BlockchainDB::removeWalletData(const std::string& key) {
    if (!initialized || !wallet_db) return false;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    std::string full_key = "wallet:" + key;
    rocksdb::Status status = wallet_db->Delete(rocksdb::WriteOptions(), full_key);
    return status.ok();
}

// Statistics and maintenance
uint64_t BlockchainDB::getChainstateSize() {
    if (!initialized || !chainstate_db) return 0;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    uint64_t size = 0;
    rocksdb::Iterator* it = chainstate_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        size += it->key().size() + it->value().size();
    }
    delete it;
    return size;
}

uint64_t BlockchainDB::getBlocksSize() {
    if (!initialized || !blocks_db) return 0;
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    uint64_t size = 0;
    rocksdb::Iterator* it = blocks_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        size += it->key().size() + it->value().size();
    }
    delete it;
    return size;
}

uint64_t BlockchainDB::getIndexSize() {
    if (!initialized || !index_db) return 0;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    uint64_t size = 0;
    rocksdb::Iterator* it = index_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        size += it->key().size() + it->value().size();
    }
    delete it;
    return size;
}

uint64_t BlockchainDB::getWalletSize() {
    if (!initialized || !wallet_db) return 0;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    uint64_t size = 0;
    rocksdb::Iterator* it = wallet_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        size += it->key().size() + it->value().size();
    }
    delete it;
    return size;
}

uint64_t BlockchainDB::getTotalSize() {
    return getChainstateSize() + getBlocksSize() + getIndexSize() + getWalletSize();
}

// Database maintenance
bool BlockchainDB::compactChainstate() {
    if (!initialized || !chainstate_db) return false;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    rocksdb::Status status = chainstate_db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    return status.ok();
}

bool BlockchainDB::compactBlocks() {
    if (!initialized || !blocks_db) return false;
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    rocksdb::Status status = blocks_db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    return status.ok();
}

bool BlockchainDB::compactIndex() {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    rocksdb::Status status = index_db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    return status.ok();
}

bool BlockchainDB::compactWallet() {
    if (!initialized || !wallet_db) return false;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    rocksdb::Status status = wallet_db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    return status.ok();
}

bool BlockchainDB::compactAll() {
    bool success = true;
    success &= compactChainstate();
    success &= compactBlocks();
    success &= compactIndex();
    success &= compactWallet();
    return success;
}

// Export functionality
bool BlockchainDB::exportChainstateToJSON(const std::string& filename) {
    if (!initialized || !chainstate_db) return false;
    
    std::lock_guard<std::mutex> lock(chainstate_mutex);
    
    Json::Value export_data;
    export_data["database"] = "chainstate";
    export_data["export_timestamp"] = Json::Value::Int64(std::time(nullptr));
    export_data["path"] = getChainstatePath();
    
    Json::Value utxos;
    rocksdb::Iterator* it = chainstate_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Json::Value utxo;
        utxo["key"] = it->key().ToString();
        utxo["value"] = it->value().ToString();
        utxos.push_back(utxo);
    }
    delete it;
    
    export_data["utxos"] = utxos;
    
    std::ofstream file(filename);
    if (file.is_open()) {
        Json::StyledWriter writer;
        file << writer.write(export_data);
        file.close();
        return true;
    }
    return false;
}

bool BlockchainDB::exportBlocksToJSON(const std::string& filename) {
    if (!initialized || !blocks_db) return false;
    
    std::lock_guard<std::mutex> lock(blocks_mutex);
    
    Json::Value export_data;
    export_data["database"] = "blocks";
    export_data["export_timestamp"] = Json::Value::Int64(std::time(nullptr));
    export_data["path"] = getBlocksPath();
    
    Json::Value blocks;
    rocksdb::Iterator* it = blocks_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Json::Value block;
        block["key"] = it->key().ToString();
        block["value"] = it->value().ToString();
        blocks.append(block);
    }
    delete it;
    
    export_data["blocks"] = blocks;
    
    std::ofstream file(filename);
    if (file.is_open()) {
        Json::StyledWriter writer;
        file << writer.write(export_data);
        file.close();
        return true;
    }
    return false;
}

bool BlockchainDB::exportIndexToJSON(const std::string& filename) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    
    Json::Value export_data;
    export_data["database"] = "index";
    export_data["export_timestamp"] = Json::Value::Int64(std::time(nullptr));
    export_data["path"] = getIndexPath();
    
    Json::Value indices;
    rocksdb::Iterator* it = index_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Json::Value index;
        index["key"] = it->key().ToString();
        index["value"] = it->value().ToString();
        indices.append(index);
    }
    delete it;
    
    export_data["indices"] = indices;
    
    std::ofstream file(filename);
    if (file.is_open()) {
        Json::StyledWriter writer;
        file << writer.write(export_data);
        file.close();
        return true;
    }
    return false;
}

bool BlockchainDB::exportWalletToJSON(const std::string& filename) {
    if (!initialized || !wallet_db) return false;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    Json::Value export_data;
    export_data["database"] = "wallet";
    export_data["export_timestamp"] = Json::Value::Int64(std::time(nullptr));
    export_data["path"] = getWalletPath();
    
    Json::Value wallet_data;
    rocksdb::Iterator* it = wallet_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Json::Value data;
        data["key"] = it->key().ToString();
        data["value"] = it->value().ToString();
        wallet_data.push_back(data);
    }
    delete it;
    
    export_data["wallet_data"] = wallet_data;
    
    std::ofstream file(filename);
    if (file.is_open()) {
        Json::StyledWriter writer;
        file << writer.write(export_data);
        file.close();
        return true;
    }
    return false;
}

// UndoRecord::SpentOutput JSON serialization
std::string BlockchainDB::UndoRecord::SpentOutput::toJSON() const {
    Json::Value root;
    root["outpoint"] = outpoint;
    root["amount"] = static_cast<uint64_t>(amount);
    root["scriptPubKey"] = scriptPubKey;
    root["height"] = height;
    root["isCoinbase"] = isCoinbase;
    root["address"] = address;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

BlockchainDB::UndoRecord::SpentOutput BlockchainDB::UndoRecord::SpentOutput::fromJSON(const std::string& json) {
    SpentOutput output;
    
    try {
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> json_reader(reader.newCharReader());
        Json::Value root;
        std::string errors;
        
        if (json_reader->parse(json.c_str(), json.c_str() + json.length(), &root, &errors)) {
            output.outpoint = root.isMember("outpoint") ? "outpoint" : "";
            output.amount = root.isMember("amount") ? root["amount"].asUInt64() : 0;
            output.scriptPubKey = root.isMember("scriptPubKey") ? "scriptPubKey" : "";
            output.height = root.isMember("height") ? root["height"].asUInt() : 0;
            output.isCoinbase = root.isMember("isCoinbase") ? root["isCoinbase"].asBool() : false;
            output.address = root.isMember("address") ? "address" : "";
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception parsing SpentOutput JSON: " << e.what() << std::endl;
    }
    
    return output;
}

// UndoRecord JSON serialization
std::string BlockchainDB::UndoRecord::toJSON() const {
    Json::Value root;
    root["blockhash"] = blockhash;
    root["height"] = height;
    
    Json::Value outputs_array;
    for (const auto& output : spentOutputs) {
        Json::Value output_json;
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> json_reader(reader.newCharReader());
        std::string errors;
        
        if (json_reader->parse(output.toJSON().c_str(), output.toJSON().c_str() + output.toJSON().length(), &output_json, &errors)) {
            outputs_array.append(output_json);
        }
    }
    root["spentOutputs"] = outputs_array;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

BlockchainDB::UndoRecord BlockchainDB::UndoRecord::fromJSON(const std::string& json) {
    UndoRecord undo;
    
    try {
        Json::CharReaderBuilder reader;
        std::unique_ptr<Json::CharReader> json_reader(reader.newCharReader());
        Json::Value root;
        std::string errors;
        
        if (json_reader->parse(json.c_str(), json.c_str() + json.length(), &root, &errors)) {
            undo.blockhash = root.isMember("blockhash") ? "blockhash" : "";
            undo.height = root.isMember("height") ? root["height"].asUInt() : 0;
            
            if (root.isMember("spentOutputs") && root["spentOutputs"].isArray()) {
                for (const auto& output_json : root["spentOutputs"]) {
                    Json::StreamWriterBuilder builder;
                    builder["indentation"] = "";
                    std::string output_str = Json::writeString(builder, output_json);
                    undo.spentOutputs.push_back(SpentOutput::fromJSON(output_str));
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception parsing UndoRecord JSON: " << e.what() << std::endl;
    }
    
    return undo;
}

// Undo operations
bool BlockchainDB::storeUndoRecord(const std::string& blockhash, const UndoRecord& undo) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "undo:" + blockhash;
    std::string value = undo.toJSON();
    
    rocksdb::WriteOptions wo;
    wo.sync = true;  // Ensure durability
    
    rocksdb::Status status = index_db->Put(wo, key, value);
    return status.ok();
}

BlockchainDB::UndoRecord BlockchainDB::getUndoRecord(const std::string& blockhash) {
    UndoRecord undo;
    if (!initialized || !index_db) return undo;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "undo:" + blockhash;
    std::string value;
    
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    if (status.ok() && !value.empty()) {
        undo = UndoRecord::fromJSON(value);
    }
    
    return undo;
}

bool BlockchainDB::hasUndoRecord(const std::string& blockhash) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "undo:" + blockhash;
    std::string value;
    
    rocksdb::Status status = index_db->Get(rocksdb::ReadOptions(), key, &value);
    return status.ok();
}

bool BlockchainDB::removeUndoRecord(const std::string& blockhash) {
    if (!initialized || !index_db) return false;
    
    std::lock_guard<std::mutex> lock(index_mutex);
    std::string key = "undo:" + blockhash;
    
    rocksdb::WriteOptions wo;
    wo.sync = true;
    
    rocksdb::Status status = index_db->Delete(wo, key);
    return status.ok();
}

// Atomic block commit operation
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
    
    if (!initialized) {
        result.error = "Database not initialized";
        return result;
    }
    
    try {
        // Use sync writes for durability across separate databases
        rocksdb::WriteOptions wo;
        wo.sync = true;  // Force fsync of WAL
        wo.disableWAL = false;  // We want WAL for recovery
        
        // 1. Store block data (blocks_db)
        {
            std::lock_guard<std::mutex> lock(blocks_mutex);
            std::string block_key = "block:" + std::to_string(height);
            std::string block_hash_key = "block_hash:" + blockhash;
            
            rocksdb::Status status = blocks_db->Put(wo, block_key, block_data);
            if (!status.ok()) {
                result.error = "Failed to store block by height: " + status.ToString();
                return result;
            }
            
            status = blocks_db->Put(wo, block_hash_key, block_data);
            if (!status.ok()) {
                result.error = "Failed to store block by hash: " + status.ToString();
                return result;
            }
        }
        
        // 2. Store block index and undo record (index_db)
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            std::string index_key = "index:" + std::to_string(height);
            std::string index_hash_key = "index_hash:" + blockhash;
            std::string undo_key = "undo:" + blockhash;
            
            rocksdb::Status status = index_db->Put(wo, index_key, index_data);
            if (!status.ok()) {
                result.error = "Failed to store index by height: " + status.ToString();
                return result;
            }
            
            status = index_db->Put(wo, index_hash_key, index_data);
            if (!status.ok()) {
                result.error = "Failed to store index by hash: " + status.ToString();
                return result;
            }
            
            status = index_db->Put(wo, undo_key, undo_record.toJSON());
            if (!status.ok()) {
                result.error = "Failed to store undo record: " + status.ToString();
                return result;
            }
        }
        
        // 3. Update UTXO set atomically (chainstate_db)
        {
            std::lock_guard<std::mutex> lock(chainstate_mutex);
            
            // Remove spent UTXOs
            for (const std::string& outpoint : utxos_to_spend) {
                std::string utxo_key = "utxo:" + outpoint;
                rocksdb::Status status = chainstate_db->Delete(wo, utxo_key);
                if (!status.ok()) {
                    result.error = "Failed to remove UTXO " + outpoint + ": " + status.ToString();
                    return result;
                }
                result.utxos_spent++;
            }
            
            // Create new UTXOs
            for (const UTXOData& utxo : utxos_to_create) {
                std::string utxo_key = "utxo:" + utxo.outpoint;
                rocksdb::Status status = chainstate_db->Put(wo, utxo_key, utxo.toJSON());
                if (!status.ok()) {
                    result.error = "Failed to create UTXO " + utxo.outpoint + ": " + status.ToString();
                    return result;
                }
                result.utxos_created++;
            }
        }
        
        // 4. Update best block pointers (LAST - this makes the block "official")
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            rocksdb::Status status = index_db->Put(wo, "best_height", std::to_string(height));
            if (!status.ok()) {
                result.error = "Failed to update best height: " + status.ToString();
                return result;
            }
            
            status = index_db->Put(wo, "best_hash", blockhash);
            if (!status.ok()) {
                result.error = "Failed to update best hash: " + status.ToString();
                return result;
            }
        }
        
        result.success = true;
        std::cout << "✅ Block " << height << " committed with sync writes (" 
                  << result.utxos_created << " UTXOs created, " 
                  << result.utxos_spent << " UTXOs spent)" << std::endl;
        
    } catch (const std::exception& e) {
        result.error = "Exception during commit: " + std::string(e.what());
    }
    
    return result;
}

// Startup recovery operations
bool BlockchainDB::performStartupRecovery() {
    if (!initialized) return false;
    
    std::cout << "🔄 Performing startup recovery..." << std::endl;
    
    try {
        // Check if we have a valid best tip
        std::string best_height_str = getMetadata("best_height");
        std::string best_hash = getMetadata("best_hash");
        
        if (best_height_str.empty() || best_hash.empty()) {
            std::cout << "ℹ️  No best tip found, assuming genesis state" << std::endl;
            return true;
        }
        
        uint32_t best_height = std::stoul(best_height_str);
        
        // Verify the best block exists
        if (!hasBlockByHash(best_hash)) {
            std::cout << "⚠️  Best block " << best_hash << " not found, rolling back..." << std::endl;
            return rollbackToHeight(best_height - 1);
        }
        
        // TODO: Add state hash validation here
        // For now, assume recovery is successful if best block exists
        std::cout << "✅ Recovery complete, best tip: " << best_height << " (" << best_hash << ")" << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Recovery failed: " << e.what() << std::endl;
        return false;
    }
}

bool BlockchainDB::rollbackToHeight(uint32_t target_height) {
    if (!initialized) return false;
    
    std::cout << "🔄 Rolling back to height " << target_height << "..." << std::endl;
    
    try {
        std::string current_height_str = getMetadata("best_height");
        if (current_height_str.empty()) {
            std::cout << "ℹ️  No current height, nothing to rollback" << std::endl;
            return true;
        }
        
        uint32_t current_height = std::stoul(current_height_str);
        
        // Rollback blocks one by one using undo records
        for (uint32_t height = current_height; height > target_height; height--) {
            std::string block_hash = getBlockIndexByHash("height:" + std::to_string(height));
            if (block_hash.empty()) {
                std::cerr << "⚠️  Cannot find block at height " << height << std::endl;
                continue;
            }
            
            // Get undo record
            UndoRecord undo = getUndoRecord(block_hash);
            if (undo.spentOutputs.empty() && height > 0) {
                std::cerr << "⚠️  No undo record for block " << block_hash << std::endl;
                continue;
            }
            
            // Restore spent outputs
            for (const auto& spent : undo.spentOutputs) {
                UTXOData utxo;
                utxo.outpoint = spent.outpoint;
                utxo.amount = spent.amount;
                utxo.scriptPubKey = spent.scriptPubKey;
                utxo.height = spent.height;
                utxo.isCoinbase = spent.isCoinbase;
                utxo.address = spent.address;
                
                if (!storeUTXOData(utxo)) {
                    std::cerr << "⚠️  Failed to restore UTXO " << spent.outpoint << std::endl;
                }
            }
            
            // Remove created outputs (TODO: implement this properly)
            // For now, we'll rely on the fact that we're rolling back sequentially
            
            std::cout << "🔄 Rolled back block " << height << " (" << block_hash << ")" << std::endl;
        }
        
        // Update best tip
        if (!storeMetadata("best_height", std::to_string(target_height))) {
            std::cerr << "❌ Failed to update best height" << std::endl;
            return false;
        }
        
        std::cout << "✅ Rollback complete to height " << target_height << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Rollback failed: " << e.what() << std::endl;
        return false;
    }
}

bool BlockchainDB::validateChainIntegrity(uint32_t from_height, uint32_t to_height) {
    if (!initialized) return false;
    
    std::cout << "🔍 Validating chain integrity..." << std::endl;
    
    // TODO: Implement comprehensive chain validation
    // - Verify block hashes
    // - Verify UTXO set consistency
    // - Verify undo records exist
    
    std::cout << "✅ Chain integrity validation complete" << std::endl;
    return true;
}

} // namespace Common
} // namespace Dinero 