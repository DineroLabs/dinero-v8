#include "daemon/cookie_auth.h"
#include "common/config_manager.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include "crypto/dinero_crypto_minimal.h"

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <unistd.h>
#include <sys/stat.h>
#endif

#if defined(__APPLE__) || defined(__linux__)

#endif

using namespace std;
namespace fs = std::filesystem;
namespace dinero::rpc_auth {

fs::path CookiePath(const fs::path& datadir) { 
    return datadir / ".cookie"; 
}

static string Hex(const uint8_t* p, size_t n) {
    static const char* k = "0123456789abcdef";
    string s; 
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) { 
        s[2*i] = k[(p[i]>>4)&0xF]; 
        s[2*i+1] = k[p[i]&0xF]; 
    }
    return s;
}

bool ReadCookie(const fs::path& path, string& user, string& pass) {
    ifstream f(path, ios::in | ios::binary);
    if (!f) return false;
    
    string line; 
    getline(f, line);
    auto pos = line.find(':');
    if (pos == string::npos) return false;
    
    user = line.substr(0, pos);
    pass = line.substr(pos + 1);
    return !user.empty() && !pass.empty();
}

bool WriteCookie(const fs::path& path, string& user, string& pass) {
    // Generate 32-byte secret
    uint8_t buf[32];
    
#if defined(__APPLE__) || defined(__linux__)
    // Use OpenSSL RNG if available
    if (!CF_GeneratePrivKey(buf)) {
        // Fallback to std::random if OpenSSL fails
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = static_cast<uint8_t>(dis(gen));
        }
    }
#else
    // Fallback for other platforms
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = static_cast<uint8_t>(dis(gen));
    }
#endif

    user = "__cookie__";
    pass = Hex(buf, sizeof(buf));

    // Create directories if they don't exist
    fs::create_directories(path.parent_path());
    
    // Write to temporary file first
    fs::path tmp = path; 
    tmp += ".tmp";

    {
        ofstream f(tmp, ios::out | ios::binary | ios::trunc);
        if (!f) return false;
        f << user << ":" << pass << "\n";
        f.flush();
        f.close();
    }

#if defined(__APPLE__) || defined(__FreeBSD__)
    // Set file permissions to 0600 (user read/write only)
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
#endif

    // Atomic rename
    fs::rename(tmp, path);
    return true;
}

void DeleteCookie(const fs::path& path) {
    std::error_code ec; 
    fs::remove(path, ec);
    // Ignore errors on deletion
}

bool ShouldUseCookieAuth(const fs::path& datadir) {
    // Check if rpcauth is set in config
    // For now, assume cookie auth should be used
    // This can be enhanced to check actual config file
    return true;
}

} // namespace dinero::rpc_auth
