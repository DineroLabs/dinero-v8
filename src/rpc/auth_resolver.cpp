#include "auth_resolver.h"
#include "daemon/rpc_authcookie.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace dinero {

// Base64 encoding (simple implementation)
static std::string base64_encode(const std::string& input) {
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string result;
    int val = 0, valb = -6;

    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }

    if (valb > -6) {
        result.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }

    while (result.size() % 4) {
        result.push_back('=');
    }

    return result;
}

// Trim whitespace
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

AuthResolver::AuthResolver(const std::string& datadir,
                           const std::string& host,
                           int port)
    : datadir_(datadir)
    , host_(host)
    , port_(port)
{
    // Set default error
    lastError_.type = AuthErrorType::UNKNOWN;
    lastError_.message = "No credentials found";
    lastError_.suggestion = "See documentation for authentication setup";
}

std::optional<RpcCredentials> AuthResolver::resolve() {
    // Try in order of precedence
    if (tryExplicitCredentials()) return credentials_;
    if (tryEnvironmentVariables()) return credentials_;
    if (tryConfigFile()) return credentials_;
    if (tryCookieAutoDiscovery()) return credentials_;

    // All methods failed
    lastError_.type = AuthErrorType::FILE_NOT_FOUND;
    lastError_.message = "No RPC credentials found";
    lastError_.suggestion =
        "Try one of:\n"
        "  1. Start dinerod (creates cookie automatically)\n"
        "  2. Set --rpc-user and --rpc-password\n"
        "  3. Set DIN_RPC_COOKIE environment variable\n"
        "  4. Create ~/.dinero/dinero.conf with rpcuser/rpcpassword";

    return std::nullopt;
}

void AuthResolver::setExplicitCookie(const std::string& path) {
    explicitCookieFile_ = path;
}

void AuthResolver::setExplicitUserPass(const std::string& user, const std::string& pass) {
    explicitUser_ = user;
    explicitPassword_ = pass;
}

void AuthResolver::setExplicitUrl(const std::string& url) {
    explicitUrl_ = url;
}

bool AuthResolver::tryExplicitCredentials() {
    // Try explicit user/pass first
    if (!explicitUser_.empty() && !explicitPassword_.empty()) {
        return tryUserPassword(explicitUser_, explicitPassword_, "explicit-flags");
    }

    // Try explicit cookie file
    if (!explicitCookieFile_.empty()) {
        return tryCookieFile(explicitCookieFile_, "flag:" + explicitCookieFile_);
    }

    return false;
}

bool AuthResolver::tryEnvironmentVariables() {
    // Try DIN_RPC_COOKIE
    const char* cookieEnv = std::getenv("DIN_RPC_COOKIE");
    if (cookieEnv && *cookieEnv) {
        if (tryCookieFile(cookieEnv, "env:DIN_RPC_COOKIE")) {
            return true;
        }
    }

    // Try DIN_RPC_USER and DIN_RPC_PASSWORD
    const char* userEnv = std::getenv("DIN_RPC_USER");
    const char* passEnv = std::getenv("DIN_RPC_PASSWORD");
    if (userEnv && passEnv && *userEnv && *passEnv) {
        return tryUserPassword(userEnv, passEnv, "env:DIN_RPC_USER/PASSWORD");
    }

    return false;
}

bool AuthResolver::tryConfigFile() {
    std::string configPath;

    // Try custom datadir first
    if (!datadir_.empty()) {
        configPath = datadir_ + "/dinero.conf";
    } else {
        configPath = getDefaultDataDir() + "/dinero.conf";
    }

    std::string content = readConfigFile(configPath);
    if (content.empty()) {
        attemptedSources_.push_back("config:" + configPath + " (not found)");
        return false;
    }

    // Parse config
    std::string rpcUser = getConfigValue(content, "rpcuser");
    std::string rpcPass = getConfigValue(content, "rpcpassword");
    std::string rpcCookie = getConfigValue(content, "rpccookiefile");

    // Try cookie file from config
    if (!rpcCookie.empty()) {
        if (tryCookieFile(rpcCookie, "config:" + configPath + " (rpccookiefile)")) {
            return true;
        }
    }

    // Try user/pass from config
    if (!rpcUser.empty() && !rpcPass.empty()) {
        return tryUserPassword(rpcUser, rpcPass, "config:" + configPath);
    }

    attemptedSources_.push_back("config:" + configPath + " (no credentials)");
    return false;
}

bool AuthResolver::tryCookieAutoDiscovery() {
    for (const auto& path : getCookiePaths()) {
        if (tryCookieFile(path, "auto:" + path)) {
            return true;
        }
    }
    return false;
}

bool AuthResolver::tryCookieFile(const std::string& path, const std::string& source) {
    // Check if file exists and is readable
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        attemptedSources_.push_back(source + " (not found)");
        return false;
    }

    // Warn if permissions are too open (not 600)
    #ifndef _WIN32
    if ((st.st_mode & 0077) != 0) {
        attemptedSources_.push_back(source + " (WARNING: insecure permissions)");
    }
    #endif

    // Read file
    std::ifstream file(path);
    if (!file.is_open()) {
        attemptedSources_.push_back(source + " (permission denied)");
        lastError_.type = AuthErrorType::FILE_PERMISSION;
        lastError_.message = "Cookie file unreadable: " + path;
        lastError_.suggestion = "Check permissions: chmod 600 " + path;
        return false;
    }

    std::string line;
    std::getline(file, line);
    file.close();

    line = trim(line);

    // Validate format: __cookie__:XXXXX
    if (!validateCookieFormat(line)) {
        attemptedSources_.push_back(source + " (invalid format)");
        lastError_.type = AuthErrorType::INVALID_FORMAT;
        lastError_.message = "Invalid cookie format in: " + path;
        lastError_.suggestion = "Cookie should be: __cookie__:random_string";
        return false;
    }

    // Split into user:pass
    size_t colonPos = line.find(':');
    if (colonPos == std::string::npos) {
        attemptedSources_.push_back(source + " (malformed)");
        return false;
    }

    std::string user = line.substr(0, colonPos);
    std::string pass = line.substr(colonPos + 1);

    return tryUserPassword(user, pass, source);
}

bool AuthResolver::tryUserPassword(const std::string& user, const std::string& pass,
                                   const std::string& source) {
    // Create credentials
    std::string url = explicitUrl_.empty()
        ? "http://" + host_ + ":" + std::to_string(port_)
        : explicitUrl_;

    std::string authString = user + ":" + pass;
    std::string authHeader = "Basic " + base64_encode(authString);

    RpcCredentials creds;
    creds.url = url;
    creds.authHeader = authHeader;
    creds.source = source;
    creds.username = user;
    creds.password = pass;

    // Test connection (optional - can be disabled for speed)
    // For now, accept credentials without testing
    // testConnection() can be called separately if needed

    credentials_ = creds;
    attemptedSources_.push_back(source + " ✓");
    return true;
}

std::vector<std::string> AuthResolver::getCookiePaths() const {
    std::vector<std::string> paths;
    auto add_if_missing = [&paths](const std::string& path) {
        if (!path.empty() &&
            std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    };

    // 1. Custom datadir
    if (!datadir_.empty()) {
        add_if_missing(GetAuthCookiePath(datadir_, ""));
        add_if_missing(datadir_ + "/mainnet/.cookie");
        add_if_missing(datadir_ + "/testnet/.cookie");
        add_if_missing(datadir_ + "/regtest/.cookie");
    }

    // 2. Platform default
    std::string defaultDir = getDefaultDataDir();
    add_if_missing(GetAuthCookiePath(defaultDir, ""));
    add_if_missing(defaultDir + "/mainnet/.cookie");
    add_if_missing(defaultDir + "/testnet/.cookie");
    add_if_missing(defaultDir + "/regtest/.cookie");

    // 3. Current directory (for development/testing)
    add_if_missing("./.dinero/.cookie");
    add_if_missing("./.cookie");

    return paths;
}

std::string AuthResolver::getDefaultDataDir() const {
#ifdef _WIN32
    char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "\\Dinero";
    }
    return ".";
#elif __APPLE__
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/Library/Application Support/Dinero";
    }
    return ".";
#else // Linux
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.dinero";
    }
    return ".";
#endif
}

std::string AuthResolver::readConfigFile(const std::string& path) const {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string AuthResolver::getConfigValue(const std::string& content, const std::string& key) const {
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);

        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Find key=value
        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        std::string lineKey = trim(line.substr(0, eqPos));
        if (lineKey == key) {
            return trim(line.substr(eqPos + 1));
        }
    }

    return "";
}

bool AuthResolver::validateCookieFormat(const std::string& cookie) const {
    // Should be: __cookie__:XXXXXXXX (at least 20 chars)
    if (cookie.length() < 20) return false;

    size_t colonPos = cookie.find(':');
    if (colonPos == std::string::npos) return false;

    // Must have content after colon
    if (colonPos == cookie.length() - 1) return false;

    return true;
}

bool AuthResolver::testConnection(const RpcCredentials& creds) {
    // Lightweight local validation only. Network probing is intentionally skipped
    // to keep credential discovery fast and deterministic for CLI startup.
    if (creds.url.empty() || creds.authHeader.empty()) {
        return false;
    }
    if (creds.authHeader.rfind("Basic ", 0) != 0) {
        return false;
    }
    if (creds.username.empty() || creds.password.empty()) {
        return false;
    }
    if (creds.url.rfind("http://", 0) != 0 && creds.url.rfind("https://", 0) != 0) {
        return false;
    }
    return true;
}

std::vector<std::string> AuthResolver::getAttemptedSources() const {
    return attemptedSources_;
}

AuthError AuthResolver::getLastError() const {
    return lastError_;
}

std::string AuthResolver::getErrorMessage() const {
    std::string msg;

    // Show what was tried
    if (!attemptedSources_.empty()) {
        msg += "Attempted authentication sources:\n";
        for (const auto& source : attemptedSources_) {
            msg += "  - " + source + "\n";
        }
        msg += "\n";
    }

    // Show error and suggestion
    msg += "Error: " + lastError_.message + "\n\n";
    msg += lastError_.suggestion;

    return msg;
}

} // namespace dinero
