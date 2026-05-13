#pragma once

#include <string>
#include <optional>
#include <vector>

namespace dinero {

struct RpcCredentials {
    std::string url;           // http://127.0.0.1:20998
    std::string authHeader;    // "Basic <base64>"
    std::string source;        // "cookie:~/.dinero/.cookie"
    std::string username;      // "__cookie__" or actual user
    std::string password;      // cookie value or actual password
};

enum class AuthErrorType {
    CONNECTION_REFUSED,    // Daemon not running
    UNAUTHORIZED,          // Wrong credentials
    FILE_NOT_FOUND,       // Cookie file missing
    FILE_PERMISSION,      // Cookie file unreadable
    TIMEOUT,              // Connection timeout
    INVALID_FORMAT,       // Malformed cookie/config
    UNKNOWN
};

struct AuthError {
    AuthErrorType type;
    std::string message;
    std::string suggestion;
};

class AuthResolver {
public:
    /**
     * @param datadir Custom data directory (or empty for default)
     * @param host RPC host (default: 127.0.0.1)
     * @param port RPC port (default: 20998)
     */
    AuthResolver(const std::string& datadir = "",
                 const std::string& host = "127.0.0.1",
                 int port = 20998);

    /**
     * Try all credential sources in order:
     * 1. Explicit flags (rpccookiefile, rpcuser/rpcpassword)
     * 2. Environment variables (DIN_RPC_*)
     * 3. Config file (dinero.conf)
     * 4. Cookie auto-discovery (platform defaults)
     *
     * @return Credentials if found and working, nullopt otherwise
     */
    std::optional<RpcCredentials> resolve();

    /**
     * Set explicit credentials (from command-line flags)
     */
    void setExplicitCookie(const std::string& path);
    void setExplicitUserPass(const std::string& user, const std::string& pass);
    void setExplicitUrl(const std::string& url);

    /**
     * Get list of all paths/sources attempted (for error reporting)
     * Format: "cookie:~/.dinero/.cookie (not found)"
     */
    std::vector<std::string> getAttemptedSources() const;

    /**
     * Get last error with actionable suggestion
     */
    AuthError getLastError() const;

    /**
     * Get user-friendly error message for CLI
     */
    std::string getErrorMessage() const;

private:
    // Discovery methods
    bool tryExplicitCredentials();
    bool tryEnvironmentVariables();
    bool tryConfigFile();
    bool tryCookieAutoDiscovery();

    // Helper methods
    bool tryCookieFile(const std::string& path, const std::string& source);
    bool tryUserPassword(const std::string& user, const std::string& pass,
                        const std::string& source);
    std::vector<std::string> getCookiePaths() const;
    std::string getDefaultDataDir() const;
    std::string readConfigFile(const std::string& path) const;
    std::string getConfigValue(const std::string& content, const std::string& key) const;

    // Validation
    bool validateCookieFormat(const std::string& cookie) const;
    bool testConnection(const RpcCredentials& creds);

    // State
    std::string datadir_;
    std::string host_;
    int port_;
    std::string explicitCookieFile_;
    std::string explicitUser_;
    std::string explicitPassword_;
    std::string explicitUrl_;

    std::optional<RpcCredentials> credentials_;
    std::vector<std::string> attemptedSources_;
    AuthError lastError_;
};

} // namespace dinero
