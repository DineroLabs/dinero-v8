/**
 * Marketplace Manager Implementation - SQLite Backend
 *
 * Production-grade persistent storage for P2P marketplace.
 */

#include "p2p/marketplace_manager.h"
#include "database/sqlite_conn.h"
#include "common/logger.h"
#include <sqlite3.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>
#include <filesystem>

namespace din {

// ═══════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════

static std::string generateId(const std::string& prefix) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    auto random = dis(gen);

    std::stringstream ss;
    ss << prefix << "_" << std::hex << timestamp << "_" << random;
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// JSON CONVERSION
// ═══════════════════════════════════════════════════════════════

Json MarketplaceOffer::toJson() const {
    Json j;
    j["offer_id"] = offer_id;
    j["type"] = type;
    j["asset"] = asset;
    j["amount"] = amount;
    j["price"] = price;
    j["currency"] = currency;
    j["description"] = description;
    j["creator_pubkey"] = creator_pubkey;
    j["mediator_pubkey"] = mediator_pubkey;
    j["min_trade"] = min_trade;
    j["max_trade"] = max_trade;
    j["payment_methods"] = ::Json::arrayValue;
    for (const auto& method : payment_methods) {
        j["payment_methods"].append(method);
    }
    j["delivery_time"] = delivery_time;
    j["status"] = status;
    j["created_at"] = static_cast<Json::Int64>(created_at);
    j["updated_at"] = static_cast<Json::Int64>(updated_at);
    return j;
}

MarketplaceOffer MarketplaceOffer::fromJson(const Json& j) {
    MarketplaceOffer offer;
    offer.offer_id = j["offer_id"].asString();
    offer.type = j["type"].asString();
    offer.asset = j["asset"].asString();
    offer.amount = j["amount"].asDouble();
    offer.price = j["price"].asDouble();
    offer.currency = j["currency"].asString();
    offer.description = j["description"].asString();
    offer.creator_pubkey = j["creator_pubkey"].asString();
    offer.mediator_pubkey = j.isMember("mediator_pubkey") ? j["mediator_pubkey"].asString() : "";
    offer.min_trade = j.isMember("min_trade") ? j["min_trade"].asDouble() : 0.0;
    offer.max_trade = j.isMember("max_trade") ? j["max_trade"].asDouble() : offer.amount;

    offer.payment_methods.clear();
    if (j.isMember("payment_methods") && j["payment_methods"].isArray()) {
        for (const auto& method : j["payment_methods"]) {
            offer.payment_methods.push_back(method.asString());
        }
    }

    offer.delivery_time = j.isMember("delivery_time") ? j["delivery_time"].asInt() : 0;
    offer.status = j["status"].asString();
    offer.created_at = j["created_at"].asInt64();
    offer.updated_at = j.isMember("updated_at") ? j["updated_at"].asInt64() : offer.created_at;
    return offer;
}

Json Trade::toJson() const {
    Json j;
    j["trade_id"] = trade_id;
    j["offer_id"] = offer_id;
    j["buyer_pubkey"] = buyer_pubkey;
    j["seller_pubkey"] = seller_pubkey;
    j["mediator_pubkey"] = mediator_pubkey;
    j["amount"] = amount;
    j["price"] = price;
    j["total_value"] = total_value;
    j["currency"] = currency;
    j["contract_id"] = contract_id;
    j["escrow_address"] = escrow_address;
    j["status"] = status;
    j["created_at"] = static_cast<Json::Int64>(created_at);
    j["completed_at"] = static_cast<Json::Int64>(completed_at);
    j["rating"] = rating;
    j["review"] = review;
    return j;
}

Trade Trade::fromJson(const Json& j) {
    Trade trade;
    trade.trade_id = j["trade_id"].asString();
    trade.offer_id = j["offer_id"].asString();
    trade.buyer_pubkey = j["buyer_pubkey"].asString();
    trade.seller_pubkey = j["seller_pubkey"].asString();
    trade.mediator_pubkey = j["mediator_pubkey"].asString();
    trade.amount = j["amount"].asDouble();
    trade.price = j["price"].asDouble();
    trade.total_value = j["total_value"].asDouble();
    trade.currency = j["currency"].asString();
    trade.contract_id = j["contract_id"].asString();
    trade.escrow_address = j["escrow_address"].asString();
    trade.status = j["status"].asString();
    trade.created_at = j["created_at"].asInt64();
    trade.completed_at = j.isMember("completed_at") ? j["completed_at"].asInt64() : 0;
    trade.rating = j.isMember("rating") ? j["rating"].asInt() : 0;
    trade.review = j.isMember("review") ? j["review"].asString() : "";
    return trade;
}

Json Reputation::toJson() const {
    Json j;
    j["user_pubkey"] = user_pubkey;
    j["total_trades"] = total_trades;
    j["successful_trades"] = successful_trades;
    j["disputed_trades"] = disputed_trades;
    j["average_rating"] = average_rating;
    j["rating_distribution"] = ::Json::arrayValue;
    for (const auto& count : rating_distribution) {
        j["rating_distribution"].append(count);
    }
    j["first_trade_date"] = static_cast<Json::Int64>(first_trade_date);
    j["last_trade_date"] = static_cast<Json::Int64>(last_trade_date);
    return j;
}

Reputation Reputation::fromJson(const Json& j) {
    Reputation rep;
    rep.user_pubkey = j["user_pubkey"].asString();
    rep.total_trades = j["total_trades"].asInt();
    rep.successful_trades = j["successful_trades"].asInt();
    rep.disputed_trades = j["disputed_trades"].asInt();
    rep.average_rating = j["average_rating"].asDouble();

    rep.rating_distribution.clear();
    if (j.isMember("rating_distribution") && j["rating_distribution"].isArray()) {
        for (const auto& count : j["rating_distribution"]) {
            rep.rating_distribution.push_back(count.asInt());
        }
    }

    rep.first_trade_date = j["first_trade_date"].asInt64();
    rep.last_trade_date = j["last_trade_date"].asInt64();
    return rep;
}

Json Dispute::toJson() const {
    Json j;
    j["dispute_id"] = dispute_id;
    j["trade_id"] = trade_id;
    j["complainant_pubkey"] = complainant_pubkey;
    j["reason"] = reason;
    j["evidence"] = evidence;
    j["mediator_pubkey"] = mediator_pubkey;
    j["resolution"] = resolution;
    j["status"] = status;
    j["created_at"] = static_cast<Json::Int64>(created_at);
    j["resolved_at"] = static_cast<Json::Int64>(resolved_at);
    return j;
}

Dispute Dispute::fromJson(const Json& j) {
    Dispute dispute;
    dispute.dispute_id = j["dispute_id"].asString();
    dispute.trade_id = j["trade_id"].asString();
    dispute.complainant_pubkey = j["complainant_pubkey"].asString();
    dispute.reason = j["reason"].asString();
    dispute.evidence = j.isMember("evidence") ? j["evidence"].asString() : "";
    dispute.mediator_pubkey = j["mediator_pubkey"].asString();
    dispute.resolution = j.isMember("resolution") ? j["resolution"].asString() : "";
    dispute.status = j["status"].asString();
    dispute.created_at = j["created_at"].asInt64();
    dispute.resolved_at = j.isMember("resolved_at") ? j["resolved_at"].asInt64() : 0;
    return dispute;
}

// ═══════════════════════════════════════════════════════════════
// MARKETPLACE MANAGER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════

std::unique_ptr<MarketplaceManager> MarketplaceManager::instance_;
std::once_flag MarketplaceManager::init_flag_;

MarketplaceManager& MarketplaceManager::instance() {
    std::call_once(init_flag_, []() {
        instance_.reset(new MarketplaceManager());
    });
    return *instance_;
}

MarketplaceManager::MarketplaceManager() {
    dinero::g_logger.info("[Marketplace] Initializing marketplace manager");
}

MarketplaceManager::~MarketplaceManager() {
    dinero::g_logger.info("[Marketplace] Shutting down marketplace manager");
}

void MarketplaceManager::setDataDir(const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(get_mutex());
    data_dir_ = data_dir;

    // Ensure directory exists
    std::filesystem::create_directories(data_dir_);

    // Initialize database
    std::string db_path = data_dir_ + "/marketplace.db";

    try {
        dinero::SqliteConn conn(db_path);

        // Create tables
        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS offers (
                offer_id TEXT PRIMARY KEY,
                type TEXT NOT NULL,
                asset TEXT NOT NULL,
                amount REAL NOT NULL,
                price REAL NOT NULL,
                currency TEXT NOT NULL,
                description TEXT NOT NULL,
                creator_pubkey TEXT NOT NULL,
                mediator_pubkey TEXT,
                min_trade REAL,
                max_trade REAL,
                payment_methods TEXT,
                delivery_time INTEGER,
                status TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
        )");

        conn.Exec(R"(
            CREATE INDEX IF NOT EXISTS idx_offers_status ON offers(status);
            CREATE INDEX IF NOT EXISTS idx_offers_creator ON offers(creator_pubkey);
            CREATE INDEX IF NOT EXISTS idx_offers_type ON offers(type);
            CREATE INDEX IF NOT EXISTS idx_offers_asset ON offers(asset);
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS trades (
                trade_id TEXT PRIMARY KEY,
                offer_id TEXT NOT NULL,
                buyer_pubkey TEXT NOT NULL,
                seller_pubkey TEXT NOT NULL,
                mediator_pubkey TEXT NOT NULL,
                amount REAL NOT NULL,
                price REAL NOT NULL,
                total_value REAL NOT NULL,
                currency TEXT NOT NULL,
                contract_id TEXT NOT NULL,
                escrow_address TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                completed_at INTEGER,
                rating INTEGER,
                review TEXT,
                FOREIGN KEY (offer_id) REFERENCES offers(offer_id)
            );
        )");

        conn.Exec(R"(
            CREATE INDEX IF NOT EXISTS idx_trades_buyer ON trades(buyer_pubkey);
            CREATE INDEX IF NOT EXISTS idx_trades_seller ON trades(seller_pubkey);
            CREATE INDEX IF NOT EXISTS idx_trades_status ON trades(status);
            CREATE INDEX IF NOT EXISTS idx_trades_offer ON trades(offer_id);
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS reputations (
                user_pubkey TEXT PRIMARY KEY,
                total_trades INTEGER DEFAULT 0,
                successful_trades INTEGER DEFAULT 0,
                disputed_trades INTEGER DEFAULT 0,
                average_rating REAL DEFAULT 0.0,
                rating_1star INTEGER DEFAULT 0,
                rating_2star INTEGER DEFAULT 0,
                rating_3star INTEGER DEFAULT 0,
                rating_4star INTEGER DEFAULT 0,
                rating_5star INTEGER DEFAULT 0,
                first_trade_date INTEGER,
                last_trade_date INTEGER
            );
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS disputes (
                dispute_id TEXT PRIMARY KEY,
                trade_id TEXT NOT NULL,
                complainant_pubkey TEXT NOT NULL,
                reason TEXT NOT NULL,
                evidence TEXT,
                mediator_pubkey TEXT NOT NULL,
                resolution TEXT,
                status TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                resolved_at INTEGER,
                FOREIGN KEY (trade_id) REFERENCES trades(trade_id)
            );
        )");

        conn.Exec(R"(
            CREATE INDEX IF NOT EXISTS idx_disputes_trade ON disputes(trade_id);
            CREATE INDEX IF NOT EXISTS idx_disputes_mediator ON disputes(mediator_pubkey);
            CREATE INDEX IF NOT EXISTS idx_disputes_status ON disputes(status);
        )");

        dinero::g_logger.info("[Marketplace] Database initialized: " + db_path);

    } catch (const std::exception& e) {
        dinero::g_logger.error("[Marketplace] Failed to initialize database: " + std::string(e.what()));
        throw;
    }
}

std::string MarketplaceManager::generateOfferId() {
    return generateId("offer");
}

std::string MarketplaceManager::generateTradeId() {
    return generateId("trade");
}

std::string MarketplaceManager::generateDisputeId() {
    return generateId("dispute");
}

// ═══════════════════════════════════════════════════════════════
// OFFER MANAGEMENT
// ═══════════════════════════════════════════════════════════════

Json MarketplaceManager::createOffer(const Json& params) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::string offer_id = generateOfferId();
    int64_t now = std::time(nullptr);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT INTO offers (
            offer_id, type, asset, amount, price, currency, description,
            creator_pubkey, mediator_pubkey, min_trade, max_trade,
            payment_methods, delivery_time, status, created_at, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");

    // Serialize payment_methods array to JSON string for storage
    std::string payment_methods_json = "[]";
    if (params.isMember("payment_methods") && params["payment_methods"].isArray()) {
        ::Json::StreamWriterBuilder builder;
        builder["indentation"] = "";  // Compact JSON
        payment_methods_json = ::Json::writeString(builder, params["payment_methods"]);
    }

    sqlite3_bind_text(stmt, 1, offer_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, params["type"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, params["asset"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, params["amount"].asDouble());
    sqlite3_bind_double(stmt, 5, params["price"].asDouble());
    sqlite3_bind_text(stmt, 6, params["currency"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, params["description"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, params["creator_pubkey"].asString().c_str(), -1, SQLITE_TRANSIENT);
    std::string mediator_pubkey = params.isMember("mediator_pubkey") ? params["mediator_pubkey"].asString() : "";
    sqlite3_bind_text(stmt, 9, mediator_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    double default_min_trade = params["amount"].asDouble() * 0.1;
    double min_trade = params.isMember("min_trade") ? params["min_trade"].asDouble() : default_min_trade;
    sqlite3_bind_double(stmt, 10, min_trade);
    double max_trade = params.isMember("max_trade") ? params["max_trade"].asDouble() : params["amount"].asDouble();
    sqlite3_bind_double(stmt, 11, max_trade);
    sqlite3_bind_text(stmt, 12, payment_methods_json.c_str(), -1, SQLITE_TRANSIENT);
    int delivery_time = params.isMember("delivery_time") ? params["delivery_time"].asInt() : 0;
    sqlite3_bind_int64(stmt, 13, delivery_time);
    sqlite3_bind_text(stmt, 14, params["status"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 15, now);
    sqlite3_bind_int64(stmt, 16, now);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to insert offer: " + std::string(sqlite3_errmsg(conn.db)));
    }

    sqlite3_finalize(stmt);

    Json result = params;
    result["offer_id"] = offer_id;
    result["created_at"] = now;
    result["updated_at"] = now;

    return result;
}

bool MarketplaceManager::cancelOffer(const std::string& offer_id, const std::string& creator_pubkey) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        UPDATE offers
        SET status = 'cancelled', updated_at = ?
        WHERE offer_id = ? AND creator_pubkey = ? AND status = 'active'
    )");

    int64_t now = std::time(nullptr);
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, offer_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, creator_pubkey.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    int changes = sqlite3_changes(conn.db);
    sqlite3_finalize(stmt);

    return changes > 0;
}

Json MarketplaceManager::updateOffer(const std::string& offer_id, const Json& updates, const std::string& creator_pubkey) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::stringstream sql;
    sql << "UPDATE offers SET updated_at = ?";

    int param_index = 2;
    if (updates.isMember("price")) sql << ", price = ?";
    if (updates.isMember("amount")) sql << ", amount = ?";
    if (updates.isMember("description")) sql << ", description = ?";
    if (updates.isMember("status")) sql << ", status = ?";

    sql << " WHERE offer_id = ? AND creator_pubkey = ?";

    sqlite3_stmt* stmt = conn.Prepare(sql.str().c_str());

    int64_t now = std::time(nullptr);
    sqlite3_bind_int64(stmt, 1, now);

    if (updates.isMember("price")) {
        sqlite3_bind_double(stmt, param_index++, updates["price"].asDouble());
    }
    if (updates.isMember("amount")) {
        sqlite3_bind_double(stmt, param_index++, updates["amount"].asDouble());
    }
    if (updates.isMember("description")) {
        sqlite3_bind_text(stmt, param_index++, updates["description"].asString().c_str(), -1, SQLITE_TRANSIENT);
    }
    if (updates.isMember("status")) {
        sqlite3_bind_text(stmt, param_index++, updates["status"].asString().c_str(), -1, SQLITE_TRANSIENT);
    }

    sqlite3_bind_text(stmt, param_index++, offer_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, param_index++, creator_pubkey.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to update offer");
    }

    sqlite3_finalize(stmt);

    return getOffer(offer_id);
}

Json MarketplaceManager::getOffer(const std::string& offer_id) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        SELECT * FROM offers WHERE offer_id = ?
    )");

    sqlite3_bind_text(stmt, 1, offer_id.c_str(), -1, SQLITE_TRANSIENT);

    Json result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result["offer_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result["type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result["asset"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        result["amount"] = sqlite3_column_double(stmt, 3);
        result["price"] = sqlite3_column_double(stmt, 4);
        result["currency"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        result["description"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        result["creator_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        result["mediator_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        result["min_trade"] = sqlite3_column_double(stmt, 9);
        result["max_trade"] = sqlite3_column_double(stmt, 10);
        result["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        result["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 14));
        result["updated_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 15));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<Json> MarketplaceManager::listOffers(const std::string& type, const std::string& asset,
                                                   double min_price, double max_price, int limit, int offset) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::stringstream sql;
    sql << "SELECT * FROM offers WHERE status = 'active'";

    if (type != "all" && !type.empty()) {
        sql << " AND type = '" << type << "'";
    }
    if (!asset.empty()) {
        sql << " AND asset = '" << asset << "'";
    }
    if (min_price > 0) {
        sql << " AND price >= " << min_price;
    }
    if (max_price < std::numeric_limits<double>::max()) {
        sql << " AND price <= " << max_price;
    }

    sql << " ORDER BY created_at DESC LIMIT " << limit << " OFFSET " << offset;

    sqlite3_stmt* stmt = conn.Prepare(sql.str().c_str());

    std::vector<Json> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json offer;
        offer["offer_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        offer["type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offer["asset"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        offer["amount"] = sqlite3_column_double(stmt, 3);
        offer["price"] = sqlite3_column_double(stmt, 4);
        offer["currency"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        offer["description"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        offer["creator_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        offer["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        offer["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 14));
        results.push_back(offer);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<Json> MarketplaceManager::search(const std::string& query, const std::string& asset,
                                               double min_price, double max_price,
                                               const std::string& sort_by, const std::string& sort_order) {
    // Similar to listOffers but with LIKE search on description
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::stringstream sql;
    sql << "SELECT * FROM offers WHERE status = 'active'";

    if (!query.empty()) {
        sql << " AND (description LIKE '%" << query << "%' OR asset LIKE '%" << query << "%')";
    }
    if (!asset.empty()) {
        sql << " AND asset = '" << asset << "'";
    }
    if (min_price > 0) {
        sql << " AND price >= " << min_price;
    }
    if (max_price < std::numeric_limits<double>::max()) {
        sql << " AND price <= " << max_price;
    }

    std::string order_col = "created_at";
    if (sort_by == "price") order_col = "price";
    else if (sort_by == "amount") order_col = "amount";

    sql << " ORDER BY " << order_col << " " << (sort_order == "asc" ? "ASC" : "DESC");

    sqlite3_stmt* stmt = conn.Prepare(sql.str().c_str());

    std::vector<Json> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json offer;
        offer["offer_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        offer["type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offer["asset"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        offer["amount"] = sqlite3_column_double(stmt, 3);
        offer["price"] = sqlite3_column_double(stmt, 4);
        offer["currency"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        offer["description"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        offer["creator_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        offer["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        offer["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 14));
        results.push_back(offer);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<Json> MarketplaceManager::getOffersByCreator(const std::string& creator_pubkey, const std::string& status) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::stringstream sql;
    sql << "SELECT * FROM offers WHERE creator_pubkey = ?";
    if (status != "all" && !status.empty()) {
        sql << " AND status = ?";
    }
    sql << " ORDER BY created_at DESC";

    sqlite3_stmt* stmt = conn.Prepare(sql.str().c_str());
    sqlite3_bind_text(stmt, 1, creator_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    if (status != "all" && !status.empty()) {
        sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<Json> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json offer;
        offer["offer_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        offer["type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offer["asset"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        offer["amount"] = sqlite3_column_double(stmt, 3);
        offer["price"] = sqlite3_column_double(stmt, 4);
        offer["currency"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        offer["description"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        offer["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        offer["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 14));
        offer["updated_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 15));
        results.push_back(offer);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ═══════════════════════════════════════════════════════════════
// TRADE MANAGEMENT - Similar pattern, implementing createTrade,
// getTrade, updateTrade, getTradesByUser
// ═══════════════════════════════════════════════════════════════

Json MarketplaceManager::createTrade(const Json& params) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::string trade_id = generateTradeId();
    int64_t now = std::time(nullptr);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT INTO trades (
            trade_id, offer_id, buyer_pubkey, seller_pubkey, mediator_pubkey,
            amount, price, total_value, currency, contract_id, escrow_address,
            status, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");

    sqlite3_bind_text(stmt, 1, trade_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, params["offer_id"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, params["buyer_pubkey"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, params["seller_pubkey"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, params["mediator_pubkey"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 6, params["amount"].asDouble());
    sqlite3_bind_double(stmt, 7, params["price"].asDouble());
    sqlite3_bind_double(stmt, 8, params["total_value"].asDouble());
    sqlite3_bind_text(stmt, 9, params["currency"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, params["contract_id"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, params["escrow_address"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, params["status"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 13, now);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to insert trade");
    }

    sqlite3_finalize(stmt);

    Json result = params;
    result["trade_id"] = trade_id;
    result["created_at"] = now;

    return result;
}

Json MarketplaceManager::getTrade(const std::string& trade_id) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare("SELECT * FROM trades WHERE trade_id = ?");
    sqlite3_bind_text(stmt, 1, trade_id.c_str(), -1, SQLITE_TRANSIENT);

    Json result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result["trade_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result["offer_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result["buyer_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        result["seller_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result["mediator_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result["amount"] = sqlite3_column_double(stmt, 5);
        result["price"] = sqlite3_column_double(stmt, 6);
        result["total_value"] = sqlite3_column_double(stmt, 7);
        result["currency"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        result["contract_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        result["escrow_address"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        result["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        result["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 12));
        result["completed_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 13));
        result["rating"] = sqlite3_column_int(stmt, 14);
        result["review"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool MarketplaceManager::updateTrade(const std::string& trade_id, const Json& updates) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::stringstream sql;
    sql << "UPDATE trades SET ";

    bool first = true;
    if (updates.isMember("status")) {
        sql << "status = ?";
        first = false;
    }
    if (updates.isMember("completed_at")) {
        if (!first) sql << ", ";
        sql << "completed_at = ?";
        first = false;
    }
    if (updates.isMember("rating")) {
        if (!first) sql << ", ";
        sql << "rating = ?";
        first = false;
    }
    if (updates.isMember("review")) {
        if (!first) sql << ", ";
        sql << "review = ?";
        first = false;
    }

    sql << " WHERE trade_id = ?";

    sqlite3_stmt* stmt = conn.Prepare(sql.str().c_str());

    int param_index = 1;
    if (updates.isMember("status")) {
        sqlite3_bind_text(stmt, param_index++, updates["status"].asString().c_str(), -1, SQLITE_TRANSIENT);
    }
    if (updates.isMember("completed_at")) {
        sqlite3_bind_int64(stmt, param_index++, updates["completed_at"].asInt64());
    }
    if (updates.isMember("rating")) {
        sqlite3_bind_int(stmt, param_index++, updates["rating"].asInt());
    }
    if (updates.isMember("review")) {
        sqlite3_bind_text(stmt, param_index++, updates["review"].asString().c_str(), -1, SQLITE_TRANSIENT);
    }

    sqlite3_bind_text(stmt, param_index++, trade_id.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    int changes = sqlite3_changes(conn.db);
    sqlite3_finalize(stmt);

    return changes > 0;
}

std::vector<Json> MarketplaceManager::getTradesByUser(const std::string& user_pubkey, const std::string& role, const std::string& status) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::stringstream sql;
    sql << "SELECT * FROM trades WHERE ";

    if (role == "buyer") {
        sql << "buyer_pubkey = ?";
    } else if (role == "seller") {
        sql << "seller_pubkey = ?";
    } else {
        sql << "(buyer_pubkey = ? OR seller_pubkey = ?)";
    }

    if (status != "all" && !status.empty()) {
        sql << " AND status = ?";
    }

    sql << " ORDER BY created_at DESC";

    sqlite3_stmt* stmt = conn.Prepare(sql.str().c_str());

    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    if (role == "all") {
        sqlite3_bind_text(stmt, 2, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    }

    if (status != "all" && !status.empty()) {
        int status_index = (role == "all") ? 3 : 2;
        sqlite3_bind_text(stmt, status_index, status.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<Json> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json trade;
        trade["trade_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        trade["offer_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        trade["buyer_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        trade["seller_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        trade["amount"] = sqlite3_column_double(stmt, 5);
        trade["total_value"] = sqlite3_column_double(stmt, 7);
        trade["currency"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        trade["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        trade["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 12));
        results.push_back(trade);
    }

    sqlite3_finalize(stmt);
    return results;
}

// ═══════════════════════════════════════════════════════════════
// REPUTATION MANAGEMENT
// ═══════════════════════════════════════════════════════════════

Json MarketplaceManager::getReputation(const std::string& user_pubkey) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare("SELECT * FROM reputations WHERE user_pubkey = ?");
    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);

    Json result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result["user_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result["total_trades"] = sqlite3_column_int(stmt, 1);
        result["successful_trades"] = sqlite3_column_int(stmt, 2);
        result["disputed_trades"] = sqlite3_column_int(stmt, 3);
        result["average_rating"] = sqlite3_column_double(stmt, 4);
        result["rating_distribution"] = ::Json::arrayValue;
        result["rating_distribution"].append(sqlite3_column_int(stmt, 5));
        result["rating_distribution"].append(sqlite3_column_int(stmt, 6));
        result["rating_distribution"].append(sqlite3_column_int(stmt, 7));
        result["rating_distribution"].append(sqlite3_column_int(stmt, 8));
        result["rating_distribution"].append(sqlite3_column_int(stmt, 9));
        result["first_trade_date"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 10));
        result["last_trade_date"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 11));
    } else {
        // No reputation yet - return defaults
        result["user_pubkey"] = user_pubkey;
        result["total_trades"] = 0;
        result["successful_trades"] = 0;
        result["disputed_trades"] = 0;
        result["average_rating"] = 0.0;
        result["rating_distribution"] = ::Json::arrayValue;
        result["rating_distribution"].append(0);
        result["rating_distribution"].append(0);
        result["rating_distribution"].append(0);
        result["rating_distribution"].append(0);
        result["rating_distribution"].append(0);
        result["first_trade_date"] = static_cast<Json::Int64>(0);
        result["last_trade_date"] = static_cast<Json::Int64>(0);
    }

    sqlite3_finalize(stmt);
    return result;
}

void MarketplaceManager::addRating(const std::string& rated_pubkey, int rating, const std::string& review, const std::string& rater_pubkey) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    // Get current reputation or create new
    auto rep = getReputation(rated_pubkey);

    int total_trades = rep["total_trades"].asInt() + 1;
    int successful_trades = rep["successful_trades"].asInt() + 1;

    // Update rating distribution
    std::vector<int> dist;
    if (rep["rating_distribution"].isArray()) {
        for (const auto& count : rep["rating_distribution"]) {
            dist.push_back(count.asInt());
        }
    }
    dist[rating - 1]++;

    // Calculate new average
    double current_avg = rep["average_rating"].asDouble();
    int previous_total = rep["total_trades"].asInt();
    double new_avg = ((current_avg * previous_total) + rating) / total_trades;

    int64_t now = std::time(nullptr);
    int64_t first_trade = rep["first_trade_date"].asInt64();
    if (first_trade == 0) first_trade = now;

    // Upsert reputation
    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT OR REPLACE INTO reputations (
            user_pubkey, total_trades, successful_trades, disputed_trades,
            average_rating, rating_1star, rating_2star, rating_3star, rating_4star, rating_5star,
            first_trade_date, last_trade_date
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");

    sqlite3_bind_text(stmt, 1, rated_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, total_trades);
    sqlite3_bind_int(stmt, 3, successful_trades);
    sqlite3_bind_int(stmt, 4, rep["disputed_trades"].asInt());
    sqlite3_bind_double(stmt, 5, new_avg);
    sqlite3_bind_int(stmt, 6, dist[0]);
    sqlite3_bind_int(stmt, 7, dist[1]);
    sqlite3_bind_int(stmt, 8, dist[2]);
    sqlite3_bind_int(stmt, 9, dist[3]);
    sqlite3_bind_int(stmt, 10, dist[4]);
    sqlite3_bind_int64(stmt, 11, first_trade);
    sqlite3_bind_int64(stmt, 12, now);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ═══════════════════════════════════════════════════════════════
// DISPUTE MANAGEMENT
// ═══════════════════════════════════════════════════════════════

Json MarketplaceManager::createDispute(const Json& params) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    std::string dispute_id = generateDisputeId();
    int64_t now = std::time(nullptr);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT INTO disputes (
            dispute_id, trade_id, complainant_pubkey, reason, evidence,
            mediator_pubkey, status, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )");

    sqlite3_bind_text(stmt, 1, dispute_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, params["trade_id"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, params["complainant_pubkey"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, params["reason"].asString().c_str(), -1, SQLITE_TRANSIENT);
    std::string evidence = params.isMember("evidence") ? params["evidence"].asString() : "";
    sqlite3_bind_text(stmt, 5, evidence.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, params["mediator_pubkey"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, params["status"].asString().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, now);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to create dispute");
    }

    sqlite3_finalize(stmt);

    Json result = params;
    result["dispute_id"] = dispute_id;
    result["created_at"] = now;

    return result;
}

Json MarketplaceManager::getDispute(const std::string& dispute_id) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare("SELECT * FROM disputes WHERE dispute_id = ?");
    sqlite3_bind_text(stmt, 1, dispute_id.c_str(), -1, SQLITE_TRANSIENT);

    Json result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result["dispute_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result["trade_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result["complainant_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        result["reason"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result["evidence"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result["mediator_pubkey"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        result["resolution"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        result["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        result["created_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 8));
        result["resolved_at"] = static_cast<Json::Int64>(sqlite3_column_int64(stmt, 9));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool MarketplaceManager::resolveDispute(const std::string& dispute_id, const std::string& resolution, const std::string& mediator_pubkey) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::string db_path = data_dir_ + "/marketplace.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        UPDATE disputes
        SET resolution = ?, status = 'resolved', resolved_at = ?
        WHERE dispute_id = ? AND mediator_pubkey = ?
    )");

    int64_t now = std::time(nullptr);
    sqlite3_bind_text(stmt, 1, resolution.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_text(stmt, 3, dispute_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, mediator_pubkey.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    int changes = sqlite3_changes(conn.db);
    sqlite3_finalize(stmt);

    return changes > 0;
}

} // namespace din
