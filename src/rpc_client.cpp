#include "rpc_client.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <limits>

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
#endif

// Base64 encoding for HTTP Basic Auth
static std::string base64_encode(const std::string& input) {
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string result;
    int val = 0;
    int valb = -6;

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

namespace dinero {
namespace rpc {

namespace {

bool parse_content_length(const std::string& headers, size_t& content_length) {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const std::string marker = "\r\ncontent-length:";
    size_t pos = lower.find(marker);
    if (pos == std::string::npos) {
        if (lower.rfind("content-length:", 0) == 0) {
            pos = 0;
        } else {
            return false;
        }
    } else {
        pos += 2;
    }

    size_t value_start = pos + std::strlen("content-length:");
    while (value_start < headers.size() &&
           (headers[value_start] == ' ' || headers[value_start] == '\t')) {
        ++value_start;
    }

    size_t value_end = headers.find("\r\n", value_start);
    if (value_end == std::string::npos) {
        value_end = headers.size();
    }

    if (value_end == value_start) {
        return false;
    }

    size_t parsed = 0;
    for (size_t i = value_start; i < value_end; ++i) {
        const unsigned char ch = static_cast<unsigned char>(headers[i]);
        if (!std::isdigit(ch)) {
            return false;
        }
        const size_t digit = static_cast<size_t>(ch - '0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }

    content_length = parsed;
    return true;
}

}  // namespace

// Constructor with cookie auth
RpcClient::RpcClient(const std::string& host, uint16_t port, const std::string& datadir)
    : host_(host), port_(port), datadir_(datadir), use_cookie_auth_(true) {

    status_message_ = "Not connected";
}

// Constructor with explicit credentials
RpcClient::RpcClient(const std::string& host, uint16_t port,
                     const std::string& username, const std::string& password)
    : host_(host), port_(port), username_(username), password_(password), use_cookie_auth_(false) {

    status_message_ = "Not connected";
}

bool RpcClient::connect() {
    // Load auth credentials
    if (use_cookie_auth_) {
        if (!load_cookie()) {
            set_error("Failed to load RPC cookie from " + datadir_, 0, true);
            return false;
        }
    }

    // Test connection with blockchain.getblockcount
    auto result = call("blockchain.getblockcount");
    if (result) {
        connected_ = true;
        status_message_ = "Connected to " + host_ + ":" + std::to_string(port_);
        debug_log("Connected successfully");
        return true;
    }

    connected_ = false;
    // Error message already set by call()
    return false;
}

bool RpcClient::load_cookie() {
    std::filesystem::path cookie_path = std::filesystem::path(datadir_) / ".cookie";

    std::ifstream file(cookie_path);
    if (!file.is_open()) {
        debug_log("Cookie file not found: " + cookie_path.string());
        return false;
    }

    std::string line;
    if (std::getline(file, line)) {
        // Cookie format: "username:password"
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            username_ = line.substr(0, colon_pos);
            password_ = line.substr(colon_pos + 1);

            // Trim whitespace
            password_.erase(password_.find_last_not_of(" \t\r\n") + 1);

            debug_log("Cookie loaded: user=" + username_);
            return true;
        }
    }

    debug_log("Invalid cookie format");
    return false;
}

std::string RpcClient::build_auth_header() const {
    std::string credentials = username_ + ":" + password_;
    std::string encoded = base64_encode(credentials);
    return "Authorization: Basic " + encoded + "\r\n";
}

std::string RpcClient::build_json_rpc_request(const std::string& method, const Json::Value& params) const {
    Json::Value request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = method;
    request["params"] = params;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // Compact output
    return Json::writeString(builder, request);
}

std::optional<Json::Value> RpcClient::send_http_request(const std::string& request_body) {
    // Create socket
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        set_error("Failed to create socket", 0, true);
        return std::nullopt;
    }

    // Set timeout. Winsock expects milliseconds as a DWORD for SO_*TIMEO;
    // POSIX expects timeval. Passing timeval to Winsock turns the intended
    // 30-second timeout into ~30ms, which breaks slower RPCs like generate.
#ifdef _WIN32
    const DWORD timeout_ms = static_cast<DWORD>(timeout_seconds_) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval timeout;
    timeout.tv_sec = timeout_seconds_;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    // Resolve host
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);

    if (inet_pton(AF_INET, host_.c_str(), &server_addr.sin_addr) <= 0) {
        // Try hostname resolution
        struct hostent* he = gethostbyname(host_.c_str());
        if (!he) {
            set_error("Failed to resolve host: " + host_, 0, true);
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            return std::nullopt;
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    // Connect
    if (::connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        set_error("Failed to connect to " + host_ + ":" + std::to_string(port_), 0, true);
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return std::nullopt;
    }

    // Build HTTP request
    std::ostringstream http_request;
    http_request << "POST / HTTP/1.1\r\n";
    http_request << "Host: " << host_ << ":" << port_ << "\r\n";
    http_request << "Content-Type: application/json\r\n";
    http_request << "Content-Length: " << request_body.length() << "\r\n";
    http_request << build_auth_header();
    http_request << "Connection: close\r\n";
    http_request << "\r\n";
    http_request << request_body;

    std::string request_str = http_request.str();
    debug_log("Sending request: " + request_body);

    // Send request
    if (send(sock, request_str.c_str(), request_str.length(), 0) < 0) {
        set_error("Failed to send request", 0, true);
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return std::nullopt;
    }

    // Receive response. Prefer the HTTP Content-Length over waiting for the
    // server to close the socket; long RPCs such as regtest mining can race
    // close/FIN delivery on Windows even after the server has sent the body.
    std::string response;
    char buffer[4096];
    int bytes_received;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;

        const size_t header_end = response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            size_t content_length = 0;
            if (parse_content_length(response.substr(0, header_end), content_length)) {
                const size_t body_start = header_end + 4;
                const size_t body_received =
                    response.size() > body_start ? response.size() - body_start : 0;
                if (body_received >= content_length) {
                    break;
                }
            }
        }
    }

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    if (response.empty()) {
        set_error("Empty response from server", 0, true);
        return std::nullopt;
    }

    debug_log("Received response (" + std::to_string(response.length()) + " bytes)");

    // Parse HTTP response
    size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        set_error("Invalid HTTP response", 0, true);
        return std::nullopt;
    }

    std::string body = response.substr(body_start + 4);

    // Parse JSON
    Json::Value json_response;
    Json::Reader reader;
    if (!reader.parse(body, json_response)) {
        set_error("Failed to parse JSON response: " + reader.getFormattedErrorMessages(), 0, true);
        return std::nullopt;
    }

    // Check for RPC error
    if (json_response.isMember("error") && !json_response["error"].isNull()) {
        Json::Value error_obj = json_response["error"];
        int code = error_obj.get("code", -1).asInt();
        std::string message = error_obj.get("message", "Unknown error").asString();
        set_error(message, code, false);
        return std::nullopt;
    }

    // Success
    last_error_.clear();
    last_error_code_ = 0;
    last_error_was_connection_ = false;

    return json_response;
}

void RpcClient::set_error(const std::string& message, int code, bool connection_error) {
    last_error_ = message;
    last_error_code_ = code;
    last_error_was_connection_ = connection_error;

    if (connection_error) {
        connected_ = false;
        status_message_ = "Connection error: " + message;
    }

    debug_log("Error: " + message + " (code: " + std::to_string(code) + ")");
}

void RpcClient::debug_log(const std::string& message) const {
    if (debug_) {
        std::cout << "[RpcClient] " << message << std::endl;
    }
}

// Public call methods

std::optional<Json::Value> RpcClient::call(const std::string& method) {
    return call(method, Json::Value(Json::arrayValue));
}

std::optional<Json::Value> RpcClient::call(const std::string& method, const Json::Value& params) {
    std::string request_body = build_json_rpc_request(method, params);
    return send_http_request(request_body);
}

std::optional<Json::Value> RpcClient::call(const std::string& method, const std::string& param) {
    Json::Value params(Json::arrayValue);
    params.append(param);
    return call(method, params);
}

std::optional<Json::Value> RpcClient::call(const std::string& method, int param) {
    Json::Value params(Json::arrayValue);
    params.append(param);
    return call(method, params);
}

// Convenience functions

int get_block_count(RpcClient& client) {
    auto result = client.call("blockchain.getblockcount");
    if (result && result.value().isMember("result")) {
        return result.value()["result"].asInt();
    }
    return -1;
}

std::optional<Json::Value> get_version(RpcClient& client) {
    auto result = client.call("rpc.version");
    if (result && result.value().isMember("result")) {
        return result.value()["result"];
    }
    return std::nullopt;
}

std::optional<Json::Value> check_database(RpcClient& client) {
    auto result = client.call("consensus.checkdb");
    if (result && result.value().isMember("result")) {
        return result.value()["result"];
    }
    return std::nullopt;
}

double get_balance(RpcClient& client) {
    auto result = client.call("getbalance");
    if (result && result.value().isMember("result")) {
        return result.value()["result"].asDouble();
    }
    return 0.0;
}

std::string get_new_address(RpcClient& client) {
    auto result = client.call("getnewaddress");
    if (result && result.value().isMember("result")) {
        return result.value()["result"].asString();
    }
    return "";
}

} // namespace rpc
} // namespace dinero
