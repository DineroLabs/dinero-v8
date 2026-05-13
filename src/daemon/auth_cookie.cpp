#include "daemon/auth_cookie.h"
#include "common/logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>

namespace dinero {

// ---- small helpers ----
static inline void trim(std::string& s) {
    auto ns = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
}

static std::string to_lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return std::tolower(c); });
    return v;
}

// Minimal Base64 (RFC 4648)
static std::string base64_encode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size()+2)/3)*4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = (unsigned((unsigned char)in[i]) << 16)
                   | (unsigned((unsigned char)in[i+1]) << 8)
                   | (unsigned((unsigned char)in[i+2]));
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6)  & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned((unsigned char)in[i]) << 16);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned((unsigned char)in[i]) << 16)
                   | (unsigned((unsigned char)in[i+1]) << 8);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6)  & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// ---- cookie loading ----
std::string load_cookie_token(const std::string& cookie_path) {
    std::ifstream f(cookie_path, std::ios::in | std::ios::binary);
    std::string cookie_data;
    if (f) { 
        std::ostringstream oss; 
        oss << f.rdbuf(); 
        cookie_data = oss.str(); 
        trim(cookie_data); 
    } else {
        g_logger.warning("Could not read cookie file: " + cookie_path);
        return "";
    }
    
    // Parse cookie: should be "__cookie__:token"
    size_t colon_pos = cookie_data.find(':');
    if (colon_pos != std::string::npos && cookie_data.substr(0, colon_pos) == "__cookie__") {
        return cookie_data.substr(colon_pos + 1);  // Return just the token part
    }
    
    // Fallback: if no colon found, assume the whole thing is the token
    return cookie_data;
}

std::string make_basic_value(const std::string& token) {
    std::string raw = "__cookie__:" + token;
    return "Basic " + base64_encode(raw);
}

// ---- header check (shared by RPC + WS) ----
bool check_basic_authorization(
    const std::unordered_map<std::string, std::string>& headers_lowercased,
    const std::string& cookie_path)
{
    auto it = headers_lowercased.find("authorization");
    if (it == headers_lowercased.end()) {
        g_logger.debug("No Authorization header found");
        return false;
    }

    std::string got = it->second;
    trim(got);

    static std::string cached_token;
    static std::string cached_basic;
    static std::string cached_cookie_path;
    static std::time_t last_load = 0;

    // Reload every 5 seconds to survive daemon restarts that rotate the cookie.
    // Also reload immediately when the caller switches to a different cookie
    // path so multiple local instances do not share the wrong cached token.
    std::time_t now = std::time(nullptr);
    if (now - last_load > 5 || cached_token.empty() || cached_cookie_path != cookie_path) {
        cached_token = load_cookie_token(cookie_path);
        if (cached_token.empty()) {
            g_logger.error("Failed to load cookie token from: " + cookie_path);
            return false;
        }
        cached_basic = make_basic_value(cached_token);  // "Basic <b64>"
        cached_cookie_path = cookie_path;
        last_load = now;
        g_logger.debug("Reloaded cookie token, Basic auth: " + cached_basic);
    }

    // Accept exact match; optionally allow case-insensitive "basic " prefix
    if (got == cached_basic) {
        g_logger.debug("Authorization header matches cached Basic auth");
        return true;
    }

    // tolerant compare (prefix case-insensitive, then compare payload)
    auto lower = to_lower(got);
    if (lower.rfind("basic ", 0) == 0) {
        bool match = got.size() == cached_basic.size() &&
                     got.compare(6, std::string::npos, cached_basic, 6, std::string::npos) == 0;
        if (match) {
            g_logger.debug("Authorization header matches cached Basic auth (case-insensitive)");
        } else {
            g_logger.debug("Authorization header does not match cached Basic auth");
        }
        return match;
    }
    
    g_logger.debug("Authorization header does not start with 'Basic '");
    return false;
}

} // namespace dinero
