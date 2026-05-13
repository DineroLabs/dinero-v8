#include "daemon/rpc_authcookie.h"
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <random>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <windows.h>
  #include <wincrypt.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
  #ifdef __linux__
    #include <sys/random.h>
  #endif
#endif

static std::string Hex(const std::vector<unsigned char>& v) {
    std::ostringstream oss;
    for (auto b : v) oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return oss.str();
}

// Replace with your project's secure RNG if available (e.g., OpenSSL CF_GeneratePrivKey)
// This version pulls from OS randomness when possible.
static bool GetStrongRandBytes(std::vector<unsigned char>& out) {
#ifdef _WIN32
    // BCryptGenRandom (no linking boilerplate shown)
    HCRYPTPROV prov;
    if (!CryptAcquireContext(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    BOOL ok = CryptGenRandom(prov, (DWORD)out.size(), out.data());
    CryptReleaseContext(prov, 0);
    return ok == TRUE;
#else
    // Try getrandom(2); fallback to /dev/urandom
    #ifdef __linux__
    ssize_t n = getrandom(out.data(), out.size(), 0);
    if (n == (ssize_t)out.size()) return true;
    #endif
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t r = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return r == out.size();
#endif
}

std::string GetAuthCookiePath(const std::string& datadir, const std::string& rpccookiefileOpt) {
    if (!rpccookiefileOpt.empty()) return rpccookiefileOpt;
#ifdef _WIN32
    return datadir + "\\.cookie";
#else
    return datadir + "/.cookie";
#endif
}

#ifndef _WIN32
static bool Set0600(const std::string& path) {
    return chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
}
#endif

bool WriteAuthCookie(const std::string& path, std::string& out_user, std::string& out_pass) {
    out_user = "__cookie__";

    std::vector<unsigned char> rnd(32);
    if (!GetStrongRandBytes(rnd)) return false;
    out_pass = Hex(rnd); // 64 hex chars

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) return false;

    f << out_user << ":" << out_pass << "\n";
    f.close();
#ifndef _WIN32
    if (!Set0600(path)) return false;
#endif
    return true;
}

bool ReadAuthCookie(const std::string& path, std::string& out_user, std::string& out_pass) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    out_user = line.substr(0, pos);
    out_pass = line.substr(pos + 1);
    // Remove trailing whitespace/newlines
    while (!out_pass.empty() && (out_pass.back() == '\n' || out_pass.back() == '\r' || out_pass.back() == ' ')) {
        out_pass.pop_back();
    }
    return true;
}

bool RemoveAuthCookie(const std::string& path) {
#ifdef _WIN32
    return _unlink(path.c_str()) == 0 || errno == ENOENT;
#else
    return unlink(path.c_str()) == 0 || errno == ENOENT;
#endif
}
