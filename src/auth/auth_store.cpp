#include "auth/auth_store.h"
#include "crypto/dinero_crypto_minimal.h"  // Use project's existing SHA-256
#include "crypto/secure_random.h"  // Use OS CSPRNG (non-blocking)
#include <json/json.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>

using namespace dinero::auth;
namespace fs = std::filesystem;

// Minimal base58
static const char* B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
static std::string to_base58(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> num = bytes;
    std::string result;
    size_t zeros=0;
    while (zeros < num.size() && num[zeros] == 0) ++zeros;
    std::vector<uint8_t> b = num;
    size_t start = zeros;
    while (start < b.size()) {
        int carry = 0;
        for (size_t i=start;i<b.size();++i) {
            int val = int(b[i]) + carry*256;
            b[i] = uint8_t(val / 58);
            carry = val % 58;
        }
        result.push_back(B58[carry]);
        while (start < b.size() && b[start] == 0) ++start;
    }
    while (zeros--) result.push_back('1');
    std::reverse(result.begin(), result.end());
    return result;
}

// ---- AuthStore ----
AuthStore::AuthStore(std::string datadir) : m_dir(std::move(datadir)) {
    fs::path p(m_dir);
    p /= "auth";
    fs::create_directories(p);
    p /= "auth.json";
    m_file = p.string();
}

std::string AuthStore::path() const { return m_file; }

static bool write_atomic_json(const std::string& path, const Json::Value& root) {
    fs::path p(path), tmp = p; tmp += ".tmp";
    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    Json::StreamWriterBuilder w; w["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> writer(w.newStreamWriter());
    writer->write(root, &ofs);
    ofs.flush(); ofs.close();
    fs::rename(tmp, p);
#if defined(__APPLE__) || defined(__linux__)
    chmod(p.c_str(), 0600);
#endif
    return true;
}

bool AuthStore::load() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_tokens.clear();

    if (!fs::exists(m_file)) {
        Json::Value root(Json::objectValue);
        root["schema"] = "din.auth.v1";
        root["tokens"] = Json::arrayValue;
        return write_atomic_json(m_file, root);
    }

    std::ifstream ifs(m_file);
    if (!ifs) return false;
    Json::Value root; ifs >> root;
    if (!root.isObject()) return false;
    const auto& arr = root["tokens"];
    if (arr.isArray()) {
        for (const auto& t : arr) {
            TokenRecord r;
            r.label          = t.get("label","").asString();
            r.token_hash_hex = t.get("token_hash_hex","").asString();
            r.created_at     = t.get("created_at","").asString();
            r.last_used      = t.get("last_used","").asString();
            if (t.isMember("expires") && !t["expires"].isNull())
                r.expires = t["expires"].asString();
            r.revoked = t.get("revoked", false).asBool();
            if (!r.token_hash_hex.empty()) m_tokens.push_back(std::move(r));
        }
    }
    return true;
}

bool AuthStore::persist() const {
    // NOTE: Caller must hold m_mu lock
    Json::Value root(Json::objectValue);
    root["schema"] = "din.auth.v1";
    Json::Value arr(Json::arrayValue);
    for (const auto& r : m_tokens) {
        Json::Value t(Json::objectValue);
        t["label"]          = r.label;
        t["token_hash_hex"] = r.token_hash_hex;
        t["created_at"]     = r.created_at;
        t["last_used"]      = r.last_used;
        if (r.expires.has_value()) t["expires"] = *r.expires; else t["expires"] = Json::nullValue;
        t["revoked"]        = r.revoked;
        arr.append(std::move(t));
    }
    root["tokens"] = std::move(arr);
    return write_atomic_json(m_file, root);
}

// time utils
static std::string iso_utc(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{}; 
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
std::string AuthStore::nowIso() { return iso_utc(std::chrono::system_clock::now()); }
std::string AuthStore::toIso(std::chrono::system_clock::time_point tp) { return iso_utc(tp); }

// Use project's existing SHA-256
static std::string to_hex(const uint8_t* p, size_t n) {
    std::ostringstream oss; oss << std::hex << std::setfill('0');
    for (size_t i=0;i<n;++i) oss << std::setw(2) << (int)p[i];
    return oss.str();
}
std::string AuthStore::sha256_hex(const std::string& data) {
    uint8_t out[32];
    sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
    return to_hex(out, 32);
}

std::string AuthStore::rand_token_base58(size_t bytes) {
    auto raw = ::secure_random_bytes(bytes);  // Use global function explicitly
    return to_base58(raw);
}

std::string AuthStore::createToken(const std::string& label,
                                   std::optional<std::chrono::system_clock::time_point> expiresAt,
                                   std::string& out_token_plaintext) {
    std::lock_guard<std::mutex> lk(m_mu);
    out_token_plaintext = rand_token_base58(32); // 256-bit => base58
    const auto hash_hex = sha256_hex(out_token_plaintext);

    TokenRecord rec;
    rec.label = label;
    rec.token_hash_hex = hash_hex;
    rec.created_at = nowIso();
    rec.last_used = rec.created_at;
    if (expiresAt.has_value()) rec.expires = toIso(*expiresAt);
    rec.revoked = false;

    m_tokens.push_back(rec);
    persist();
    return hash_hex;
}

bool AuthStore::revokeToken(const std::string& token_hash_hex) {
    std::lock_guard<std::mutex> lk(m_mu);
    bool changed=false;
    for (auto& r : m_tokens) {
        if (r.token_hash_hex == token_hash_hex && !r.revoked) {
            r.revoked = true; changed=true;
        }
    }
    if (changed) persist();
    return changed;
}

std::vector<TokenRecord> AuthStore::listTokens() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_tokens;
}

bool AuthStore::validateBearerAndTouch(const std::string& bearer_token) {
    const auto hash_hex = sha256_hex(bearer_token);
    const auto now = nowIso();
    std::lock_guard<std::mutex> lk(m_mu);
    bool found=false, changed=false;
    for (auto& r : m_tokens) {
        if (r.token_hash_hex != hash_hex) continue;
        found = true;
        if (r.revoked) return false;
        if (r.expires.has_value() && now > *r.expires) return false;
        r.last_used = now; changed=true;
        break;
    }
    if (found && changed) persist();
    return found;
}
