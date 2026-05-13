#include "auth_cookie.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>

namespace dinero {

// ---------- helpers ----------
static inline void trim(std::string& s) {
    auto ns = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
}

// minimal RFC4648 Base64 decode
static bool base64_decode(const std::string& in, std::string& out) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int rev[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; i < 64; i++) rev[(unsigned char)tbl[i]] = i;
        init = true;
    }
    
    out.clear();
    out.reserve((in.size() * 3) / 4);
    
    int val = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int v = rev[c];
        if (v < 0) return false;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            out.push_back((val >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return true;
}

// Load cookie as user:pass (Bitcoin-style file supports both forms)
static std::string load_expected_creds() {
    std::ifstream f("./data/.cookie", std::ios::in | std::ios::binary);
    std::string s; if (f) { s.assign(std::istreambuf_iterator<char>(f), {}); trim(s); }
    if (s.empty()) return {};
    if (s.find(':') != std::string::npos) return s;              // file already "user:pass"
    return std::string("__cookie__:") + s;                        // token-only -> "__cookie__:token"
}

std::pair<std::string,std::string> load_cookie_userpass() {
    std::string creds = load_expected_creds();
    auto p = creds.find(':');
    if (p != std::string::npos) {
        return { creds.substr(0,p), creds.substr(p+1) };
    }
    return { "__cookie__", creds };
}

std::string make_basic_value(const std::string& user, const std::string& pass) {
    // This is just for compatibility - we don't actually use it in the new auth
    static const char* tbl="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string in = user + ":" + pass;
    std::string out; out.reserve(((in.size()+2)/3)*4);
    size_t i=0;
    while (i+2<in.size()) {
        unsigned v=(unsigned((unsigned char)in[i])<<16)|(unsigned((unsigned char)in[i+1])<<8)|unsigned((unsigned char)in[i+2]);
        out.push_back(tbl[(v>>18)&0x3F]); out.push_back(tbl[(v>>12)&0x3F]); out.push_back(tbl[(v>>6)&0x3F]); out.push_back(tbl[v&0x3F]);
        i+=3;
    }
    if (i+1==in.size()) {
        unsigned v=(unsigned((unsigned char)in[i])<<16);
        out.push_back(tbl[(v>>18)&0x3F]); out.push_back(tbl[(v>>12)&0x3F]); out.push_back('='); out.push_back('=');
    } else if (i+2==in.size()) {
        unsigned v=(unsigned((unsigned char)in[i])<<16)|(unsigned((unsigned char)in[i+1])<<8);
        out.push_back(tbl[(v>>18)&0x3F]); out.push_back(tbl[(v>>12)&0x3F]); out.push_back(tbl[(v>>6)&0x3F]); out.push_back('=');
    }
    return "Basic " + out;
}

bool check_basic_authorization(const std::unordered_map<std::string, std::string>& headers_lower) {
    auto it = headers_lower.find("authorization");
    if (it == headers_lower.end()) return false;

    // Expect "Basic <b64>"
    std::string auth = it->second;
    trim(auth);
    if (auth.size() < 6 || !std::equal(auth.begin(), auth.begin()+5, "Basic",
            [](char a,char b){ return std::tolower((unsigned char)a)==std::tolower((unsigned char)b);} ) ||
        (auth.size() >= 6 && auth[5] != ' ')) {
        return false;
    }
    std::string b64 = auth.substr(6);
    trim(b64);

    std::string decoded;
    if (!base64_decode(b64, decoded)) return false;

    static std::string expected;
    static std::time_t last=0;
    std::time_t now = std::time(nullptr);
    if (now-last > 5 || expected.empty()) { 
        expected = load_expected_creds(); 
        last = now; 
    }

    // DEBUG (remove later):
    printf("AUTH expected: '%s' (%zu)\n", expected.c_str(), expected.size());
    printf("AUTH got:      '%s' (%zu)\n", decoded.c_str(), decoded.size());

    return decoded == expected;
}

} // namespace dinero