#include "rpc_http_core.hpp"
#include "rpc_cookie.hpp"
#include "auth_cookie.hpp"
#include <thread>
#include <atomic>
#include <iostream>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>

// POSIX / macOS
#ifndef _WIN32
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <errno.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0   // macOS: use SO_NOSIGPIPE; we also ignore SIGPIPE
  #endif
#else
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define close closesocket
  #define SHUT_WR SD_SEND
  #define MSG_NOSIGNAL 0
#endif

namespace dinero::rpc {

// Simple rate limiter for HTTP-only server
class SimpleRateLimiter {
public:
    SimpleRateLimiter(double rate, int burst) : rate_(rate), burst_(burst) {}
    
    bool check(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        
        auto& bucket = buckets_[key];
        
        // Refill tokens
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.last_refill).count();
        if (elapsed > 0) {
            double tokens_to_add = (elapsed / 1000.0) * rate_;
            bucket.tokens = std::min(static_cast<double>(burst_), bucket.tokens + tokens_to_add);
            bucket.last_refill = now;
        }
        
        // Check if we can consume a token
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }
        
        return false;
    }

private:
    struct TokenBucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point last_refill = std::chrono::steady_clock::now();
    };
    
    std::mutex mutex_;
    std::map<std::string, TokenBucket> buckets_;
    double rate_;
    int burst_;
};

struct HttpRpcServer {
    std::thread server_thread;
    std::atomic<bool> running{false};
    int server_socket = -1;
    RpcConfig config;
    RpcHandler handler;
    std::unique_ptr<SimpleRateLimiter> rate_limiter;
    std::string cookie_creds;
    
    void run();
    void handle_client(int client_socket);
    std::string get_client_ip(int socket);
};

// Utility functions
inline std::string tolower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c){ return std::tolower(c); });
    return s;
}



inline std::string base64_decode(const std::string& input) {
    // Simple base64 decode (for Basic auth)
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + pos;
        valb += 6;
        if (valb >= 0) {
            result.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

inline std::string base64_encode(const std::string& input) {
    // Simple base64 encode (for Basic auth comparison)
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (result.size() % 4) {
        result.push_back('=');
    }
    return result;
}

// ---- Auth helper functions ----
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

static inline std::string ltrim(std::string s) { 
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int c){return !std::isspace(c);})); 
    return s; 
}

static inline std::string rtrim(std::string s) { 
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int c){return !std::isspace(c);} ).base(), s.end()); 
    return s; 
}

static inline std::string trim(std::string s) { 
    return ltrim(rtrim(std::move(s))); 
}

static inline bool istarts_with(const std::string& s, const char* pfx) {
    size_t n = std::strlen(pfx); 
    if (s.size() < n) return false;
    for (size_t i=0;i<n;++i) 
        if (std::tolower(s[i]) != std::tolower(pfx[i])) return false;
    return true;
}

static inline void hex_dump(const char* label, const std::string& s) {
    std::ostringstream oss; 
    oss << label << " (len " << s.size() << "): ";
    for (unsigned char c: s) 
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)c << ' ';
    std::cout << oss.str() << std::endl;
}

static inline bool auth_debug() { 
    return std::getenv("DINERO_AUTH_DEBUG") != nullptr; 
}

struct CookieCreds { 
    std::string user, pass; 
};

static bool parse_cookie_pair(const std::string& cookie, CookieCreds& cc) {
    auto p = cookie.find(':'); 
    if (p == std::string::npos) return false;
    cc.user = cookie.substr(0, p);
    cc.pass = cookie.substr(p+1);
    return true;
}

// ---- CORE: check_basic_auth ----
static bool check_basic_auth(const std::map<std::string, std::string>& headers, const std::string& cookie_raw) {
    CookieCreds allowed{};
    if (!parse_cookie_pair(cookie_raw, allowed)) {
        if (auth_debug()) std::cout << "[AUTH] Invalid cookie format, expected user:pass\n";
        return false;
    }

    std::string auth;
    auto auth_it = headers.find("authorization");
    if (auth_it == headers.end()) {
        if (auth_debug()) std::cout << "[AUTH] No Authorization header\n";
        return false;
    }
    auth = auth_it->second;

    if (!istarts_with(auth, "Basic ")) {
        if (auth_debug()) std::cout << "[AUTH] Authorization not Basic: " << auth << "\n";
        return false;
    }

    std::string b64 = trim(auth.substr(6));
    std::string decoded = base64_decode(b64);
    
    // Strip CR/LF and NUL if any
    while (!decoded.empty() && (decoded.back()=='\r' || decoded.back()=='\n' || decoded.back()=='\0')) 
        decoded.pop_back();

    auto p = decoded.find(':');
    if (p == std::string::npos) {
        if (auth_debug()) { 
            std::cout << "[AUTH] Decoded has no colon\n"; 
            hex_dump("decoded", decoded); 
        }
        return false;
    }

    std::string u = decoded.substr(0, p);
    std::string pw = decoded.substr(p+1);

    if (auth_debug()) {
        std::cout << "[AUTH] Comparing creds...\n";
        hex_dump("  allowed.user", allowed.user);
        hex_dump("  allowed.pass", allowed.pass);
        hex_dump("  got.user    ", u);
        hex_dump("  got.pass    ", pw);
        std::cout << "  header(b64): " << b64 << "\n";
        std::cout << "  expect(b64): " << base64_decode(cookie_raw) << "\n";
    }

    return (u == allowed.user && pw == allowed.pass);
}

std::string HttpRpcServer::get_client_ip(int socket) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(socket, (struct sockaddr*)&addr, &addr_len) == 0) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        return std::string(ip_str);
    }
    return "unknown";
}

void HttpRpcServer::handle_client(int client_socket) {
    // Read HTTP request
    char buffer[4096];
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    buffer[bytes_read] = '\0';
    
    std::string request(buffer);
    std::cout << "=== RAW HTTP REQUEST ===" << std::endl;
    std::cout << "Bytes read: " << bytes_read << std::endl;
    std::cout << "Raw request:" << std::endl;
    std::cout << request << std::endl;
    std::cout << "=== END RAW REQUEST ===" << std::endl;
    
    // Parse HTTP request line
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;
    
    // Parse headers - ROBUST VERSION
    std::map<std::string, std::string> headers;
    std::string line;
    
    // Skip the first line (already parsed as method/path/version)
    std::getline(iss, line);
    
    while (std::getline(iss, line)) {
        // Stop at empty line (end of headers)
        if (line.empty() || line == "\r") break;
        
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);

            // trim spaces + CR/LF
            auto trim = [](std::string &s){
                while (!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='\r'||s.front()=='\n')) 
                    s.erase(s.begin());
                while (!s.empty() && (s.back()==' ' ||s.back()=='\t' ||s.back()=='\r' ||s.back()=='\n')) 
                    s.pop_back();
            };
            trim(key); trim(val);

            // normalize key to lowercase (so lookups are case-insensitive)
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            headers[key] = val; // e.g., headers["authorization"] = "Basic …"
            
            if (std::getenv("DINERO_AUTH_DEBUG")) {
                std::cout << "Parsed header: '" << key << "' = '" << val << "'" << std::endl;
            }
        }
    }
    
    if (std::getenv("DINERO_AUTH_DEBUG")) {
        std::cout << "Total headers parsed: " << headers.size() << std::endl;
        for (const auto& h : headers) {
            std::cout << "  '" << h.first << "' = '" << h.second << "'" << std::endl;
        }
    }
    
    // Check method
    if (method != "POST") {
        std::string response = "HTTP/1.1 405 Method Not Allowed\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: 18\r\n"
                              "\r\n"
                              "Method Not Allowed";
        send(client_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
        close(client_socket);
        return;
    }
    
    // Check authentication using shared auth module
    if (config.require_auth) {
        // Convert headers map to unordered_map for shared auth module
        std::unordered_map<std::string, std::string> headers_lowercased;
        for (const auto& pair : headers) {
            headers_lowercased[pair.first] = pair.second;
        }
        
        if (!check_basic_authorization(headers_lowercased)) {
            // Standards-compliant 401 so Qt plays nice
            std::string response = "HTTP/1.1 401 Unauthorized\r\n"
                                  "Content-Type: application/json\r\n"
                                  "WWW-Authenticate: Basic realm=\"Dinero RPC\"\r\n"
                                  "Content-Length: 12\r\n"
                                  "\r\n"
                                  "Unauthorized";
            send(client_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
            close(client_socket);
            return;
        }
    }
    
    // Rate limiting
    std::string client_ip = get_client_ip(client_socket);
    if (!rate_limiter->check(client_ip)) {
        std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: 60\r\n"
                              "\r\n"
                              "{\"error\":{\"code\":-32000,\"message\":\"Rate limit exceeded\"}}";
        send(client_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
        close(client_socket);
        return;
    }
    
    // Read body
    std::string body;
    auto content_length_it = headers.find("content-length");
    if (content_length_it != headers.end()) {
        int content_length = std::stoi(content_length_it->second);
        if (content_length > 0 && content_length < 1024 * 1024) { // 1MB limit
            body.resize(content_length);
            ssize_t body_read = recv(client_socket, &body[0], content_length, 0);
            body.resize(std::max(0, static_cast<int>(body_read)));
        }
    }
    
    // Parse JSON and call handler
    try {
        Json::CharReaderBuilder reader_builder;
        std::string err;
        std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
        Json::Value request_json;
        if (!reader->parse(body.c_str(), body.c_str() + body.size(), &request_json, &err)) {
            throw std::runtime_error("JSON parse error: " + err);
        }
        Json::Value response_json = handler(request_json);
        
        Json::StreamWriterBuilder writer_builder;
        writer_builder["indentation"] = "";
        std::string response_body = Json::writeString(writer_builder, response_json);
        std::string response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(response_body.length()) + "\r\n"
                              "\r\n" + response_body;
        
        send(client_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
        
    } catch (const std::exception& e) {
        Json::Value error_response;
        error_response["jsonrpc"] = "2.0";
        error_response["id"] = Json::Value::null;
        error_response["error"]["code"] = -32700;
        error_response["error"]["message"] = "Parse error";
        
        Json::StreamWriterBuilder writer_builder;
        writer_builder["indentation"] = "";
        std::string response_body = Json::writeString(writer_builder, error_response);
        std::string response = "HTTP/1.1 400 Bad Request\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(response_body.length()) + "\r\n"
                              "\r\n" + response_body;
        
        send(client_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
    }
    
    close(client_socket);
}

void HttpRpcServer::run() {
    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Failed to create server socket" << std::endl;
        return;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config.port);
    
    if (config.bind_ip == "0.0.0.0") {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, config.bind_ip.c_str(), &server_addr.sin_addr);
    }
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind to " << config.bind_ip << ":" << config.port << std::endl;
        close(server_socket);
        return;
    }
    
    // Listen
    if (listen(server_socket, 128) < 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        close(server_socket);
        return;
    }
    
    std::cout << "HTTP RPC server listening on " << config.bind_ip << ":" << config.port << std::endl;
    
    // Accept loop
    while (running.load()) {
        int client_socket = accept(server_socket, nullptr, nullptr);
        if (client_socket < 0) {
            if (running.load()) {
                std::cerr << "Accept failed" << std::endl;
            }
            continue;
        }
        
        // Handle client in separate thread
        std::thread([this, client_socket]() {
            handle_client(client_socket);
        }).detach();
    }
    
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
}

HttpRpcServer* start_http_rpc(const RpcConfig& cfg, RpcHandler handler) {
    auto* server = new HttpRpcServer();
    server->config = cfg;
    server->handler = handler;
    server->rate_limiter = std::make_unique<SimpleRateLimiter>(cfg.rate, cfg.burst);
    
    // Load cookie credentials
    try {
        if (!cfg.cookie_path.empty()) {
            std::cout << "Loading cookie from explicit path: " << cfg.cookie_path << std::endl;
            server->cookie_creds = load_cookie_creds(cfg.cookie_path);
        } else {
            std::cout << "Loading cookie from default path" << std::endl;
            server->cookie_creds = load_cookie_creds();
        }
        std::cout << "Loaded cookie credentials: " << server->cookie_creds.substr(0, server->cookie_creds.find(':') + 5) << "..." << std::endl;
        std::cout << "FULL COOKIE VALUE: '" << server->cookie_creds << "' (length: " << server->cookie_creds.length() << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to load cookie credentials: " << e.what() << std::endl;
        server->cookie_creds = "__cookie__:devcookie";
        std::cout << "Using fallback cookie: " << server->cookie_creds << std::endl;
    }
    
    server->running = true;
    server->server_thread = std::thread([server]() {
        server->run();
    });
    
    return server;
}

void stop_http_rpc(HttpRpcServer* server) {
    if (!server) return;
    
    server->running = false;
    
    // Close server socket to wake up accept()
    if (server->server_socket >= 0) {
        close(server->server_socket);
        server->server_socket = -1;
    }
    
    if (server->server_thread.joinable()) {
        server->server_thread.join();
    }
    
    delete server;
}

} // namespace dinero::rpc
