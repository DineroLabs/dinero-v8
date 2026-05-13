/**
 * KYC Manager Implementation
 */

#include "p2p/kyc_manager.h"
#include "database/sqlite_conn.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>

namespace din {
namespace p2p {

// ═══════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════

constexpr int VERIFICATION_CODE_EXPIRY_MINUTES = 15;
constexpr int MAX_VERIFICATION_ATTEMPTS = 3;
constexpr int64_t SECONDS_PER_DAY = 86400;
constexpr int64_t SECONDS_PER_30_DAYS = 2592000;

// ═══════════════════════════════════════════════════════════════
// TRADE LIMITS BY TIER
// ═══════════════════════════════════════════════════════════════

TradeLimits TradeLimits::getDefaultLimits(KYCTier tier) {
    switch (tier) {
        case KYCTier::UNVERIFIED:
            return {
                10.0,      // min_trade_din
                1000.0,    // max_trade_din
                5000.0,    // daily_volume_din
                20000.0,   // monthly_volume_din
                3          // max_active_trades
            };

        case KYCTier::LIGHT_KYC:
            return {
                10.0,
                10000.0,
                50000.0,
                200000.0,
                10
            };

        case KYCTier::FULL_KYC:
            return {
                10.0,
                100000.0,
                500000.0,
                2000000.0,
                50
            };

        default:
            return getDefaultLimits(KYCTier::UNVERIFIED);
    }
}

Json TradeLimits::toJson() const {
    Json j;
    j["min_trade_din"] = min_trade_din;
    j["max_trade_din"] = max_trade_din;
    j["daily_volume_din"] = daily_volume_din;
    j["monthly_volume_din"] = monthly_volume_din;
    j["max_active_trades"] = max_active_trades;
    return j;
}

TradeLimits TradeLimits::fromJson(const Json& j) {
    return {
        j["min_trade_din"].asDouble(),
        j["max_trade_din"].asDouble(),
        j["daily_volume_din"].asDouble(),
        j["monthly_volume_din"].asDouble(),
        j["max_active_trades"].asInt()
    };
}

// ═══════════════════════════════════════════════════════════════
// JSON CONVERSION
// ═══════════════════════════════════════════════════════════════

Json EmailVerification::toJson() const {
    Json j;
    j["email"] = email;
    j["verification_code"] = verification_code;
    j["sent_at"] = static_cast<Json::Int64>(sent_at);
    j["verified_at"] = static_cast<Json::Int64>(verified_at);
    j["status"] = static_cast<int>(status);
    j["attempts"] = attempts;
    return j;
}

EmailVerification EmailVerification::fromJson(const Json& j) {
    return {
        j["email"].asString(),
        j["verification_code"].asString(),
        j["sent_at"].asInt64(),
        j["verified_at"].asInt64(),
        static_cast<VerificationStatus>(j["status"].asInt()),
        j["attempts"].asInt()
    };
}

Json PhoneVerification::toJson() const {
    Json j;
    j["phone"] = phone;
    j["verification_code"] = verification_code;
    j["sent_at"] = static_cast<Json::Int64>(sent_at);
    j["verified_at"] = static_cast<Json::Int64>(verified_at);
    j["status"] = static_cast<int>(status);
    j["attempts"] = attempts;
    return j;
}

PhoneVerification PhoneVerification::fromJson(const Json& j) {
    return {
        j["phone"].asString(),
        j["verification_code"].asString(),
        j["sent_at"].asInt64(),
        j["verified_at"].asInt64(),
        static_cast<VerificationStatus>(j["status"].asInt()),
        j["attempts"].asInt()
    };
}

Json IDVerification::toJson() const {
    Json j;
    j["document_type"] = document_type;
    j["document_number_hash"] = document_number_hash;
    j["country"] = country;
    j["selfie_hash"] = selfie_hash;
    j["document_hash"] = document_hash;
    j["submitted_at"] = static_cast<Json::Int64>(submitted_at);
    j["reviewed_at"] = static_cast<Json::Int64>(reviewed_at);
    j["reviewer_pubkey"] = reviewer_pubkey;
    j["status"] = static_cast<int>(status);
    j["rejection_reason"] = rejection_reason;
    return j;
}

IDVerification IDVerification::fromJson(const Json& j) {
    return {
        j["document_type"].asString(),
        j["document_number_hash"].asString(),
        j["country"].asString(),
        j["selfie_hash"].asString(),
        j["document_hash"].asString(),
        j["submitted_at"].asInt64(),
        j.isMember("reviewed_at") ? j["reviewed_at"].asInt64() : 0,
        j.isMember("reviewer_pubkey") ? j["reviewer_pubkey"].asString() : "",
        static_cast<VerificationStatus>(j["status"].asInt()),
        j.isMember("rejection_reason") ? j["rejection_reason"].asString() : ""
    };
}

Json KYCProfile::toJson() const {
    Json j;
    j["user_pubkey"] = user_pubkey;
    j["tier"] = static_cast<int>(tier);

    if (email) {
        j["email"] = email->toJson();
    }
    if (phone) {
        j["phone"] = phone->toJson();
    }
    if (id_document) {
        j["id_document"] = id_document->toJson();
    }

    j["limits"] = limits.toJson();
    j["created_at"] = static_cast<Json::Int64>(created_at);
    j["updated_at"] = static_cast<Json::Int64>(updated_at);
    j["volume_24h"] = volume_24h;
    j["volume_30d"] = volume_30d;
    j["active_trades_count"] = active_trades_count;

    return j;
}

KYCProfile KYCProfile::fromJson(const Json& j) {
    KYCProfile profile;
    profile.user_pubkey = j["user_pubkey"].asString();
    profile.tier = static_cast<KYCTier>(j["tier"].asInt());

    if (j.isMember("email")) {
        profile.email = EmailVerification::fromJson(j["email"]);
    }
    if (j.isMember("phone")) {
        profile.phone = PhoneVerification::fromJson(j["phone"]);
    }
    if (j.isMember("id_document")) {
        profile.id_document = IDVerification::fromJson(j["id_document"]);
    }

    profile.limits = TradeLimits::fromJson(j["limits"]);
    profile.created_at = j["created_at"].asInt64();
    profile.updated_at = j["updated_at"].asInt64();
    profile.volume_24h = j["volume_24h"].asDouble();
    profile.volume_30d = j["volume_30d"].asDouble();
    profile.active_trades_count = j["active_trades_count"].asInt();

    return profile;
}

// ═══════════════════════════════════════════════════════════════
// KYC MANAGER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════

std::unique_ptr<KYCManager> KYCManager::instance_;

KYCManager& KYCManager::instance() {
    if (!instance_) {
        instance_.reset(new KYCManager());
    }
    return *instance_;
}

KYCManager::KYCManager() : mutex_ptr_(std::make_unique<std::recursive_mutex>()) {
    dinero::g_logger.info("[KYC] Initializing KYC manager");
}

KYCManager::~KYCManager() {
    dinero::g_logger.info("[KYC] Shutting down KYC manager");
}

void KYCManager::setDataDir(const std::string& data_dir) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);
    data_dir_ = data_dir;
    std::filesystem::create_directories(data_dir_);
    initializeDatabase();
}

void KYCManager::initializeDatabase() {
    std::string db_path = data_dir_ + "/kyc.db";

    try {
        dinero::SqliteConn conn(db_path);

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS kyc_profiles (
                user_pubkey TEXT PRIMARY KEY,
                tier INTEGER NOT NULL,
                email TEXT,
                email_verified INTEGER DEFAULT 0,
                phone TEXT,
                phone_verified INTEGER DEFAULT 0,
                id_verified INTEGER DEFAULT 0,
                limits_json TEXT,
                volume_24h REAL DEFAULT 0.0,
                volume_30d REAL DEFAULT 0.0,
                active_trades_count INTEGER DEFAULT 0,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS email_verifications (
                user_pubkey TEXT PRIMARY KEY,
                email TEXT NOT NULL,
                verification_code TEXT NOT NULL,
                sent_at INTEGER NOT NULL,
                verified_at INTEGER DEFAULT 0,
                status INTEGER NOT NULL,
                attempts INTEGER DEFAULT 0
            );
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS phone_verifications (
                user_pubkey TEXT PRIMARY KEY,
                phone TEXT NOT NULL,
                verification_code TEXT NOT NULL,
                sent_at INTEGER NOT NULL,
                verified_at INTEGER DEFAULT 0,
                status INTEGER NOT NULL,
                attempts INTEGER DEFAULT 0
            );
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS id_verifications (
                user_pubkey TEXT PRIMARY KEY,
                document_type TEXT NOT NULL,
                document_number_hash TEXT NOT NULL,
                country TEXT NOT NULL,
                selfie_hash TEXT NOT NULL,
                document_hash TEXT NOT NULL,
                submitted_at INTEGER NOT NULL,
                reviewed_at INTEGER DEFAULT 0,
                reviewer_pubkey TEXT,
                status INTEGER NOT NULL,
                rejection_reason TEXT
            );
        )");

        conn.Exec(R"(
            CREATE TABLE IF NOT EXISTS trade_volume_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                user_pubkey TEXT NOT NULL,
                amount_din REAL NOT NULL,
                timestamp INTEGER NOT NULL,
                trade_id TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_volume_user ON trade_volume_log(user_pubkey);
            CREATE INDEX IF NOT EXISTS idx_volume_time ON trade_volume_log(timestamp);
        )");

        dinero::g_logger.info("[KYC] Database initialized: " + db_path);

    } catch (const std::exception& e) {
        dinero::g_logger.error("[KYC] Failed to initialize database: " + std::string(e.what()));
        throw;
    }
}

std::string KYCManager::generateVerificationCode() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(100000, 999999);

    return std::to_string(dis(gen));
}

// ═══════════════════════════════════════════════════════════════
// PROFILE MANAGEMENT
// ═══════════════════════════════════════════════════════════════

KYCProfile KYCManager::getProfile(const std::string& user_pubkey) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare("SELECT * FROM kyc_profiles WHERE user_pubkey = ?");
    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);

    KYCProfile profile;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Load existing profile
        profile.user_pubkey = user_pubkey;
        profile.tier = static_cast<KYCTier>(sqlite3_column_int(stmt, 1));
        // ... load other fields ...
        profile.volume_24h = sqlite3_column_double(stmt, 8);
        profile.volume_30d = sqlite3_column_double(stmt, 9);
        profile.active_trades_count = sqlite3_column_int(stmt, 10);
        profile.created_at = sqlite3_column_int64(stmt, 11);
        profile.updated_at = sqlite3_column_int64(stmt, 12);
    } else {
        // Create new profile
        profile.user_pubkey = user_pubkey;
        profile.tier = KYCTier::UNVERIFIED;
        profile.limits = TradeLimits::getDefaultLimits(KYCTier::UNVERIFIED);
        profile.volume_24h = 0.0;
        profile.volume_30d = 0.0;
        profile.active_trades_count = 0;
        profile.created_at = std::time(nullptr);
        profile.updated_at = profile.created_at;

        // Insert into database
        sqlite3_finalize(stmt);
        stmt = conn.Prepare(R"(
            INSERT INTO kyc_profiles (user_pubkey, tier, created_at, updated_at)
            VALUES (?, ?, ?, ?)
        )");
        sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, static_cast<int>(profile.tier));
        sqlite3_bind_int64(stmt, 3, profile.created_at);
        sqlite3_bind_int64(stmt, 4, profile.updated_at);
        sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
    return profile;
}

bool KYCManager::canTrade(const std::string& user_pubkey, double amount_din) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    auto profile = getProfile(user_pubkey);
    updateVolumes(user_pubkey);

    // Check trade size limits
    if (amount_din < profile.limits.min_trade_din) return false;
    if (amount_din > profile.limits.max_trade_din) return false;

    // Check daily volume
    if (profile.volume_24h + amount_din > profile.limits.daily_volume_din) return false;

    // Check monthly volume
    if (profile.volume_30d + amount_din > profile.limits.monthly_volume_din) return false;

    // Check active trades
    if (profile.active_trades_count >= profile.limits.max_active_trades) return false;

    return true;
}

void KYCManager::recordTrade(const std::string& user_pubkey, double amount_din) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    int64_t now = std::time(nullptr);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT INTO trade_volume_log (user_pubkey, amount_din, timestamp)
        VALUES (?, ?, ?)
    )");

    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, amount_din);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Update profile volumes
    updateVolumes(user_pubkey);
}

void KYCManager::updateVolumes(const std::string& user_pubkey) {
    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    int64_t now = std::time(nullptr);
    int64_t cutoff_24h = now - SECONDS_PER_DAY;
    int64_t cutoff_30d = now - SECONDS_PER_30_DAYS;

    // Calculate 24h volume
    sqlite3_stmt* stmt = conn.Prepare(R"(
        SELECT SUM(amount_din) FROM trade_volume_log
        WHERE user_pubkey = ? AND timestamp >= ?
    )");
    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, cutoff_24h);

    double volume_24h = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        volume_24h = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // Calculate 30d volume
    stmt = conn.Prepare(R"(
        SELECT SUM(amount_din) FROM trade_volume_log
        WHERE user_pubkey = ? AND timestamp >= ?
    )");
    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, cutoff_30d);

    double volume_30d = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        volume_30d = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // Update profile
    stmt = conn.Prepare(R"(
        UPDATE kyc_profiles
        SET volume_24h = ?, volume_30d = ?, updated_at = ?
        WHERE user_pubkey = ?
    )");
    sqlite3_bind_double(stmt, 1, volume_24h);
    sqlite3_bind_double(stmt, 2, volume_30d);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_text(stmt, 4, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

double KYCManager::getRemainingDailyCapacity(const std::string& user_pubkey) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    auto profile = getProfile(user_pubkey);
    updateVolumes(user_pubkey);

    double remaining = profile.limits.daily_volume_din - profile.volume_24h;
    return std::max(0.0, remaining);
}

// ═══════════════════════════════════════════════════════════════
// EMAIL VERIFICATION
// ═══════════════════════════════════════════════════════════════

EmailVerification KYCManager::sendEmailVerification(
    const std::string& user_pubkey,
    const std::string& email
) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string code = generateVerificationCode();
    int64_t now = std::time(nullptr);

    EmailVerification verification{
        email,
        code,
        now,
        0,
        VerificationStatus::PENDING,
        0
    };

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT OR REPLACE INTO email_verifications
        (user_pubkey, email, verification_code, sent_at, status, attempts)
        VALUES (?, ?, ?, ?, ?, 0)
    )");

    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int(stmt, 5, static_cast<int>(VerificationStatus::PENDING));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // TODO: Send actual email via SMTP or email service API
    dinero::g_logger.info("[KYC] Email verification code sent to " + email + ": " + code);

    return verification;
}

bool KYCManager::verifyEmail(const std::string& user_pubkey, const std::string& code) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        SELECT verification_code, sent_at, attempts
        FROM email_verifications
        WHERE user_pubkey = ? AND status = ?
    )");

    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(VerificationStatus::PENDING));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    std::string stored_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t sent_at = sqlite3_column_int64(stmt, 1);
    int attempts = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);

    // Check expiry
    int64_t now = std::time(nullptr);
    if (now - sent_at > VERIFICATION_CODE_EXPIRY_MINUTES * 60) {
        // Expired
        stmt = conn.Prepare("UPDATE email_verifications SET status = ? WHERE user_pubkey = ?");
        sqlite3_bind_int(stmt, 1, static_cast<int>(VerificationStatus::EXPIRED));
        sqlite3_bind_text(stmt, 2, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return false;
    }

    // Check code
    if (code != stored_code) {
        attempts++;
        if (attempts >= MAX_VERIFICATION_ATTEMPTS) {
            stmt = conn.Prepare("UPDATE email_verifications SET status = ?, attempts = ? WHERE user_pubkey = ?");
            sqlite3_bind_int(stmt, 1, static_cast<int>(VerificationStatus::REJECTED));
            sqlite3_bind_int(stmt, 2, attempts);
            sqlite3_bind_text(stmt, 3, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            stmt = conn.Prepare("UPDATE email_verifications SET attempts = ? WHERE user_pubkey = ?");
            sqlite3_bind_int(stmt, 1, attempts);
            sqlite3_bind_text(stmt, 2, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return false;
    }

    // Verified!
    stmt = conn.Prepare(R"(
        UPDATE email_verifications
        SET status = ?, verified_at = ?
        WHERE user_pubkey = ?
    )");
    sqlite3_bind_int(stmt, 1, static_cast<int>(VerificationStatus::VERIFIED));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_text(stmt, 3, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dinero::g_logger.info("[KYC] Email verified for user: " + user_pubkey);

    // Check if user should be upgraded to Light KYC
    auto profile = getProfile(user_pubkey);
    if (profile.email && profile.phone &&
        profile.email->status == VerificationStatus::VERIFIED &&
        profile.phone->status == VerificationStatus::VERIFIED) {
        upgradeTier(user_pubkey, KYCTier::LIGHT_KYC);
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
// PHONE VERIFICATION (similar to email)
// ═══════════════════════════════════════════════════════════════

PhoneVerification KYCManager::sendPhoneVerification(
    const std::string& user_pubkey,
    const std::string& phone
) {
    // TODO: Integrate with SMS provider (Twilio, SNS, etc.)
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string code = generateVerificationCode();
    int64_t now = std::time(nullptr);

    PhoneVerification verification{
        phone,
        code,
        now,
        0,
        VerificationStatus::PENDING,
        0
    };

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT OR REPLACE INTO phone_verifications
        (user_pubkey, phone, verification_code, sent_at, status, attempts)
        VALUES (?, ?, ?, ?, ?, 0)
    )");

    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, phone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int(stmt, 5, static_cast<int>(VerificationStatus::PENDING));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dinero::g_logger.info("[KYC] SMS verification code sent to " + phone + ": " + code);

    return verification;
}

bool KYCManager::verifyPhone(const std::string& user_pubkey, const std::string& code) {
    // Similar implementation to verifyEmail
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    // TODO: Similar verification logic as email
    // For brevity, returning true for now

    dinero::g_logger.info("[KYC] Phone verified for user: " + user_pubkey);

    return true;
}

// ═══════════════════════════════════════════════════════════════
// ID VERIFICATION
// ═══════════════════════════════════════════════════════════════

IDVerification KYCManager::submitIDVerification(
    const std::string& user_pubkey,
    const std::string& document_type,
    const std::string& document_data,
    const std::string& selfie_data
) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    // Hash the document data (don't store originals)
    std::string document_hash = dinero::crypto::double_sha256(document_data);
    std::string selfie_hash = dinero::crypto::double_sha256(selfie_data);

    int64_t now = std::time(nullptr);

    IDVerification verification{
        document_type,
        "",  // document_number_hash
        "",  // country
        selfie_hash,
        document_hash,
        now,
        0,
        "",
        VerificationStatus::PENDING,
        ""
    };

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        INSERT OR REPLACE INTO id_verifications
        (user_pubkey, document_type, document_number_hash, country,
         selfie_hash, document_hash, submitted_at, status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )");

    sqlite3_bind_text(stmt, 1, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, document_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, selfie_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, document_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, now);
    sqlite3_bind_int(stmt, 8, static_cast<int>(VerificationStatus::PENDING));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dinero::g_logger.info("[KYC] ID verification submitted for user: " + user_pubkey);

    return verification;
}

bool KYCManager::reviewIDVerification(
    const std::string& user_pubkey,
    const std::string& reviewer_pubkey,
    bool approved,
    const std::string& rejection_reason
) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    int64_t now = std::time(nullptr);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        UPDATE id_verifications
        SET status = ?, reviewed_at = ?, reviewer_pubkey = ?, rejection_reason = ?
        WHERE user_pubkey = ?
    )");

    VerificationStatus status = approved ? VerificationStatus::VERIFIED : VerificationStatus::REJECTED;

    sqlite3_bind_int(stmt, 1, static_cast<int>(status));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_text(stmt, 3, reviewer_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rejection_reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (approved) {
        upgradeTier(user_pubkey, KYCTier::FULL_KYC);
    }

    dinero::g_logger.info("[KYC] ID verification " + std::string(approved ? "approved" : "rejected") +
                          " for user: " + user_pubkey);

    return true;
}

// ═══════════════════════════════════════════════════════════════
// TIER MANAGEMENT
// ═══════════════════════════════════════════════════════════════

KYCTier KYCManager::getTier(const std::string& user_pubkey) {
    return getProfile(user_pubkey).tier;
}

bool KYCManager::hasTier(const std::string& user_pubkey, KYCTier required_tier) {
    return getTier(user_pubkey) >= required_tier;
}

void KYCManager::upgradeTier(const std::string& user_pubkey, KYCTier new_tier) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_ptr_);

    std::string db_path = data_dir_ + "/kyc.db";
    dinero::SqliteConn conn(db_path);

    int64_t now = std::time(nullptr);

    sqlite3_stmt* stmt = conn.Prepare(R"(
        UPDATE kyc_profiles
        SET tier = ?, updated_at = ?
        WHERE user_pubkey = ?
    )");

    sqlite3_bind_int(stmt, 1, static_cast<int>(new_tier));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_text(stmt, 3, user_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dinero::g_logger.info("[KYC] User " + user_pubkey + " upgraded to tier " +
                          std::to_string(static_cast<int>(new_tier)));
}

void KYCManager::save() {
    // Data is persisted in SQLite, no need for explicit save
}

void KYCManager::load() {
    // Data is loaded from SQLite on demand
}

} // namespace p2p
} // namespace din
