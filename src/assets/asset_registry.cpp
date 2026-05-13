/**
 * Phase 30: Taproot Asset Layer - Asset Registry Implementation
 */

#include "assets/asset_registry.h"
#include "crypto/sha256.h"
#include <sqlite3.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <ctime>

namespace dinero {
namespace assets {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoi(byteString, nullptr, 16)));
    }
    return bytes;
}

} // anonymous namespace

// ============================================================================
// Global Registry
// ============================================================================

static std::unique_ptr<AssetRegistry> g_asset_registry;

AssetRegistry& GetAssetRegistry() {
    if (!g_asset_registry) {
        throw std::runtime_error("Asset registry not initialized");
    }
    return *g_asset_registry;
}

void InitAssetRegistry(const std::string& db_path) {
    g_asset_registry = std::make_unique<AssetRegistry>(db_path);
}

void ShutdownAssetRegistry() {
    g_asset_registry.reset();
}

// ============================================================================
// AssetRegistry Implementation
// ============================================================================

AssetRegistry::AssetRegistry(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to open asset registry database: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    initSchema();
    prepareStatements();
}

AssetRegistry::~AssetRegistry() {
    finalizeStatements();
    if (db_) {
        sqlite3_close(db_);
    }
}

void AssetRegistry::initSchema() {
    const char* schema = R"(
        -- Asset definitions
        CREATE TABLE IF NOT EXISTS assets (
            asset_id BLOB PRIMARY KEY NOT NULL,
            issuer_pubkey BLOB NOT NULL,
            creation_txid TEXT NOT NULL,
            creation_vout INTEGER NOT NULL,
            creation_height INTEGER NOT NULL,
            metadata_json TEXT,
            supply_config BLOB,
            created_at INTEGER NOT NULL,
            total_minted INTEGER DEFAULT 0,
            total_burned INTEGER DEFAULT 0
        );

        -- Asset UTXOs
        CREATE TABLE IF NOT EXISTS asset_utxos (
            txid TEXT NOT NULL,
            vout INTEGER NOT NULL,
            asset_id BLOB NOT NULL,
            amount INTEGER NOT NULL,
            state_hash BLOB,
            script_pubkey BLOB,
            owner_address TEXT,
            height INTEGER DEFAULT 0,
            timestamp INTEGER DEFAULT 0,
            is_spent INTEGER DEFAULT 0,
            spending_txid TEXT,
            spending_input INTEGER,
            PRIMARY KEY (txid, vout)
        );

        CREATE INDEX IF NOT EXISTS idx_utxos_asset ON asset_utxos(asset_id);
        CREATE INDEX IF NOT EXISTS idx_utxos_owner ON asset_utxos(owner_address);
        CREATE INDEX IF NOT EXISTS idx_utxos_unspent ON asset_utxos(is_spent, asset_id);

        -- Supply tracking
        CREATE TABLE IF NOT EXISTS supply_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            asset_id BLOB NOT NULL,
            event_type TEXT NOT NULL,  -- 'mint' or 'burn'
            amount INTEGER NOT NULL,
            txid TEXT NOT NULL,
            block_height INTEGER NOT NULL,
            timestamp INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_supply_asset ON supply_events(asset_id);

        -- Transfer history
        CREATE TABLE IF NOT EXISTS transfers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            txid TEXT NOT NULL,
            asset_id BLOB NOT NULL,
            amount INTEGER NOT NULL,
            from_address TEXT,
            to_address TEXT,
            block_height INTEGER NOT NULL,
            timestamp INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_transfers_txid ON transfers(txid);
        CREATE INDEX IF NOT EXISTS idx_transfers_from ON transfers(from_address);
        CREATE INDEX IF NOT EXISTS idx_transfers_to ON transfers(to_address);

        -- Block index tracking
        CREATE TABLE IF NOT EXISTS block_index (
            height INTEGER PRIMARY KEY,
            block_hash TEXT NOT NULL,
            processed_at INTEGER NOT NULL
        );
    )";

    char* error = nullptr;
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string err_msg = error ? error : "Unknown error";
        sqlite3_free(error);
        throw std::runtime_error("Failed to create asset registry schema: " + err_msg);
    }
}

void AssetRegistry::prepareStatements() {
    // Register asset
    sqlite3_prepare_v2(db_,
        "INSERT INTO assets (asset_id, issuer_pubkey, creation_txid, creation_vout, "
        "creation_height, metadata_json, supply_config, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt_register_asset_, nullptr);

    // Get genesis
    sqlite3_prepare_v2(db_,
        "SELECT asset_id, issuer_pubkey, creation_txid, creation_vout, creation_height, "
        "metadata_json, supply_config, created_at, total_minted, total_burned "
        "FROM assets WHERE asset_id = ?",
        -1, &stmt_get_genesis_, nullptr);

    // Add UTXO
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO asset_utxos (txid, vout, asset_id, amount, state_hash, "
        "script_pubkey, owner_address, height, timestamp, is_spent) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0)",
        -1, &stmt_add_utxo_, nullptr);

    // Mark spent
    sqlite3_prepare_v2(db_,
        "UPDATE asset_utxos SET is_spent = 1, spending_txid = ?, spending_input = ? "
        "WHERE txid = ? AND vout = ?",
        -1, &stmt_mark_spent_, nullptr);

    // Get UTXO
    sqlite3_prepare_v2(db_,
        "SELECT txid, vout, asset_id, amount, state_hash, script_pubkey, owner_address, "
        "height, timestamp, is_spent, spending_txid, spending_input "
        "FROM asset_utxos WHERE txid = ? AND vout = ?",
        -1, &stmt_get_utxo_, nullptr);

    // Record mint
    sqlite3_prepare_v2(db_,
        "INSERT INTO supply_events (asset_id, event_type, amount, txid, block_height, timestamp) "
        "VALUES (?, 'mint', ?, ?, ?, ?)",
        -1, &stmt_record_mint_, nullptr);

    // Record burn
    sqlite3_prepare_v2(db_,
        "INSERT INTO supply_events (asset_id, event_type, amount, txid, block_height, timestamp) "
        "VALUES (?, 'burn', ?, ?, ?, ?)",
        -1, &stmt_record_burn_, nullptr);

    // Record transfer
    sqlite3_prepare_v2(db_,
        "INSERT INTO transfers (txid, asset_id, amount, from_address, to_address, block_height, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt_record_transfer_, nullptr);
}

void AssetRegistry::finalizeStatements() {
    if (stmt_register_asset_) sqlite3_finalize(stmt_register_asset_);
    if (stmt_get_genesis_) sqlite3_finalize(stmt_get_genesis_);
    if (stmt_add_utxo_) sqlite3_finalize(stmt_add_utxo_);
    if (stmt_mark_spent_) sqlite3_finalize(stmt_mark_spent_);
    if (stmt_get_utxo_) sqlite3_finalize(stmt_get_utxo_);
    if (stmt_record_mint_) sqlite3_finalize(stmt_record_mint_);
    if (stmt_record_burn_) sqlite3_finalize(stmt_record_burn_);
    if (stmt_record_transfer_) sqlite3_finalize(stmt_record_transfer_);
}

// ============================================================================
// Asset Registration
// ============================================================================

bool AssetRegistry::registerAsset(const AssetGenesis& genesis) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_register_asset_);

    sqlite3_bind_blob(stmt_register_asset_, 1, genesis.asset_id.data(), 32, SQLITE_STATIC);
    sqlite3_bind_blob(stmt_register_asset_, 2, genesis.issuer_pubkey.data(), 32, SQLITE_STATIC);
    sqlite3_bind_text(stmt_register_asset_, 3, genesis.creation_txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_register_asset_, 4, genesis.creation_output_index);
    sqlite3_bind_int(stmt_register_asset_, 5, genesis.creation_height);

    std::string meta_json = genesis.metadata.toJSON();
    sqlite3_bind_text(stmt_register_asset_, 6, meta_json.c_str(), -1, SQLITE_STATIC);

    auto supply_data = genesis.supply.serialize();
    sqlite3_bind_blob(stmt_register_asset_, 7, supply_data.data(), supply_data.size(), SQLITE_STATIC);

    sqlite3_bind_int64(stmt_register_asset_, 8, genesis.created_at);

    int rc = sqlite3_step(stmt_register_asset_);
    return rc == SQLITE_DONE;
}

std::optional<AssetGenesis> AssetRegistry::getAssetGenesis(const AssetID& asset_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_get_genesis_);
    sqlite3_bind_blob(stmt_get_genesis_, 1, asset_id.data(), 32, SQLITE_STATIC);

    if (sqlite3_step(stmt_get_genesis_) != SQLITE_ROW) {
        return std::nullopt;
    }

    AssetGenesis genesis;

    // Asset ID
    const uint8_t* aid = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_get_genesis_, 0));
    std::copy(aid, aid + 32, genesis.asset_id.begin());

    // Issuer pubkey
    const uint8_t* pk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_get_genesis_, 1));
    std::copy(pk, pk + 32, genesis.issuer_pubkey.begin());

    // Creation info
    genesis.creation_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_genesis_, 2));
    genesis.creation_output_index = sqlite3_column_int(stmt_get_genesis_, 3);
    genesis.creation_height = sqlite3_column_int(stmt_get_genesis_, 4);

    // Metadata
    const char* meta_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_genesis_, 5));
    if (meta_json) {
        auto meta = AssetMetadata::fromJSON(meta_json);
        if (meta) genesis.metadata = *meta;
    }

    // Supply config
    const uint8_t* supply_data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_get_genesis_, 6));
    int supply_len = sqlite3_column_bytes(stmt_get_genesis_, 6);
    if (supply_data && supply_len > 0) {
        std::vector<uint8_t> supply_bytes(supply_data, supply_data + supply_len);
        auto supply = AssetSupplyConfig::deserialize(supply_bytes);
        if (supply) genesis.supply = *supply;
    }

    genesis.created_at = sqlite3_column_int64(stmt_get_genesis_, 7);

    return genesis;
}

std::optional<AssetSummary> AssetRegistry::getAssetSummary(const AssetID& asset_id) {
    auto genesis = getAssetGenesis(asset_id);
    if (!genesis) return std::nullopt;

    AssetSummary summary;
    summary.asset_id = genesis->asset_id;
    summary.metadata = genesis->metadata;
    summary.supply_config = genesis->supply;
    summary.creation_txid = genesis->creation_txid;
    summary.creation_height = genesis->creation_height;
    summary.created_at = genesis->created_at;

    // Get supply stats
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT SUM(CASE WHEN event_type='mint' THEN amount ELSE 0 END), "
        "SUM(CASE WHEN event_type='burn' THEN amount ELSE 0 END) "
        "FROM supply_events WHERE asset_id = ?",
        -1, &stmt, nullptr);

    sqlite3_bind_blob(stmt, 1, asset_id.data(), 32, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        summary.total_minted = sqlite3_column_int64(stmt, 0);
        summary.total_burned = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);

    summary.circulating_supply = summary.total_minted - summary.total_burned;

    // Get UTXO stats
    sqlite3_prepare_v2(db_,
        "SELECT COUNT(*), COUNT(DISTINCT owner_address) "
        "FROM asset_utxos WHERE asset_id = ? AND is_spent = 0",
        -1, &stmt, nullptr);

    sqlite3_bind_blob(stmt, 1, asset_id.data(), 32, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        summary.utxo_count = sqlite3_column_int(stmt, 0);
        summary.holder_count = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);

    return summary;
}

std::vector<AssetSummary> AssetRegistry::listAssets(uint32_t limit, uint32_t offset) {
    std::vector<AssetSummary> results;

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT asset_id FROM assets ORDER BY created_at DESC LIMIT ? OFFSET ?",
        -1, &stmt, nullptr);

    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);

    std::vector<AssetID> asset_ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AssetID id;
        const uint8_t* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        std::copy(data, data + 32, id.begin());
        asset_ids.push_back(id);
    }
    sqlite3_finalize(stmt);

    // Get summaries (release lock for each call)
    for (const auto& id : asset_ids) {
        // Release lock temporarily
        db_mutex_.unlock();
        auto summary = getAssetSummary(id);
        db_mutex_.lock();

        if (summary) {
            results.push_back(*summary);
        }
    }

    return results;
}

std::vector<AssetSummary> AssetRegistry::searchAssets(const std::string& query) {
    std::vector<AssetSummary> results;

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT asset_id FROM assets WHERE metadata_json LIKE ? LIMIT 20",
        -1, &stmt, nullptr);

    std::string pattern = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_STATIC);

    std::vector<AssetID> asset_ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AssetID id;
        const uint8_t* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        std::copy(data, data + 32, id.begin());
        asset_ids.push_back(id);
    }
    sqlite3_finalize(stmt);

    for (const auto& id : asset_ids) {
        db_mutex_.unlock();
        auto summary = getAssetSummary(id);
        db_mutex_.lock();

        if (summary) {
            results.push_back(*summary);
        }
    }

    return results;
}

// ============================================================================
// UTXO Management
// ============================================================================

bool AssetRegistry::addUTXO(const AssetUTXO& utxo) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_add_utxo_);

    sqlite3_bind_text(stmt_add_utxo_, 1, utxo.txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_add_utxo_, 2, utxo.vout);
    sqlite3_bind_blob(stmt_add_utxo_, 3, utxo.asset_id.data(), 32, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_add_utxo_, 4, utxo.amount);
    sqlite3_bind_blob(stmt_add_utxo_, 5, utxo.state_hash.data(), 32, SQLITE_STATIC);
    sqlite3_bind_blob(stmt_add_utxo_, 6, utxo.script_pubkey.data(), utxo.script_pubkey.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt_add_utxo_, 7, utxo.owner_address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_add_utxo_, 8, utxo.height);
    sqlite3_bind_int64(stmt_add_utxo_, 9, utxo.timestamp);

    return sqlite3_step(stmt_add_utxo_) == SQLITE_DONE;
}

bool AssetRegistry::markUTXOSpent(
    const std::string& txid,
    uint32_t vout,
    const std::string& spending_txid,
    uint32_t spending_input) {

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_mark_spent_);

    sqlite3_bind_text(stmt_mark_spent_, 1, spending_txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_mark_spent_, 2, spending_input);
    sqlite3_bind_text(stmt_mark_spent_, 3, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_mark_spent_, 4, vout);

    return sqlite3_step(stmt_mark_spent_) == SQLITE_DONE;
}

std::optional<AssetUTXO> AssetRegistry::getUTXO(const std::string& txid, uint32_t vout) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_get_utxo_);

    sqlite3_bind_text(stmt_get_utxo_, 1, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_get_utxo_, 2, vout);

    if (sqlite3_step(stmt_get_utxo_) != SQLITE_ROW) {
        return std::nullopt;
    }

    AssetUTXO utxo;
    utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 0));
    utxo.vout = sqlite3_column_int(stmt_get_utxo_, 1);

    const uint8_t* aid = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_get_utxo_, 2));
    std::copy(aid, aid + 32, utxo.asset_id.begin());

    utxo.amount = sqlite3_column_int64(stmt_get_utxo_, 3);

    const uint8_t* sh = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_get_utxo_, 4));
    if (sh) std::copy(sh, sh + 32, utxo.state_hash.begin());

    const uint8_t* spk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_get_utxo_, 5));
    int spk_len = sqlite3_column_bytes(stmt_get_utxo_, 5);
    if (spk) utxo.script_pubkey.assign(spk, spk + spk_len);

    const char* addr = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 6));
    if (addr) utxo.owner_address = addr;

    utxo.height = sqlite3_column_int(stmt_get_utxo_, 7);
    utxo.timestamp = sqlite3_column_int64(stmt_get_utxo_, 8);
    utxo.is_spent = (sqlite3_column_int(stmt_get_utxo_, 9) != 0);

    const char* stxid = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 10));
    if (stxid) utxo.spending_txid = stxid;

    utxo.spending_input_index = sqlite3_column_int(stmt_get_utxo_, 11);

    return utxo;
}

std::vector<AssetUTXO> AssetRegistry::queryUTXOs(const AssetUTXOFilter& filter) {
    std::vector<AssetUTXO> results;

    std::stringstream sql;
    sql << "SELECT txid, vout, asset_id, amount, state_hash, script_pubkey, "
        << "owner_address, height, timestamp, is_spent, spending_txid, spending_input "
        << "FROM asset_utxos WHERE 1=1";

    if (!filter.include_spent) {
        sql << " AND is_spent = 0";
    }

    if (filter.asset_id) {
        sql << " AND asset_id = ?";
    }

    if (filter.address) {
        sql << " AND owner_address = ?";
    }

    if (filter.min_amount) {
        sql << " AND amount >= " << *filter.min_amount;
    }

    if (filter.max_amount) {
        sql << " AND amount <= " << *filter.max_amount;
    }

    if (filter.min_height) {
        sql << " AND height >= " << *filter.min_height;
    }

    if (filter.max_height) {
        sql << " AND height <= " << *filter.max_height;
    }

    sql << " ORDER BY height DESC LIMIT " << filter.limit << " OFFSET " << filter.offset;

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);

    int param_idx = 1;
    if (filter.asset_id) {
        sqlite3_bind_blob(stmt, param_idx++, filter.asset_id->data(), 32, SQLITE_STATIC);
    }
    if (filter.address) {
        sqlite3_bind_text(stmt, param_idx++, filter.address->c_str(), -1, SQLITE_STATIC);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AssetUTXO utxo;

        utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        utxo.vout = sqlite3_column_int(stmt, 1);

        const uint8_t* aid = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
        std::copy(aid, aid + 32, utxo.asset_id.begin());

        utxo.amount = sqlite3_column_int64(stmt, 3);

        const uint8_t* sh = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 4));
        if (sh) std::copy(sh, sh + 32, utxo.state_hash.begin());

        const uint8_t* spk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 5));
        int spk_len = sqlite3_column_bytes(stmt, 5);
        if (spk) utxo.script_pubkey.assign(spk, spk + spk_len);

        const char* addr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        if (addr) utxo.owner_address = addr;

        utxo.height = sqlite3_column_int(stmt, 7);
        utxo.timestamp = sqlite3_column_int64(stmt, 8);
        utxo.is_spent = (sqlite3_column_int(stmt, 9) != 0);

        const char* stxid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        if (stxid) utxo.spending_txid = stxid;

        utxo.spending_input_index = sqlite3_column_int(stmt, 11);

        results.push_back(utxo);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<AssetUTXO> AssetRegistry::getAddressUTXOs(
    const std::string& address,
    const std::optional<AssetID>& asset_id) {

    AssetUTXOFilter filter;
    filter.address = address;
    filter.asset_id = asset_id;
    filter.include_spent = false;
    filter.limit = 10000;

    return queryUTXOs(filter);
}

std::vector<AssetBalance> AssetRegistry::getAddressBalances(const std::string& address) {
    std::vector<AssetBalance> results;

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT asset_id, "
        "SUM(CASE WHEN is_spent = 0 THEN amount ELSE 0 END) as confirmed, "
        "COUNT(*) as utxo_count "
        "FROM asset_utxos WHERE owner_address = ? "
        "GROUP BY asset_id",
        -1, &stmt, nullptr);

    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AssetBalance balance;

        const uint8_t* aid = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        std::copy(aid, aid + 32, balance.asset_id.begin());

        balance.confirmed_balance = sqlite3_column_int64(stmt, 1);
        balance.utxo_count = sqlite3_column_int(stmt, 2);
        balance.unconfirmed_balance = 0;
        balance.pending_spend = 0;

        results.push_back(balance);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ============================================================================
// Supply Tracking
// ============================================================================

bool AssetRegistry::recordMint(
    const AssetID& asset_id,
    uint64_t amount,
    const std::string& mint_txid,
    uint32_t block_height) {

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_record_mint_);

    sqlite3_bind_blob(stmt_record_mint_, 1, asset_id.data(), 32, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_record_mint_, 2, amount);
    sqlite3_bind_text(stmt_record_mint_, 3, mint_txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_record_mint_, 4, block_height);
    sqlite3_bind_int64(stmt_record_mint_, 5, std::time(nullptr));

    bool success = sqlite3_step(stmt_record_mint_) == SQLITE_DONE;

    // Update asset total
    if (success) {
        sqlite3_stmt* update;
        sqlite3_prepare_v2(db_,
            "UPDATE assets SET total_minted = total_minted + ? WHERE asset_id = ?",
            -1, &update, nullptr);
        sqlite3_bind_int64(update, 1, amount);
        sqlite3_bind_blob(update, 2, asset_id.data(), 32, SQLITE_STATIC);
        sqlite3_step(update);
        sqlite3_finalize(update);
    }

    return success;
}

bool AssetRegistry::recordBurn(
    const AssetID& asset_id,
    uint64_t amount,
    const std::string& burn_txid,
    uint32_t block_height) {

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_record_burn_);

    sqlite3_bind_blob(stmt_record_burn_, 1, asset_id.data(), 32, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_record_burn_, 2, amount);
    sqlite3_bind_text(stmt_record_burn_, 3, burn_txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_record_burn_, 4, block_height);
    sqlite3_bind_int64(stmt_record_burn_, 5, std::time(nullptr));

    bool success = sqlite3_step(stmt_record_burn_) == SQLITE_DONE;

    // Update asset total
    if (success) {
        sqlite3_stmt* update;
        sqlite3_prepare_v2(db_,
            "UPDATE assets SET total_burned = total_burned + ? WHERE asset_id = ?",
            -1, &update, nullptr);
        sqlite3_bind_int64(update, 1, amount);
        sqlite3_bind_blob(update, 2, asset_id.data(), 32, SQLITE_STATIC);
        sqlite3_step(update);
        sqlite3_finalize(update);
    }

    return success;
}

uint64_t AssetRegistry::getCirculatingSupply(const AssetID& asset_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT total_minted - total_burned FROM assets WHERE asset_id = ?",
        -1, &stmt, nullptr);

    sqlite3_bind_blob(stmt, 1, asset_id.data(), 32, SQLITE_STATIC);

    uint64_t supply = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        supply = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return supply;
}

bool AssetRegistry::canMint(const AssetID& asset_id, uint64_t amount) {
    auto genesis = getAssetGenesis(asset_id);
    if (!genesis) return false;

    // Check supply model
    if (genesis->supply.model == SupplyModel::FIXED) {
        return false; // No minting allowed
    }

    if (genesis->supply.model == SupplyModel::CAPPED) {
        uint64_t current = getCirculatingSupply(asset_id);
        return (current + amount) <= genesis->supply.max_supply;
    }

    return true; // UNLIMITED or ALGORITHMIC
}

// ============================================================================
// Transfer Indexing
// ============================================================================

bool AssetRegistry::indexTransfer(
    const AssetStateTransition& transition,
    const std::string& txid,
    uint32_t block_height) {

    std::lock_guard<std::mutex> lock(db_mutex_);

    // Record each movement
    for (const auto& output : transition.outputs) {
        sqlite3_reset(stmt_record_transfer_);

        // Find matching input for from_address
        std::string from_addr;
        for (const auto& input : transition.inputs) {
            if (input.asset_id == output.asset_id) {
                // Would need UTXO lookup for actual from address
                break;
            }
        }

        sqlite3_bind_text(stmt_record_transfer_, 1, txid.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(stmt_record_transfer_, 2, output.asset_id.data(), 32, SQLITE_STATIC);
        sqlite3_bind_int64(stmt_record_transfer_, 3, output.amount);
        sqlite3_bind_text(stmt_record_transfer_, 4, from_addr.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_record_transfer_, 5, "", -1, SQLITE_STATIC); // to_address from scriptPubKey
        sqlite3_bind_int(stmt_record_transfer_, 6, block_height);
        sqlite3_bind_int64(stmt_record_transfer_, 7, std::time(nullptr));

        sqlite3_step(stmt_record_transfer_);
    }

    return true;
}

std::vector<AssetRegistry::TransferRecord> AssetRegistry::getTransferHistory(
    const std::string& address,
    const std::optional<AssetID>& asset_id,
    uint32_t limit,
    uint32_t offset) {

    std::vector<TransferRecord> results;

    std::stringstream sql;
    sql << "SELECT txid, asset_id, amount, from_address, to_address, block_height, timestamp "
        << "FROM transfers WHERE (from_address = ? OR to_address = ?)";

    if (asset_id) {
        sql << " AND asset_id = ?";
    }

    sql << " ORDER BY timestamp DESC LIMIT " << limit << " OFFSET " << offset;

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);

    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, address.c_str(), -1, SQLITE_STATIC);

    if (asset_id) {
        sqlite3_bind_blob(stmt, 3, asset_id->data(), 32, SQLITE_STATIC);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TransferRecord record;

        record.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        const uint8_t* aid = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
        std::copy(aid, aid + 32, record.asset_id.begin());

        record.amount = sqlite3_column_int64(stmt, 2);

        const char* from = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (from) record.from_address = from;

        const char* to = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (to) record.to_address = to;

        record.block_height = sqlite3_column_int(stmt, 5);
        record.timestamp = sqlite3_column_int64(stmt, 6);

        results.push_back(record);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ============================================================================
// Block Processing
// ============================================================================

uint32_t AssetRegistry::processBlock(
    uint32_t block_height,
    const std::string& block_hash,
    std::function<std::optional<std::vector<uint8_t>>(const std::string&)> get_tx) {

    // Implementation would parse transactions for asset operations
    // This is a placeholder

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO block_index (height, block_hash, processed_at) VALUES (?, ?, ?)",
        -1, &stmt, nullptr);

    sqlite3_bind_int(stmt, 1, block_height);
    sqlite3_bind_text(stmt, 2, block_hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, std::time(nullptr));

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return 0;
}

uint32_t AssetRegistry::revertBlock(uint32_t block_height) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Remove UTXOs created at this height
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "DELETE FROM asset_utxos WHERE height = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, block_height);
    sqlite3_step(stmt);
    int deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    // Restore spent UTXOs
    sqlite3_prepare_v2(db_,
        "UPDATE asset_utxos SET is_spent = 0, spending_txid = NULL, spending_input = NULL "
        "WHERE spending_txid IN (SELECT txid FROM transfers WHERE block_height = ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, block_height);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Remove transfers
    sqlite3_prepare_v2(db_,
        "DELETE FROM transfers WHERE block_height = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, block_height);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Remove supply events
    sqlite3_prepare_v2(db_,
        "DELETE FROM supply_events WHERE block_height = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, block_height);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Update block index
    sqlite3_prepare_v2(db_,
        "DELETE FROM block_index WHERE height >= ?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, block_height);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return deleted;
}

uint32_t AssetRegistry::getLastIndexedHeight() const {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT MAX(height) FROM block_index",
        -1, &stmt, nullptr);

    uint32_t height = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        height = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return height;
}

// ============================================================================
// Validation
// ============================================================================

bool AssetRegistry::validateTransfer(const AssetStateTransition& transition) {
    // Validate inputs exist
    for (const auto& input : transition.inputs) {
        auto utxo = getUTXO(input.txid, input.vout);
        if (!utxo) return false;
        if (utxo->is_spent) return false;
        if (utxo->asset_id != input.asset_id) return false;
        if (utxo->amount != input.amount) return false;
    }

    // Validate conservation
    return transition.checkConservation();
}

bool AssetRegistry::validateMintAuth(
    const AssetID& asset_id,
    uint64_t amount,
    const std::vector<uint8_t>& authorization) {

    auto genesis = getAssetGenesis(asset_id);
    if (!genesis) return false;

    // Check mint authority
    if (genesis->supply.mint_authority.empty()) {
        return false; // No mint authority
    }

    // Verify CSFS signature (placeholder)
    return authorization.size() == 64;
}

bool AssetRegistry::validateBurnAuth(
    const AssetID& asset_id,
    uint64_t amount,
    const std::vector<uint8_t>& authorization) {

    auto genesis = getAssetGenesis(asset_id);
    if (!genesis) return false;

    // Check if burn is enabled
    if (!genesis->supply.burn_enabled) {
        return false;
    }

    // If burn authority is empty, anyone can burn
    if (genesis->supply.burn_authority.empty()) {
        return true;
    }

    // Verify CSFS signature
    return authorization.size() == 64;
}

// ============================================================================
// Statistics
// ============================================================================

RegistryStats AssetRegistry::getStats() const {
    RegistryStats stats{};

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;

    // Total assets
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM assets", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_assets = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // UTXO counts
    sqlite3_prepare_v2(db_,
        "SELECT COUNT(*), SUM(is_spent) FROM asset_utxos",
        -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_utxos = sqlite3_column_int64(stmt, 0);
        stats.spent_utxos = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);

    // Transfer count
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM transfers", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_transfers = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // Supply event counts
    sqlite3_prepare_v2(db_,
        "SELECT event_type, COUNT(*) FROM supply_events GROUP BY event_type",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        uint64_t count = sqlite3_column_int64(stmt, 1);
        if (type) {
            if (std::string(type) == "mint") stats.total_mints = count;
            else if (std::string(type) == "burn") stats.total_burns = count;
        }
    }
    sqlite3_finalize(stmt);

    stats.last_indexed_height = getLastIndexedHeight();

    return stats;
}

std::vector<AssetRegistry::HolderInfo> AssetRegistry::getTopHolders(
    const AssetID& asset_id,
    uint32_t limit) {

    std::vector<HolderInfo> results;

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT owner_address, SUM(amount) as balance, COUNT(*) as utxo_count "
        "FROM asset_utxos WHERE asset_id = ? AND is_spent = 0 "
        "GROUP BY owner_address ORDER BY balance DESC LIMIT ?",
        -1, &stmt, nullptr);

    sqlite3_bind_blob(stmt, 1, asset_id.data(), 32, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HolderInfo holder;

        const char* addr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (addr) holder.address = addr;

        holder.balance = sqlite3_column_int64(stmt, 1);
        holder.utxo_count = sqlite3_column_int(stmt, 2);

        results.push_back(holder);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ============================================================================
// AssetIndexCache Implementation
// ============================================================================

AssetIndexCache::AssetIndexCache(size_t max_entries)
    : max_entries_(max_entries) {}

void AssetIndexCache::cacheAsset(const AssetID& id, const AssetMetadata& metadata) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    evictIfNeeded();

    auto data = metadata.serialize();
    CacheEntry entry{data, static_cast<uint64_t>(std::time(nullptr))};
    cache_["asset:" + bytesToHex(id.data(), 32)] = entry;
}

std::optional<AssetMetadata> AssetIndexCache::getCachedMetadata(const AssetID& id) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find("asset:" + bytesToHex(id.data(), 32));
    if (it == cache_.end()) {
        misses_++;
        return std::nullopt;
    }

    hits_++;
    it->second.last_access = static_cast<uint64_t>(std::time(nullptr));
    return AssetMetadata::deserialize(it->second.data);
}

void AssetIndexCache::cacheUTXO(const std::string& outpoint, const AssetID& asset_id) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    evictIfNeeded();

    std::vector<uint8_t> data(asset_id.begin(), asset_id.end());
    CacheEntry entry{data, static_cast<uint64_t>(std::time(nullptr))};
    cache_["utxo:" + outpoint] = entry;
}

std::optional<AssetID> AssetIndexCache::getCachedUTXOAsset(const std::string& outpoint) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find("utxo:" + outpoint);
    if (it == cache_.end() || it->second.data.size() != 32) {
        misses_++;
        return std::nullopt;
    }

    hits_++;
    it->second.last_access = static_cast<uint64_t>(std::time(nullptr));

    AssetID id;
    std::copy(it->second.data.begin(), it->second.data.end(), id.begin());
    return id;
}

void AssetIndexCache::invalidateUTXO(const std::string& outpoint) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.erase("utxo:" + outpoint);
}

void AssetIndexCache::cacheSupply(const AssetID& id, uint64_t supply) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    evictIfNeeded();

    std::vector<uint8_t> data(8);
    for (int i = 0; i < 8; i++) {
        data[i] = static_cast<uint8_t>((supply >> (i * 8)) & 0xFF);
    }

    CacheEntry entry{data, static_cast<uint64_t>(std::time(nullptr))};
    cache_["supply:" + bytesToHex(id.data(), 32)] = entry;
}

std::optional<uint64_t> AssetIndexCache::getCachedSupply(const AssetID& id) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find("supply:" + bytesToHex(id.data(), 32));
    if (it == cache_.end() || it->second.data.size() != 8) {
        misses_++;
        return std::nullopt;
    }

    hits_++;
    it->second.last_access = static_cast<uint64_t>(std::time(nullptr));

    uint64_t supply = 0;
    for (int i = 7; i >= 0; i--) {
        supply = (supply << 8) | it->second.data[i];
    }
    return supply;
}

void AssetIndexCache::invalidateSupply(const AssetID& id) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.erase("supply:" + bytesToHex(id.data(), 32));
}

void AssetIndexCache::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();
    hits_ = 0;
    misses_ = 0;
}

size_t AssetIndexCache::size() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return cache_.size();
}

double AssetIndexCache::hitRate() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    uint64_t total = hits_ + misses_;
    if (total == 0) return 0.0;
    return static_cast<double>(hits_) / total;
}

void AssetIndexCache::evictIfNeeded() {
    // Already holding lock
    if (cache_.size() < max_entries_) return;

    // Find oldest entry
    std::string oldest_key;
    uint64_t oldest_time = UINT64_MAX;

    for (const auto& [key, entry] : cache_) {
        if (entry.last_access < oldest_time) {
            oldest_time = entry.last_access;
            oldest_key = key;
        }
    }

    if (!oldest_key.empty()) {
        cache_.erase(oldest_key);
    }
}

} // namespace assets
} // namespace dinero
