#include "daemon/peer_reputation_db.h"
#include "common/logger.h"
#include <sqlite3.h>
#include <sstream>
#include <ctime>
#include <algorithm>

namespace dinero {

PeerReputationDB::PeerReputationDB()
    : m_db(nullptr),
      m_stmt_insert_peer(nullptr),
      m_stmt_get_peer(nullptr),
      m_stmt_update_score(nullptr),
      m_stmt_record_attempt(nullptr),
      m_stmt_record_success(nullptr),
      m_stmt_record_failure(nullptr),
      m_stmt_ban_peer(nullptr),
      m_stmt_unban_peer(nullptr),
      m_stmt_check_ban(nullptr) {
}

PeerReputationDB::~PeerReputationDB() {
    close();
}

bool PeerReputationDB::open(const std::string& db_path) {
    if (m_db != nullptr) {
        g_logger.warning("Database already open");
        return true;
    }

    int rc = sqlite3_open(db_path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        g_logger.error("Failed to open peer reputation database: " + std::string(sqlite3_errmsg(m_db)));
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    // Enable WAL mode for better concurrency
    char* err_msg = nullptr;
    rc = sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.warning("Failed to enable WAL mode: " + std::string(err_msg));
        sqlite3_free(err_msg);
    }

    // Create schema
    if (!createSchema()) {
        g_logger.error("Failed to create peer reputation schema");
        close();
        return false;
    }

    // Prepare statements
    if (!prepareStatements()) {
        g_logger.error("Failed to prepare statements");
        close();
        return false;
    }

    g_logger.info("📊 Peer reputation database opened: " + db_path);
    return true;
}

void PeerReputationDB::close() {
    finalizeStatements();

    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
        g_logger.info("📊 Peer reputation database closed");
    }
}

bool PeerReputationDB::createSchema() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS peers (
            ip TEXT NOT NULL,
            port INTEGER NOT NULL,
            score INTEGER DEFAULT 0,
            connection_attempts INTEGER DEFAULT 0,
            successful_connections INTEGER DEFAULT 0,
            failed_connections INTEGER DEFAULT 0,
            ban_count INTEGER DEFAULT 0,
            is_banned INTEGER DEFAULT 0,
            ban_until INTEGER DEFAULT 0,
            last_seen INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            invalid_blocks INTEGER DEFAULT 0,
            invalid_txs INTEGER DEFAULT 0,
            protocol_violations INTEGER DEFAULT 0,
            -- Week 7: Positive reputation metrics
            total_uptime_seconds INTEGER DEFAULT 0,
            connection_start_time INTEGER DEFAULT 0,
            avg_latency_ms REAL DEFAULT 0.0,
            reliability_ratio REAL DEFAULT 0.0,
            PRIMARY KEY (ip, port)
        );

        CREATE INDEX IF NOT EXISTS idx_peers_score ON peers(score DESC);
        CREATE INDEX IF NOT EXISTS idx_peers_banned ON peers(is_banned, ban_until);
        CREATE INDEX IF NOT EXISTS idx_peers_last_seen ON peers(last_seen);
    )";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        g_logger.error("Failed to create schema: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool PeerReputationDB::prepareStatements() {
    // Insert or update peer
    const char* sql_insert = R"(
        INSERT INTO peers (ip, port, last_seen, created_at)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(ip, port) DO UPDATE SET last_seen = excluded.last_seen
    )";

    if (sqlite3_prepare_v2(m_db, sql_insert, -1, &m_stmt_insert_peer, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare insert statement");
        return false;
    }

    // Get peer
    const char* sql_get = "SELECT * FROM peers WHERE ip = ? AND port = ?";
    if (sqlite3_prepare_v2(m_db, sql_get, -1, &m_stmt_get_peer, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare get statement");
        return false;
    }

    // Update score
    const char* sql_score = R"(
        UPDATE peers SET score = CASE
            WHEN score + ? > ? THEN ?
            WHEN score + ? < ? THEN ?
            ELSE score + ?
        END, last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    if (sqlite3_prepare_v2(m_db, sql_score, -1, &m_stmt_update_score, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare score update statement");
        return false;
    }

    // Record attempt
    const char* sql_attempt = R"(
        UPDATE peers SET connection_attempts = connection_attempts + 1, last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    if (sqlite3_prepare_v2(m_db, sql_attempt, -1, &m_stmt_record_attempt, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare attempt statement");
        return false;
    }

    // Record success
    const char* sql_success = R"(
        UPDATE peers SET successful_connections = successful_connections + 1, last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    if (sqlite3_prepare_v2(m_db, sql_success, -1, &m_stmt_record_success, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare success statement");
        return false;
    }

    // Record failure
    const char* sql_failure = R"(
        UPDATE peers SET failed_connections = failed_connections + 1, last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    if (sqlite3_prepare_v2(m_db, sql_failure, -1, &m_stmt_record_failure, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare failure statement");
        return false;
    }

    // Ban peer
    const char* sql_ban = R"(
        UPDATE peers SET is_banned = 1, ban_until = ?, ban_count = ban_count + 1, last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    if (sqlite3_prepare_v2(m_db, sql_ban, -1, &m_stmt_ban_peer, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare ban statement");
        return false;
    }

    // Unban peer
    const char* sql_unban = "UPDATE peers SET is_banned = 0, ban_until = 0 WHERE ip = ? AND port = ?";
    if (sqlite3_prepare_v2(m_db, sql_unban, -1, &m_stmt_unban_peer, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare unban statement");
        return false;
    }

    // Check ban
    const char* sql_check = "SELECT is_banned, ban_until FROM peers WHERE ip = ? AND port = ?";
    if (sqlite3_prepare_v2(m_db, sql_check, -1, &m_stmt_check_ban, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare ban check statement");
        return false;
    }

    return true;
}

void PeerReputationDB::finalizeStatements() {
    if (m_stmt_insert_peer) sqlite3_finalize(m_stmt_insert_peer);
    if (m_stmt_get_peer) sqlite3_finalize(m_stmt_get_peer);
    if (m_stmt_update_score) sqlite3_finalize(m_stmt_update_score);
    if (m_stmt_record_attempt) sqlite3_finalize(m_stmt_record_attempt);
    if (m_stmt_record_success) sqlite3_finalize(m_stmt_record_success);
    if (m_stmt_record_failure) sqlite3_finalize(m_stmt_record_failure);
    if (m_stmt_ban_peer) sqlite3_finalize(m_stmt_ban_peer);
    if (m_stmt_unban_peer) sqlite3_finalize(m_stmt_unban_peer);
    if (m_stmt_check_ban) sqlite3_finalize(m_stmt_check_ban);

    m_stmt_insert_peer = nullptr;
    m_stmt_get_peer = nullptr;
    m_stmt_update_score = nullptr;
    m_stmt_record_attempt = nullptr;
    m_stmt_record_success = nullptr;
    m_stmt_record_failure = nullptr;
    m_stmt_ban_peer = nullptr;
    m_stmt_unban_peer = nullptr;
    m_stmt_check_ban = nullptr;
}

std::string PeerReputationDB::getPeerKey(const std::string& ip, uint16_t port) const {
    return ip + ":" + std::to_string(port);
}

bool PeerReputationDB::addOrUpdatePeer(const std::string& ip, uint16_t port) {
    if (!m_stmt_insert_peer) return false;

    sqlite3_reset(m_stmt_insert_peer);

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    sqlite3_bind_text(m_stmt_insert_peer, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_insert_peer, 2, port);
    sqlite3_bind_int64(m_stmt_insert_peer, 3, now_ts);
    sqlite3_bind_int64(m_stmt_insert_peer, 4, now_ts);

    int rc = sqlite3_step(m_stmt_insert_peer);
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to insert/update peer: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    return true;
}

bool PeerReputationDB::getPeer(const std::string& ip, uint16_t port, PeerReputation& out) const {
    if (!m_stmt_get_peer) return false;

    sqlite3_reset(m_stmt_get_peer);
    sqlite3_bind_text(m_stmt_get_peer, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_get_peer, 2, port);

    int rc = sqlite3_step(m_stmt_get_peer);
    if (rc != SQLITE_ROW) {
        return false;
    }

    // Parse result
    out.ip = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_peer, 0));
    out.port = sqlite3_column_int(m_stmt_get_peer, 1);
    out.score = sqlite3_column_int(m_stmt_get_peer, 2);
    out.connection_attempts = sqlite3_column_int(m_stmt_get_peer, 3);
    out.successful_connections = sqlite3_column_int(m_stmt_get_peer, 4);
    out.failed_connections = sqlite3_column_int(m_stmt_get_peer, 5);
    out.ban_count = sqlite3_column_int(m_stmt_get_peer, 6);
    out.is_banned = sqlite3_column_int(m_stmt_get_peer, 7) != 0;

    int64_t ban_until_ts = sqlite3_column_int64(m_stmt_get_peer, 8);
    int64_t last_seen_ts = sqlite3_column_int64(m_stmt_get_peer, 9);
    int64_t created_at_ts = sqlite3_column_int64(m_stmt_get_peer, 10);

    out.ban_until = std::chrono::system_clock::from_time_t(ban_until_ts);
    out.last_seen = std::chrono::system_clock::from_time_t(last_seen_ts);
    out.created_at = std::chrono::system_clock::from_time_t(created_at_ts);

    out.invalid_blocks = sqlite3_column_int(m_stmt_get_peer, 11);
    out.invalid_txs = sqlite3_column_int(m_stmt_get_peer, 12);
    out.protocol_violations = sqlite3_column_int(m_stmt_get_peer, 13);

    // Week 7: Read positive reputation metrics (if columns exist)
    if (sqlite3_column_count(m_stmt_get_peer) > 14) {
        out.total_uptime_seconds = sqlite3_column_int64(m_stmt_get_peer, 14);
        out.connection_start_time = sqlite3_column_int64(m_stmt_get_peer, 15);
        out.avg_latency_ms = sqlite3_column_double(m_stmt_get_peer, 16);
        out.reliability_ratio = sqlite3_column_double(m_stmt_get_peer, 17);
    }

    return true;
}

bool PeerReputationDB::removePeer(const std::string& ip, uint16_t port) {
    const char* sql = "DELETE FROM peers WHERE ip = ? AND port = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool PeerReputationDB::recordAttempt(const std::string& ip, uint16_t port) {
    if (!m_stmt_record_attempt) return false;

    // Ensure peer exists first
    addOrUpdatePeer(ip, port);

    sqlite3_reset(m_stmt_record_attempt);

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    sqlite3_bind_int64(m_stmt_record_attempt, 1, now_ts);
    sqlite3_bind_text(m_stmt_record_attempt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_record_attempt, 3, port);

    int rc = sqlite3_step(m_stmt_record_attempt);
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to record attempt: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    // Small penalty for attempt
    adjustScore(ip, port, -1);

    return true;
}

bool PeerReputationDB::recordSuccess(const std::string& ip, uint16_t port) {
    if (!m_stmt_record_success) return false;

    sqlite3_reset(m_stmt_record_success);

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    sqlite3_bind_int64(m_stmt_record_success, 1, now_ts);
    sqlite3_bind_text(m_stmt_record_success, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_record_success, 3, port);

    int rc = sqlite3_step(m_stmt_record_success);
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to record success: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    // Week 7: Record connection start for uptime tracking
    recordConnectionStart(ip, port);
    
    // Update reliability ratio
    double reliability = calculateReliability(ip, port);
    const char* sql_update = "UPDATE peers SET reliability_ratio = ? WHERE ip = ? AND port = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql_update, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_double(stmt, 1, reliability);
        sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, port);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Reward for successful connection
    adjustScore(ip, port, 10);

    return true;
}

bool PeerReputationDB::recordFailure(const std::string& ip, uint16_t port) {
    if (!m_stmt_record_failure) return false;

    sqlite3_reset(m_stmt_record_failure);

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    sqlite3_bind_int64(m_stmt_record_failure, 1, now_ts);
    sqlite3_bind_text(m_stmt_record_failure, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_record_failure, 3, port);

    int rc = sqlite3_step(m_stmt_record_failure);
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to record failure: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    // Penalty for failed connection
    adjustScore(ip, port, -5);

    return true;
}

bool PeerReputationDB::recordMisbehavior(const std::string& ip, uint16_t port, MisbehaviorType type) {
    // Increment misbehavior count
    if (!incrementMisbehaviorCount(ip, port, type)) {
        return false;
    }

    // Apply penalty based on type
    int32_t penalty = 0;
    switch (type) {
        case MisbehaviorType::INVALID_BLOCK:
            penalty = INVALID_BLOCK_PENALTY;
            break;
        case MisbehaviorType::INVALID_TX:
            penalty = INVALID_TX_PENALTY;
            break;
        case MisbehaviorType::PROTOCOL_VIOLATION:
            penalty = PROTOCOL_VIOLATION_PENALTY;
            break;
        case MisbehaviorType::TIMEOUT:
            penalty = TIMEOUT_PENALTY;
            break;
        case MisbehaviorType::EXCESSIVE_INV:
            penalty = EXCESSIVE_INV_PENALTY;
            break;
        default:
            penalty = -10;
    }

    adjustScore(ip, port, penalty);

    // Check if we should auto-ban
    PeerReputation peer;
    if (getPeer(ip, port, peer)) {
        if (peer.score <= AUTO_BAN_THRESHOLD) {
            auto duration = getNextBanDuration(peer.ban_count);
            banPeer(ip, port, duration);
            g_logger.warning("🚫 Auto-banned peer " + getPeerKey(ip, port) +
                           " (score: " + std::to_string(peer.score) + ")");
        }
    }

    return true;
}

bool PeerReputationDB::incrementMisbehaviorCount(const std::string& ip, uint16_t port, MisbehaviorType type) {
    std::string field;
    switch (type) {
        case MisbehaviorType::INVALID_BLOCK:
            field = "invalid_blocks";
            break;
        case MisbehaviorType::INVALID_TX:
            field = "invalid_txs";
            break;
        case MisbehaviorType::PROTOCOL_VIOLATION:
            field = "protocol_violations";
            break;
        default:
            return true; // No specific counter for other types
    }

    std::string sql = "UPDATE peers SET " + field + " = " + field + " + 1 WHERE ip = ? AND port = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool PeerReputationDB::banPeer(const std::string& ip, uint16_t port, std::chrono::seconds duration) {
    if (!m_stmt_ban_peer) return false;

    sqlite3_reset(m_stmt_ban_peer);

    auto now = std::chrono::system_clock::now();
    auto ban_until = now + duration;
    auto ban_until_ts = std::chrono::duration_cast<std::chrono::seconds>(ban_until.time_since_epoch()).count();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    sqlite3_bind_int64(m_stmt_ban_peer, 1, ban_until_ts);
    sqlite3_bind_int64(m_stmt_ban_peer, 2, now_ts);
    sqlite3_bind_text(m_stmt_ban_peer, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_ban_peer, 4, port);

    int rc = sqlite3_step(m_stmt_ban_peer);
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to ban peer: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    g_logger.info("🚫 Banned peer " + getPeerKey(ip, port) + " for " +
                 std::to_string(duration.count()) + " seconds");

    return true;
}

bool PeerReputationDB::unbanPeer(const std::string& ip, uint16_t port) {
    if (!m_stmt_unban_peer) return false;

    sqlite3_reset(m_stmt_unban_peer);
    sqlite3_bind_text(m_stmt_unban_peer, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_unban_peer, 2, port);

    int rc = sqlite3_step(m_stmt_unban_peer);
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to unban peer: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    g_logger.info("✅ Unbanned peer " + getPeerKey(ip, port));
    return true;
}

bool PeerReputationDB::isPeerBanned(const std::string& ip, uint16_t port) const {
    if (!m_stmt_check_ban) return false;

    sqlite3_reset(m_stmt_check_ban);
    sqlite3_bind_text(m_stmt_check_ban, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_check_ban, 2, port);

    int rc = sqlite3_step(m_stmt_check_ban);
    if (rc != SQLITE_ROW) {
        return false;
    }

    bool is_banned = sqlite3_column_int(m_stmt_check_ban, 0) != 0;
    int64_t ban_until_ts = sqlite3_column_int64(m_stmt_check_ban, 1);

    if (!is_banned) {
        return false;
    }

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    return now_ts < ban_until_ts;
}

std::chrono::seconds PeerReputationDB::getNextBanDuration(uint32_t ban_count) const {
    switch (ban_count) {
        case 0:
            return FIRST_BAN_DURATION;
        case 1:
            return SECOND_BAN_DURATION;
        case 2:
            return THIRD_BAN_DURATION;
        default:
            return PERMANENT_BAN_DURATION;
    }
}

bool PeerReputationDB::adjustScore(const std::string& ip, uint16_t port, int32_t delta) {
    if (!m_stmt_update_score) return false;

    sqlite3_reset(m_stmt_update_score);

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // Bind for all CASE clauses
    sqlite3_bind_int(m_stmt_update_score, 1, delta);
    sqlite3_bind_int(m_stmt_update_score, 2, MAX_SCORE);
    sqlite3_bind_int(m_stmt_update_score, 3, MAX_SCORE);
    sqlite3_bind_int(m_stmt_update_score, 4, delta);
    sqlite3_bind_int(m_stmt_update_score, 5, MIN_SCORE);
    sqlite3_bind_int(m_stmt_update_score, 6, MIN_SCORE);
    sqlite3_bind_int(m_stmt_update_score, 7, delta);
    sqlite3_bind_int64(m_stmt_update_score, 8, now_ts);
    sqlite3_bind_text(m_stmt_update_score, 9, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_update_score, 10, port);

    int rc = sqlite3_step(m_stmt_update_score);
    return rc == SQLITE_DONE;
}

bool PeerReputationDB::resetScore(const std::string& ip, uint16_t port) {
    const char* sql = "UPDATE peers SET score = 0 WHERE ip = ? AND port = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<PeerReputation> PeerReputationDB::getGoodPeers(size_t limit) const {
    std::vector<PeerReputation> results;

    const char* sql = "SELECT * FROM peers WHERE score >= ? AND is_banned = 0 ORDER BY score DESC LIMIT ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int(stmt, 1, GOOD_PEER_THRESHOLD);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PeerReputation peer;
        peer.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        peer.port = sqlite3_column_int(stmt, 1);
        peer.score = sqlite3_column_int(stmt, 2);
        peer.connection_attempts = sqlite3_column_int(stmt, 3);
        peer.successful_connections = sqlite3_column_int(stmt, 4);
        peer.failed_connections = sqlite3_column_int(stmt, 5);
        peer.ban_count = sqlite3_column_int(stmt, 6);
        peer.is_banned = sqlite3_column_int(stmt, 7) != 0;

        results.push_back(peer);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<PeerReputation> PeerReputationDB::getBannedPeers() const {
    std::vector<PeerReputation> results;

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    const char* sql = "SELECT * FROM peers WHERE is_banned = 1 AND ban_until > ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int64(stmt, 1, now_ts);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PeerReputation peer;
        peer.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        peer.port = sqlite3_column_int(stmt, 1);
        peer.score = sqlite3_column_int(stmt, 2);
        peer.is_banned = true;

        results.push_back(peer);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<PeerReputation> PeerReputationDB::getPeersByScore(int32_t min_score, size_t limit) const {
    std::vector<PeerReputation> results;

    const char* sql = "SELECT * FROM peers WHERE score >= ? ORDER BY score DESC LIMIT ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int(stmt, 1, min_score);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PeerReputation peer;
        peer.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        peer.port = sqlite3_column_int(stmt, 1);
        peer.score = sqlite3_column_int(stmt, 2);

        results.push_back(peer);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool PeerReputationDB::cleanupStalePeers(std::chrono::seconds max_age) {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - max_age;
    auto cutoff_ts = std::chrono::duration_cast<std::chrono::seconds>(cutoff.time_since_epoch()).count();

    const char* sql = "DELETE FROM peers WHERE last_seen < ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, cutoff_ts);

    int rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    if (deleted > 0) {
        g_logger.info("🧹 Cleaned up " + std::to_string(deleted) + " stale peers");
    }

    return rc == SQLITE_DONE;
}

bool PeerReputationDB::expireBans() {
    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    const char* sql = "UPDATE peers SET is_banned = 0 WHERE is_banned = 1 AND ban_until < ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, now_ts);

    int rc = sqlite3_step(stmt);
    int expired = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    if (expired > 0) {
        g_logger.info("✅ Expired " + std::to_string(expired) + " peer bans");
    }

    return rc == SQLITE_DONE;
}

bool PeerReputationDB::vacuum() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "VACUUM;", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        g_logger.error("Failed to vacuum database: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }

    g_logger.info("🧹 Database vacuum complete");
    return true;
}

PeerReputationDB::Stats PeerReputationDB::getStats() const {
    Stats stats{0, 0, 0, 0};

    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    const char* sql = R"(
        SELECT
            COUNT(*) as total,
            SUM(CASE WHEN is_banned = 1 AND ban_until > ? THEN 1 ELSE 0 END) as banned,
            SUM(CASE WHEN score >= ? THEN 1 ELSE 0 END) as good,
            SUM(invalid_blocks + invalid_txs + protocol_violations) as misbehaviors
        FROM peers
    )";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }

    sqlite3_bind_int64(stmt, 1, now_ts);
    sqlite3_bind_int(stmt, 2, GOOD_PEER_THRESHOLD);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_peers = sqlite3_column_int64(stmt, 0);
        stats.banned_peers = sqlite3_column_int64(stmt, 1);
        stats.good_peers = sqlite3_column_int64(stmt, 2);
        stats.total_misbehaviors = sqlite3_column_int64(stmt, 3);
    }

    sqlite3_finalize(stmt);
    return stats;
}

// Week 7: Positive reputation metrics implementation

bool PeerReputationDB::recordConnectionStart(const std::string& ip, uint16_t port) {
    // Ensure peer exists
    addOrUpdatePeer(ip, port);
    
    const char* sql = R"(
        UPDATE peers 
        SET connection_start_time = ?,
            last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    
    sqlite3_bind_int64(stmt, 1, now_ts);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_text(stmt, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, port);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool PeerReputationDB::recordConnectionEnd(const std::string& ip, uint16_t port) {
    // Get current connection start time
    PeerReputation peer;
    if (!getPeer(ip, port, peer)) {
        return false;
    }
    
    if (peer.connection_start_time == 0) {
        return true; // Not connected, nothing to do
    }
    
    // Calculate uptime for this session
    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    uint64_t session_uptime = (now_ts > peer.connection_start_time) ? 
                              (now_ts - peer.connection_start_time) : 0;
    
    const char* sql = R"(
        UPDATE peers 
        SET total_uptime_seconds = total_uptime_seconds + ?,
            connection_start_time = 0,
            last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int64(stmt, 1, session_uptime);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_text(stmt, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, port);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool PeerReputationDB::updateLatency(const std::string& ip, uint16_t port, double latency_ms) {
    // Ensure peer exists
    addOrUpdatePeer(ip, port);
    
    // Get current average latency
    PeerReputation peer;
    if (!getPeer(ip, port, peer)) {
        return false;
    }
    
    // Exponential moving average: new_avg = alpha * new_value + (1 - alpha) * old_avg
    // alpha = 0.1 gives more weight to recent values
    constexpr double alpha = 0.1;
    double new_avg = (peer.avg_latency_ms == 0.0) ? latency_ms :
                     (alpha * latency_ms + (1.0 - alpha) * peer.avg_latency_ms);
    
    const char* sql = R"(
        UPDATE peers 
        SET avg_latency_ms = ?,
            last_seen = ?
        WHERE ip = ? AND port = ?
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    auto now = std::chrono::system_clock::now();
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    
    sqlite3_bind_double(stmt, 1, new_avg);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_text(stmt, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, port);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

double PeerReputationDB::calculateReliability(const std::string& ip, uint16_t port) const {
    PeerReputation peer;
    if (!getPeer(ip, port, peer)) {
        return 0.0;
    }
    
    if (peer.connection_attempts == 0) {
        return 0.0;
    }
    
    return static_cast<double>(peer.successful_connections) / 
           static_cast<double>(peer.connection_attempts);
}

uint64_t PeerReputationDB::getUptime(const std::string& ip, uint16_t port) const {
    PeerReputation peer;
    if (!getPeer(ip, port, peer)) {
        return 0;
    }
    
    uint64_t total_uptime = peer.total_uptime_seconds;
    
    // Add current session uptime if connected
    if (peer.connection_start_time > 0) {
        auto now = std::chrono::system_clock::now();
        auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        if (now_ts > peer.connection_start_time) {
            total_uptime += (now_ts - peer.connection_start_time);
        }
    }
    
    return total_uptime;
}

} // namespace dinero
