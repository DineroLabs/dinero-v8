#include "contracts/multiasset_persistence.h"
#include "common/logger.h"
#include <json/json.h>
#include <sstream>

namespace dinero {
namespace contracts {

bool MultiAssetPersistence::initialize(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_) {
        g_logger.warning("[MultiAssetPersistence] Already initialized");
        return true;
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        g_logger.error("[MultiAssetPersistence] Failed to open database: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    // Enable foreign keys and WAL mode for better performance
    executeSql("PRAGMA foreign_keys = ON");
    executeSql("PRAGMA journal_mode = WAL");
    executeSql("PRAGMA synchronous = NORMAL");

    if (!createSchema()) {
        g_logger.error("[MultiAssetPersistence] Failed to create schema");
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    g_logger.info("[MultiAssetPersistence] Initialized with database: " + db_path);
    return true;
}

void MultiAssetPersistence::close() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        g_logger.info("[MultiAssetPersistence] Database closed");
    }
}

bool MultiAssetPersistence::createSchema() {
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS multiasset_contracts (
            contract_id TEXT PRIMARY KEY,
            asset_id TEXT NOT NULL,
            decimals INTEGER NOT NULL,
            amount REAL NOT NULL,
            refund_time INTEGER NOT NULL,
            status TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            confirmations INTEGER DEFAULT 0,

            -- Escrow details
            redeem_script TEXT NOT NULL,
            script_hash TEXT NOT NULL,
            p2sh_address TEXT NOT NULL,
            lock_txid TEXT,
            lock_vout INTEGER DEFAULT 0,

            -- Keys (stored as JSON)
            keys_json TEXT NOT NULL,

            -- Conversion details
            release_asset TEXT,
            swap_route_json TEXT,
            asset_address TEXT,
            is_wrapped INTEGER DEFAULT 0,

            -- Indexes for common queries
            CHECK (decimals >= 0 AND decimals <= 18),
            CHECK (amount > 0),
            CHECK (confirmations >= 0)
        );

        CREATE INDEX IF NOT EXISTS idx_asset_id ON multiasset_contracts(asset_id);
        CREATE INDEX IF NOT EXISTS idx_status ON multiasset_contracts(status);
        CREATE INDEX IF NOT EXISTS idx_created_at ON multiasset_contracts(created_at);
        CREATE INDEX IF NOT EXISTS idx_release_asset ON multiasset_contracts(release_asset);
    )";

    return executeSql(schema);
}

bool MultiAssetPersistence::saveContract(const AssetEscrowContract& contract) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        g_logger.error("[MultiAssetPersistence] Database not initialized");
        return false;
    }

    const char* sql = R"(
        INSERT OR REPLACE INTO multiasset_contracts (
            contract_id, asset_id, decimals, amount, refund_time, status, created_at,
            confirmations, redeem_script, script_hash, p2sh_address, lock_txid, lock_vout,
            keys_json, release_asset, swap_route_json, asset_address, is_wrapped
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        g_logger.error("[MultiAssetPersistence] Failed to prepare statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    // Bind parameters
    sqlite3_bind_text(stmt, 1, contract.contract_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, contract.asset_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, contract.decimals);
    sqlite3_bind_double(stmt, 4, contract.amount);
    sqlite3_bind_int(stmt, 5, contract.refund_time);
    sqlite3_bind_text(stmt, 6, contract.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, contract.created_at);
    sqlite3_bind_int(stmt, 8, contract.confirmations);
    sqlite3_bind_text(stmt, 9, contract.redeem_script.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, contract.script_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, contract.p2sh_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, contract.lock_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, contract.lock_vout);

    std::string keys_json = serializeKeys(contract.keys);
    sqlite3_bind_text(stmt, 14, keys_json.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 15, contract.release_asset.c_str(), -1, SQLITE_TRANSIENT);

    std::string route_json = serializeRoute(contract.swap_route);
    sqlite3_bind_text(stmt, 16, route_json.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 17, contract.asset_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 18, contract.is_wrapped ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        g_logger.error("[MultiAssetPersistence] Failed to save contract: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    g_logger.info("[MultiAssetPersistence] Saved contract: " + contract.contract_id);
    return true;
}

std::optional<AssetEscrowContract> MultiAssetPersistence::loadContract(const std::string& contract_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) return std::nullopt;

    const char* sql = "SELECT * FROM multiasset_contracts WHERE contract_id = ?";
    sqlite3_stmt* stmt;

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<AssetEscrowContract> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = rowToContract(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<AssetEscrowContract> MultiAssetPersistence::loadContractsByAsset(const std::string& asset_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AssetEscrowContract> contracts;
    if (!db_) return contracts;

    const char* sql = "SELECT * FROM multiasset_contracts WHERE asset_id = ? ORDER BY created_at DESC";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return contracts;
    }

    sqlite3_bind_text(stmt, 1, asset_id.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        contracts.push_back(rowToContract(stmt));
    }

    sqlite3_finalize(stmt);
    return contracts;
}

std::vector<AssetEscrowContract> MultiAssetPersistence::loadActiveContracts() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AssetEscrowContract> contracts;
    if (!db_) return contracts;

    const char* sql = "SELECT * FROM multiasset_contracts WHERE status IN ('pending', 'locked') ORDER BY created_at DESC";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return contracts;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        contracts.push_back(rowToContract(stmt));
    }

    sqlite3_finalize(stmt);
    return contracts;
}

std::vector<AssetEscrowContract> MultiAssetPersistence::loadAllContracts() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AssetEscrowContract> contracts;
    if (!db_) return contracts;

    const char* sql = "SELECT * FROM multiasset_contracts ORDER BY created_at DESC";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return contracts;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        contracts.push_back(rowToContract(stmt));
    }

    sqlite3_finalize(stmt);
    return contracts;
}

bool MultiAssetPersistence::updateContractStatus(const std::string& contract_id, const std::string& new_status) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) return false;

    const char* sql = "UPDATE multiasset_contracts SET status = ? WHERE contract_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, contract_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        g_logger.info("[MultiAssetPersistence] Updated contract " + contract_id + " to " + new_status);
        return true;
    }

    return false;
}

bool MultiAssetPersistence::deleteContract(const std::string& contract_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) return false;

    const char* sql = "DELETE FROM multiasset_contracts WHERE contract_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

std::map<std::string, size_t> MultiAssetPersistence::getAssetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, size_t> stats;
    if (!db_) return stats;

    const char* sql = "SELECT asset_id, COUNT(*) as count FROM multiasset_contracts GROUP BY asset_id";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string asset_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        size_t count = sqlite3_column_int64(stmt, 1);
        stats[asset_id] = count;
    }

    sqlite3_finalize(stmt);
    return stats;
}

// Helper methods

std::string MultiAssetPersistence::serializeKeys(const EscrowKeys& keys) {
    Json::Value root;
    root["buyer"] = keys.buyer_pubkey;
    root["seller"] = keys.seller_pubkey;
    root["mediator"] = keys.mediator_pubkey;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

EscrowKeys MultiAssetPersistence::deserializeKeys(const std::string& json) {
    EscrowKeys keys;

    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;

    std::istringstream stream(json);
    if (Json::parseFromStream(reader, stream, &root, &errors)) {
        keys.buyer_pubkey = root["buyer"].asString();
        keys.seller_pubkey = root["seller"].asString();
        keys.mediator_pubkey = root["mediator"].asString();
    }

    return keys;
}

std::string MultiAssetPersistence::serializeRoute(const std::optional<bridge::ConversionRoute>& route) {
    if (!route.has_value()) {
        return "";
    }

    Json::Value root;
    root["total_rate"] = route->total_rate;
    root["total_fee_bps"] = route->total_fee_bps;
    root["slippage_bps"] = route->slippage_bps;
    root["hop_count"] = route->hop_count;

    Json::Value hops(Json::arrayValue);
    for (const auto& hop : route->hops) {
        Json::Value hop_json;
        hop_json["from_asset"] = hop.from_asset;
        hop_json["to_asset"] = hop.to_asset;
        hop_json["rate"] = hop.rate;
        hop_json["fee_bps"] = hop.fee_bps;
        hop_json["provider"] = hop.provider;
        hops.append(hop_json);
    }
    root["hops"] = hops;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::optional<bridge::ConversionRoute> MultiAssetPersistence::deserializeRoute(const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }

    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;

    std::istringstream stream(json);
    if (!Json::parseFromStream(reader, stream, &root, &errors)) {
        return std::nullopt;
    }

    bridge::ConversionRoute route;
    route.total_rate = root["total_rate"].asDouble();
    route.total_fee_bps = root["total_fee_bps"].asDouble();
    route.slippage_bps = root["slippage_bps"].asDouble();
    route.hop_count = root["hop_count"].asInt();

    for (const auto& hop_json : root["hops"]) {
        bridge::RouteHop hop;
        hop.from_asset = hop_json["from_asset"].asString();
        hop.to_asset = hop_json["to_asset"].asString();
        hop.rate = hop_json["rate"].asDouble();
        hop.fee_bps = hop_json["fee_bps"].asDouble();
        hop.provider = hop_json["provider"].asString();
        route.hops.push_back(hop);
    }

    return route;
}

AssetEscrowContract MultiAssetPersistence::rowToContract(sqlite3_stmt* stmt) {
    AssetEscrowContract contract;

    contract.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    contract.asset_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    contract.decimals = sqlite3_column_int(stmt, 2);
    contract.amount = sqlite3_column_double(stmt, 3);
    contract.refund_time = sqlite3_column_int(stmt, 4);
    contract.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    contract.created_at = sqlite3_column_int64(stmt, 6);
    contract.confirmations = sqlite3_column_int(stmt, 7);
    contract.redeem_script = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    contract.script_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    contract.p2sh_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));

    const char* lock_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    contract.lock_txid = lock_txid ? lock_txid : "";
    contract.lock_vout = sqlite3_column_int(stmt, 12);

    const char* keys_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
    contract.keys = deserializeKeys(keys_json ? keys_json : "{}");

    const char* release_asset = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    contract.release_asset = release_asset ? release_asset : "";

    const char* route_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
    contract.swap_route = deserializeRoute(route_json ? route_json : "");

    const char* asset_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
    contract.asset_address = asset_address ? asset_address : "";

    contract.is_wrapped = sqlite3_column_int(stmt, 17) != 0;

    return contract;
}

bool MultiAssetPersistence::executeSql(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[MultiAssetPersistence] SQL error: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

} // namespace contracts
} // namespace dinero
