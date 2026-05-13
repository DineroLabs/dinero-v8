#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <chrono>

namespace dinero::auth {

// A single token entry (hashed on disk)
struct TokenRecord {
    std::string label;          // "desktop", "cli@host", etc.
    std::string token_hash_hex; // SHA-256(token) hex
    std::string created_at;     // ISO-8601
    std::string last_used;      // ISO-8601
    std::optional<std::string> expires; // ISO-8601, nullopt = no expiry
    bool revoked{false};
};

class AuthStore {
public:
    // root: <datadir>/auth/auth.json
    explicit AuthStore(std::string datadir);

    bool load();                 // read from disk (creates file if missing)
    bool persist() const;        // write to disk atomically (caller must hold m_mu)
    std::string path() const;    // absolute path to auth.json

    // API
    std::string createToken(const std::string& label,
                            std::optional<std::chrono::system_clock::time_point> expiresAt,
                            std::string& out_token_plaintext); // returns plaintext token via out param

    bool revokeToken(const std::string& token_hash_hex);
    std::vector<TokenRecord> listTokens() const;

    // Validation + touch
    bool validateBearerAndTouch(const std::string& bearer_token);

private:
    static std::string nowIso();
    static std::string toIso(std::chrono::system_clock::time_point tp);
    static std::string sha256_hex(const std::string& data); // use project existing SHA-256
    static std::string rand_token_base58(size_t bytes=32);  // 256-bit random → base58

    std::string m_dir;
    std::string m_file;
    mutable std::mutex m_mu;
    std::vector<TokenRecord> m_tokens;
};

} // namespace dinero::auth
