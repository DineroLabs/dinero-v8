#include "daemon/websocket_server.h"
#include "util/json_parse.hpp"
#include "daemon/mempool.h"
#include "common/logger.h"
// #include "lightning/lightning_events.h"          // Phase 14 - DISABLED: Lightning is standalone
// #include "lightning/lightning_event_manager.h"   // Phase 14 - DISABLED: Lightning is standalone
#include "security/auth_scrubber.h"
#include "wallet/address.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include "common/sha256d.h"
#include "compat/net_compat.h"
#ifdef _WIN32
  #include <winsock2.h>  // select(), fd_set
#else
  #include <sys/select.h>
#endif
#include <cstdint>
#include <algorithm>
#include <cerrno>

// Safe big-endian reading helpers to prevent UBSan signed shift errors
static inline uint16_t read_be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8)
         |  static_cast<uint16_t>(p[1]);
}

static inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         |  static_cast<uint32_t>(p[3]);
}

static inline uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56)
         | (static_cast<uint64_t>(p[1]) << 48)
         | (static_cast<uint64_t>(p[2]) << 40)
         | (static_cast<uint64_t>(p[3]) << 32)
         | (static_cast<uint64_t>(p[4]) << 24)
         | (static_cast<uint64_t>(p[5]) << 16)
         | (static_cast<uint64_t>(p[6]) << 8)
         |  static_cast<uint64_t>(p[7]);
}

namespace dinero {

// =========================
// TokenBucket Implementation
// =========================

TokenBucket::TokenBucket(double rate, double burst)
    : tokens(burst), max_tokens(burst), refill_rate(rate),
      last_refill(std::chrono::steady_clock::now()) {
}

bool TokenBucket::consume(double tokens_needed) {
    refill();
    
    if (tokens >= tokens_needed) {
        tokens -= tokens_needed;
        return true;
    }
    return false;
}

void TokenBucket::refill() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_refill).count();
    
    if (elapsed > 0) {
        double new_tokens = elapsed * refill_rate;
        tokens = std::min(tokens + new_tokens, max_tokens);
        last_refill = now;
    }
}

// =========================
// CircuitBreaker Implementation
// =========================

CircuitBreaker::CircuitBreaker(int failure_threshold, int recovery_timeout)
    : failure_threshold(failure_threshold), recovery_timeout_seconds(recovery_timeout) {
}

bool CircuitBreaker::should_allow_request() {
    if (!is_open_.load()) {
        return true;
    }
    
    // Check if recovery timeout has passed
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_failure).count();
    
    if (elapsed >= recovery_timeout_seconds) {
        // Try to close the circuit breaker
        is_open_.store(false);
        failure_count.store(0);
        return true;
    }
    
    return false;
}

void CircuitBreaker::record_failure() {
    failure_count.fetch_add(1);
    last_failure = std::chrono::steady_clock::now();
    
    if (failure_count.load() >= failure_threshold) {
        is_open_.store(true);
    }
}

void CircuitBreaker::record_success() {
    if (failure_count.load() > 0) {
        failure_count.fetch_sub(1);
    }
}

// =========================
// WebSocketConnection Implementation
// =========================

WebSocketConnection::WebSocketConnection(int socket, const std::string& client_ip)
    : socket_fd(socket), client_ip(client_ip), state(WebSocketState::CONNECTING),
      last_activity(std::chrono::steady_clock::now()),
      rate_limiter(5.0, 10.0) { // 5 req/sec, burst 10
    
    // Set socket to non-blocking
    compat_set_nonblocking(socket_fd);
}

WebSocketConnection::~WebSocketConnection() {
    if (socket_fd >= 0) {
        COMPAT_CLOSE_SOCKET(socket_fd);
    }
}

bool WebSocketConnection::upgrade_from_http(const std::string& request) {
    // Prevent double upgrades - only allow if in CONNECTING state
    if (state != WebSocketState::CONNECTING) {
        dinero::g_logger.warning("WebSocket upgrade attempted on non-CONNECTING connection from " + client_ip);
        return false;
    }
    
    if (!parse_upgrade_request(request)) {
        return false;
    }
    
    // Extract WebSocket key
    std::string key;
    size_t key_pos = request.find("Sec-WebSocket-Key: ");
    if (key_pos != std::string::npos) {
        size_t key_start = key_pos + 19;
        size_t key_end = request.find("\r\n", key_start);
        if (key_end != std::string::npos) {
            key = request.substr(key_start, key_end - key_start);
        }
    }
    
    if (key.empty()) {
        return false;
    }
    
    // Create and send upgrade response
    std::string response = create_upgrade_response(key);
    if (send_raw(response)) {
        state = WebSocketState::OPEN;
        last_activity = std::chrono::steady_clock::now();
        dinero::g_logger.info("WebSocket upgrade successful for " + client_ip);
        return true;
    }
    
    return false;
}

bool WebSocketConnection::send_raw(const std::string& data) {
    ssize_t sent = ::send(socket_fd, data.c_str(), static_cast<int>(data.length()), 0);
    if (sent == static_cast<ssize_t>(data.length())) {
        last_activity = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

bool WebSocketConnection::send_message(const std::string& message) {
    return send_frame(WebSocketFrameType::TEXT, message);
}

bool WebSocketConnection::send_json(const Json::Value& json) {
    Json::FastWriter writer;
    std::string message = writer.write(json);
    return send_message(message);
}

void WebSocketConnection::close() {
    if (state != WebSocketState::CLOSED) {
        send_frame(WebSocketFrameType::CLOSE, "");
        state = WebSocketState::CLOSED;
        COMPAT_CLOSE_SOCKET(socket_fd);
        socket_fd = -1;
    }
}

bool WebSocketConnection::subscribe(SubscriptionChannel channel) {
    if (subscriptions.size() >= 8) { // Max 8 subscriptions per connection
        return false;
    }
    
    // Check if already subscribed
    for (const auto& sub : subscriptions) {
        if (sub == channel) {
            return true; // Already subscribed
        }
    }
    
    subscriptions.push_back(channel);
    return true;
}

bool WebSocketConnection::unsubscribe(SubscriptionChannel channel) {
    auto it = std::find(subscriptions.begin(), subscriptions.end(), channel);
    if (it != subscriptions.end()) {
        subscriptions.erase(it);
        return true;
    }
    return false;
}

bool WebSocketConnection::has_subscription(SubscriptionChannel channel) const {
    return std::find(subscriptions.begin(), subscriptions.end(), channel) != subscriptions.end();
}

bool WebSocketConnection::check_rate_limit() {
    return rate_limiter.consume(1.0);
}

void WebSocketConnection::update_rate_limit() {
    rate_limiter.refill();
}

// =========================
// WebSocket Frame Handling
// =========================

bool WebSocketConnection::send_frame(WebSocketFrameType type, const std::string& payload) {
    std::string frame = create_frame(type, payload);
    
    ssize_t sent = ::send(socket_fd, frame.c_str(), static_cast<int>(frame.length()), 0);
    if (sent == static_cast<ssize_t>(frame.length())) {
        last_activity = std::chrono::steady_clock::now();
        return true;
    }
    
    return false;
}

std::string WebSocketConnection::create_frame(WebSocketFrameType type, const std::string& payload) {
    std::string frame;
    
    // First byte: FIN (1) + RSV (000) + Opcode (4 bits)
    uint8_t first_byte = 0x80 | static_cast<uint8_t>(type);
    frame.push_back(first_byte);
    
    // Second byte: MASK (0) + Payload length (7 bits)
    if (payload.length() < 126) {
        frame.push_back(static_cast<uint8_t>(payload.length()));
    } else if (payload.length() < 65536) {
        frame.push_back(126);
        frame.push_back((payload.length() >> 8) & 0xFF);
        frame.push_back(payload.length() & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back((payload.length() >> (i * 8)) & 0xFF);
        }
    }
    
    // Payload
    frame += payload;
    
    return frame;
}

bool WebSocketConnection::read_frame(std::string& payload, WebSocketFrameType& type) {
    char buffer[4096];
    ssize_t received = ::recv(socket_fd, buffer, static_cast<int>(sizeof(buffer)), 0);
    
    if (received <= 0) {
        return false;
    }
    
    // Parse WebSocket frame (simplified)
    if (received < 2) {
        return false;
    }
    
    uint8_t first_byte = buffer[0];
    uint8_t opcode = first_byte & 0x0F;
    type = static_cast<WebSocketFrameType>(opcode);
    
    uint8_t second_byte = buffer[1];
    bool masked = (second_byte & 0x80) != 0;
    uint64_t payload_length = second_byte & 0x7F;
    
    size_t header_size = 2;
    if (payload_length == 126) {
        if (received < 4) return false;
        payload_length = read_be16(reinterpret_cast<const uint8_t*>(&buffer[2]));
        header_size = 4;
    } else if (payload_length == 127) {
        if (received < 10) return false;
        payload_length = read_be64(reinterpret_cast<const uint8_t*>(&buffer[2]));
        header_size = 10;
    }
    
    if (received < static_cast<ssize_t>(header_size + payload_length)) {
        return false;
    }
    
    payload = std::string(buffer + header_size, payload_length);
    
    // Handle different frame types
    switch (type) {
        case WebSocketFrameType::PING:
            send_frame(WebSocketFrameType::PONG, payload);
            break;
        case WebSocketFrameType::CLOSE:
            close();
            break;
        default:
            break;
    }
    
    last_activity = std::chrono::steady_clock::now();
    return true;
}

// =========================
// HTTP Upgrade Handling
// =========================

bool WebSocketConnection::parse_upgrade_request(const std::string& request) {
    // Check if this is a WebSocket upgrade request
    if (request.find("GET ") != 0) {
        return false;
    }
    
    // Check for WebSocket upgrade header (case insensitive)
    bool has_upgrade = false;
    size_t upgrade_pos = request.find("Upgrade:");
    if (upgrade_pos != std::string::npos) {
        std::string upgrade_value = request.substr(upgrade_pos);
        size_t end_line = upgrade_value.find("\r\n");
        if (end_line != std::string::npos) {
            upgrade_value = upgrade_value.substr(0, end_line);
        }
        // Convert to lowercase for comparison
        std::transform(upgrade_value.begin(), upgrade_value.end(), upgrade_value.begin(), ::tolower);
        if (upgrade_value.find("websocket") != std::string::npos) {
            has_upgrade = true;
        }
    }
    
    if (!has_upgrade) {
        return false;
    }
    
    // Check for Connection header (case insensitive)
    bool has_connection = false;
    size_t conn_pos = request.find("Connection:");
    if (conn_pos != std::string::npos) {
        std::string conn_value = request.substr(conn_pos);
        size_t end_line = conn_value.find("\r\n");
        if (end_line != std::string::npos) {
            conn_value = conn_value.substr(0, end_line);
        }
        // Convert to lowercase for comparison
        std::transform(conn_value.begin(), conn_value.end(), conn_value.begin(), ::tolower);
        if (conn_value.find("upgrade") != std::string::npos) {
            has_connection = true;
        }
    }
    
    if (!has_connection) {
        return false;
    }
    
    return true;
}

std::string WebSocketConnection::create_upgrade_response(const std::string& key) {
    std::string accept_key = calculate_accept_key(key);
    
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n";
    response << "Upgrade: websocket\r\n";
    response << "Connection: Upgrade\r\n";
    response << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    response << "X-Dinero-RPC-Engine: vNext-2024\r\n";
    response << "X-Dinero-Protocol-Version: 1.0\r\n";
    response << "Server: Dinero-Daemon/1.0\r\n";
    response << "\r\n";
    
    return response.str();
}

std::string WebSocketConnection::calculate_accept_key(const std::string& key) {
    std::string concatenated = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    
    // Use proper SHA1 hash for WebSocket key calculation
    std::vector<uint8_t> hash_data(concatenated.begin(), concatenated.end());
    
    // Calculate SHA1 hash
    std::vector<uint8_t> hash(20, 0); // SHA1 produces 20 bytes
    
    // Simple SHA1 implementation (for WebSocket compatibility)
    // This is a basic implementation - in production you'd use a proper crypto library
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;
    
    // Process the input data in chunks
    size_t data_len = hash_data.size();
    size_t padded_len = ((data_len + 8) / 64 + 1) * 64;
    
    std::vector<uint8_t> padded_data(padded_len, 0);
    std::copy(hash_data.begin(), hash_data.end(), padded_data.begin());
    padded_data[data_len] = 0x80; // Append 1 bit
    
    // Append length in bits (big-endian)
    uint64_t bit_len = data_len * 8;
    for (int i = 0; i < 8; ++i) {
        padded_data[padded_len - 8 + i] = (bit_len >> (56 - i * 8)) & 0xFF;
    }
    
    // Process each 512-bit chunk
    for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
        uint32_t w[80];
        
        // Prepare message schedule
        for (int i = 0; i < 16; ++i) {
            w[i] = (padded_data[chunk + i*4] << 24) |
                   (padded_data[chunk + i*4 + 1] << 16) |
                   (padded_data[chunk + i*4 + 2] << 8) |
                   padded_data[chunk + i*4 + 3];
        }
        
        for (int i = 16; i < 80; ++i) {
            w[i] = ((w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]) << 1) | 
                   ((w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]) >> 31);
        }
        
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        
        // Main loop
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = ((b << 30) | (b >> 2));
            b = a;
            a = temp;
        }
        
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    
    // Convert hash to bytes
    hash[0] = (h0 >> 24) & 0xFF; hash[1] = (h0 >> 16) & 0xFF; hash[2] = (h0 >> 8) & 0xFF; hash[3] = h0 & 0xFF;
    hash[4] = (h1 >> 24) & 0xFF; hash[5] = (h1 >> 16) & 0xFF; hash[6] = (h1 >> 8) & 0xFF; hash[7] = h1 & 0xFF;
    hash[8] = (h2 >> 24) & 0xFF; hash[9] = (h2 >> 16) & 0xFF; hash[10] = (h2 >> 8) & 0xFF; hash[11] = h2 & 0xFF;
    hash[12] = (h3 >> 24) & 0xFF; hash[13] = (h3 >> 16) & 0xFF; hash[14] = (h3 >> 8) & 0xFF; hash[15] = h3 & 0xFF;
    hash[16] = (h4 >> 24) & 0xFF; hash[17] = (h4 >> 16) & 0xFF; hash[18] = (h4 >> 8) & 0xFF; hash[19] = h4 & 0xFF;
    
    // Base64 encode the hash
    const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    
    std::string result;
    uint32_t val = 0;
    int valb = -6;
    
    for (unsigned char c : hash) {
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
    
    return result;
}

// =========================
// WebSocketServer Implementation
// =========================

WebSocketServer::WebSocketServer()
    : server_socket(-1), m_mining(nullptr) {
}

WebSocketServer::~WebSocketServer() {
    shutdown();
}

bool WebSocketServer::initialize(int port, const std::string& path, const std::string& bind_address) {
    ws_path = path;
    
    // Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        dinero::g_logger.error("Failed to create WebSocket server socket");
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
        dinero::g_logger.error("Failed to set SO_REUSEADDR on WebSocket server socket");
        return false;
    }
    
    // Bind socket (allow port 0 for ephemeral assignment)
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    
    // Parse bind address
    if (bind_address == "0.0.0.0") {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else if (bind_address == "127.0.0.1") {
        server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        // Try to parse as IP address
        if (inet_pton(AF_INET, bind_address.c_str(), &server_addr.sin_addr) != 1) {
            dinero::g_logger.error("Invalid bind address: " + bind_address);
            COMPAT_CLOSE_SOCKET(server_socket);
            return false;
        }
    }

    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        dinero::g_logger.warning("Failed to bind WebSocket server socket to port " + std::to_string(port) + ": " + std::string(strerror(errno)));
        COMPAT_CLOSE_SOCKET(server_socket);
        return false;
    }
    
    // If ephemeral port requested, discover actual port
    if (port == 0) {
        socklen_t addrlen = sizeof(server_addr);
        if (getsockname(server_socket, (struct sockaddr*)&server_addr, &addrlen) == 0) {
            listen_port_ = ntohs(server_addr.sin_port);
        } else {
            listen_port_ = 0;
        }
    } else {
        listen_port_ = port;
    }

    // Listen for connections
    if (listen(server_socket, 10) < 0) {
        dinero::g_logger.error("Failed to listen on WebSocket server socket port " + std::to_string(listen_port_) + ": " + std::string(strerror(errno)));
        COMPAT_CLOSE_SOCKET(server_socket);
        return false;
    }
    
    // Set socket to non-blocking
    compat_set_nonblocking(server_socket);
    
    dinero::g_logger.info("WebSocket server initialized on port " + std::to_string(listen_port_) + " at path " + path);
    dinero::g_logger.info("WebSocket server socket fd: " + std::to_string(server_socket));
    return true;
}

void WebSocketServer::shutdown() {
    running = false;
    
    // Close server socket to unblock accept() in run_server_loop
    if (server_socket >= 0) {
#ifdef _WIN32
        ::shutdown(server_socket, SD_BOTH);
#else
        ::shutdown(server_socket, SHUT_RDWR);
#endif
        COMPAT_CLOSE_SOCKET(server_socket);
        server_socket = -1;
    }
    
    // Close all connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto& [fd, conn] : connections) {
            conn->close();
        }
        connections.clear();
    }
    
    dinero::g_logger.info("WebSocket server shutdown complete");
}

void WebSocketServer::start() {
    if (running.load()) {
        return;
    }
    
    running = true;
    dinero::g_logger.info("WebSocket server started - entering accept loop");
    
    // Run the server loop directly in this thread (blocking)
    run_server_loop();
    
    dinero::g_logger.info("WebSocket server accept loop finished");
}

void WebSocketServer::set_rate_limit_config(const RateLimitConfig& config) {
    rate_limit_config = config;
}


void WebSocketServer::set_mining(Mining* mining) {
    m_mining = mining;
}

void WebSocketServer::set_mempool(std::shared_ptr<Mempool> mempool) {
    m_mempool = mempool;
}

// =========================
// Phase 14: Lightning Event Streaming
// =========================

// DISABLED: Lightning is standalone
// void WebSocketServer::set_lightning_event_manager(lightning::LightningEventManager* event_mgr) {
//     m_lightning_event_mgr = event_mgr;
//     g_logger.info("⚡ WebSocketServer: Lightning event manager connected (Phase 14)");
// }

// DISABLED: Lightning is standalone
// void WebSocketServer::broadcast_lightning_event(const lightning::LightningEvent& event) {
//     using namespace lightning;
//
//     // Convert event to JSON
//     Json::Value event_json = event.toJson();
//
//     // Determine which subscription channels should receive this event
//     // LIGHTNING_ALL receives all events
//     dispatch_to_subscribers(SubscriptionChannel::LIGHTNING_ALL, event_json);
//
//     // Route to specific channels based on event type
//     switch (event.event_type) {
//         case LightningEventType::WATCHTOWER_BREACH_DETECTED:
//         case LightningEventType::WATCHTOWER_JUSTICE_BROADCAST:
//         case LightningEventType::WATCHTOWER_JUSTICE_CONFIRMED:
//             dispatch_to_subscribers(SubscriptionChannel::LIGHTNING_WATCHTOWER, event_json);
//             break;
//
//         case LightningEventType::CHANNEL_OPENED:
//         case LightningEventType::CHANNEL_STATE_UPDATE:
//         case LightningEventType::CHANNEL_COOPERATIVE_CLOSE:
//         case LightningEventType::CHANNEL_FORCE_CLOSE:
//         case LightningEventType::CHANNEL_CLOSED:
//         case LightningEventType::CHANNEL_SWEEP_SCHEDULED:
//         case LightningEventType::CHANNEL_SWEEP_EXECUTED:
//             dispatch_to_subscribers(SubscriptionChannel::LIGHTNING_CHANNELS, event_json);
//             break;
//
//         case LightningEventType::PAYMENT_SENT:
//         case LightningEventType::PAYMENT_RECEIVED:
//         case LightningEventType::PAYMENT_FAILED:
//         case LightningEventType::PAYMENT_HTLC_FORWARDED:
//         case LightningEventType::HTLC_ADDED:
//         case LightningEventType::HTLC_SETTLED:
//         case LightningEventType::HTLC_FAILED:
//             dispatch_to_subscribers(SubscriptionChannel::LIGHTNING_PAYMENTS, event_json);
//             break;
//
//         case LightningEventType::ROUTE_DISCOVERY_SUCCESS:
//         case LightningEventType::ROUTE_DISCOVERY_FAILED:
//         case LightningEventType::ROUTE_CHANNEL_FAILURE:
//             dispatch_to_subscribers(SubscriptionChannel::LIGHTNING_ROUTING, event_json);
//             break;
//
//         default:
//             // Unknown event type, only send to LIGHTNING_ALL
//             break;
//     }
// }

// =========================
// Event Broadcasting
// =========================

void WebSocketServer::broadcast_new_block(int height, const std::string& hash, int64_t time, double difficulty) {
    Json::Value data;
    data["height"] = height;
    data["hash"] = hash;
    data["time"] = static_cast<int64_t>(time);
    data["difficulty"] = difficulty;
    
    dispatch_to_subscribers(SubscriptionChannel::NEW_HEADS, data);
}

void WebSocketServer::broadcast_mempool_tx(const std::string& txid, double fee, int size) {
    Json::Value data;
    data["txid"] = txid;
    data["fee"] = fee;
    data["size"] = size;
    
    dispatch_to_subscribers(SubscriptionChannel::MEMPOOL_TX, data);
}

void WebSocketServer::broadcast_mining_info(bool generating, int threads, double hashrate) {
    Json::Value data;
    data["generating"] = generating;
    data["threads"] = threads;
    data["hashrate"] = hashrate;
    
    dispatch_to_subscribers(SubscriptionChannel::MINING_INFO, data);
}

void WebSocketServer::broadcast_event(const std::string& event_json) {
    try {
        // Parse the JSON to extract channel and data
        auto parsed = ParseJsonStrict(event_json);
        if (parsed.ok) {
            const Json::Value& event = parsed.root;
            std::string channel = event["channel"].asString();
            
            if (channel == "newBlocks") {
                dispatch_to_subscribers(SubscriptionChannel::NEW_BLOCKS, event["result"]);
            } else if (channel == "mempoolTx") {
                dispatch_to_subscribers(SubscriptionChannel::MEMPOOL_TX, event["result"]);
            } else if (channel == "miningInfo") {
                dispatch_to_subscribers(SubscriptionChannel::MINING_INFO, event["result"]);
            } else {
                g_logger.warning("🔔 Unknown channel in broadcast_event: " + channel);
            }
        } else {
            g_logger.error("🔔 Failed to parse event JSON: " + event_json);
        }
    } catch (const std::exception& e) {
        g_logger.error("🔔 Error in broadcast_event: " + std::string(e.what()));
    }
}

// =========================
// Rate Limiting
// =========================

bool WebSocketServer::check_global_rate_limit() {
    return circuit_breaker.should_allow_request();
}

void WebSocketServer::update_circuit_breaker() {
    // Basic CPU and memory monitoring - full implementation would monitor actual system resources
    // For now, just record success
    circuit_breaker.record_success();
}

// =========================
// Monitoring
// =========================

Json::Value WebSocketServer::get_rate_limit_info() const {
    Json::Value info;
    info["enabled"] = true;
    
    Json::Value global;
    global["circuit_breaker_open"] = circuit_breaker.is_open();
    global["active_connections"] = get_active_connections();
    global["total_subscriptions"] = get_total_subscriptions();
    info["global"] = global;
    
    Json::Value limits;
    limits["http_requests_per_second"] = rate_limit_config.http_requests_per_second;
    limits["ws_subscriptions_per_second"] = rate_limit_config.ws_subscriptions_per_second;
    limits["ws_max_subscriptions"] = rate_limit_config.ws_max_subscriptions;
    limits["ws_max_connections"] = rate_limit_config.ws_max_connections;
    info["limits"] = limits;
    
    return info;
}

int WebSocketServer::get_total_subscriptions() const {
    std::lock_guard<std::mutex> lock(connections_mutex);
    int total = 0;
    for (const auto& [fd, conn] : connections) {
        // Count subscriptions for this connection
        // This is a simplified implementation
        total += 4; // Assume average of 4 subscriptions per connection
    }
    return total;
}

// =========================
// Server Loop
// =========================

void WebSocketServer::run_server_loop() {
    dinero::g_logger.info("WS: entering accept loop");
    fd_set read_fds;
    struct timeval timeout;
    
    while (running.load()) {
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        int max_fd = server_socket;
        
        // Add all client connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            for (const auto& [fd, conn] : connections) {
                FD_SET(fd, &read_fds);
                max_fd = std::max(max_fd, fd);
            }
        }
        
        // Set timeout for select
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (activity < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted system call, retry
            } else if (errno == EBADF && !running.load()) {
                // Normal during shutdown - socket was closed
                dinero::g_logger.info("WS: leaving accept loop (stop=" + std::to_string(running.load()) + ")");
                break;
            } else {
                dinero::g_logger.error("WS: select error errno=" + std::to_string(errno) + " (" + std::string(strerror(errno)) + ")");
            }
            continue;
        }
        
        if (activity == 0) {
            // Timeout - cleanup dead connections
            cleanup_dead_connections();
            continue;
        }
        
        // Check for new connections
        if (FD_ISSET(server_socket, &read_fds)) {
            handle_new_connection();
        }
        
        // Handle existing connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            for (auto it = connections.begin(); it != connections.end();) {
                if (FD_ISSET(it->first, &read_fds)) {
                    dinero::g_logger.info("Handling data for connection " + std::to_string(it->first) + " from " + it->second->get_client_ip());
                    handle_connection_data(it->second.get());
                }
                ++it;
            }
        }
        
        // Update circuit breaker
        update_circuit_breaker();
    }
    
    dinero::g_logger.info("WS: leaving accept loop (stop=" + std::to_string((int)running.load()) + ")");
}

void WebSocketServer::handle_new_connection() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
    if (client_socket < 0) {
        return;
    }
    
    // Check connection limit
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        if (connections.size() >= static_cast<size_t>(rate_limit_config.ws_max_connections)) {
            COMPAT_CLOSE_SOCKET(client_socket);
            return;
        }
    }
    
    std::string client_ip = inet_ntoa(client_addr.sin_addr);
    
    // Create connection object
    auto conn = std::make_unique<WebSocketConnection>(client_socket, client_ip);
    
    // Add to connections map
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        connections[client_socket] = std::move(conn);
    }
    
    dinero::g_logger.info("New WebSocket connection from " + client_ip);
    
    // Immediately try to handle the HTTP upgrade
    // This is necessary because the client might have already sent the request
    auto& conn_ref = connections[client_socket];
    handle_connection_data(conn_ref.get());
}

void WebSocketServer::handle_connection_data(WebSocketConnection* conn) {
    if (!conn) return;
    
    // Handle based on connection state
    if (conn->get_state() == WebSocketState::CONNECTING) {
        // HTTP upgrade phase - read HTTP request and upgrade
        char buffer[4096];
        ssize_t received = ::recv(conn->get_socket(), buffer, static_cast<int>(sizeof(buffer)), 0);
        
        if (received <= 0) {
            conn->close();
            return;
        }
        
        std::string http_request(buffer, received);
        
        // Check if we have complete headers
        size_t header_end = http_request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            // Incomplete headers, wait for more data
            return;
        }
        
        // Extract complete headers
        std::string headers = http_request.substr(0, header_end + 4);
        
        // Debug: log the HTTP request (scrubbed for security)
        std::string safe_request = dinero::security::AuthScrubber::extractSafeRequestLine(headers);
        dinero::g_logger.info("HTTP request from " + conn->get_client_ip() + ": " + safe_request);
        
        // Attempt upgrade
        if (conn->upgrade_from_http(headers)) {
            // IMPORTANT: Return here to prevent fall-through and double-upgrade
            return;
        } else {
            dinero::g_logger.error("WebSocket upgrade failed for " + conn->get_client_ip());
            conn->close();
            return;
        }
    } else if (conn->get_state() == WebSocketState::OPEN) {
        // WebSocket frame phase - handle frames
        std::string payload;
        WebSocketFrameType type;
        
        if (conn->read_frame_public(payload, type)) {
        // Handle the frame
        if (type == WebSocketFrameType::TEXT) {
            // Parse JSON-RPC message
            auto parsed = ParseJsonStrict(payload, 64 * 1024); // 64KB limit for WS messages
            if (parsed.ok) {
                const Json::Value& message = parsed.root;
                // Handle subscription methods
                std::string method = message.isMember("method") ? "method" : "";
                
                if (method == "subscribe") {
                    Json::Value params = message.get("params", Json::Value(Json::arrayValue));
                    if (params.isArray() && params.size() > 0) {
                        std::string channel = params[0].asString();
                        
                        if (channel == "newHeads") {
                            conn->subscribe(SubscriptionChannel::NEW_HEADS);
                        } else if (channel == "mempoolTx") {
                            conn->subscribe(SubscriptionChannel::MEMPOOL_TX);
                        } else if (channel == "miningInfo") {
                            conn->subscribe(SubscriptionChannel::MINING_INFO);
                        }
                        
                        // Send subscription confirmation
                        Json::Value response;
                        response["jsonrpc"] = "2.0";
                        response["id"] = message.get("id", Json::Value::null);
                        response["result"]["subscription"] = "sub-" + std::to_string(conn->get_socket());
                        response["result"]["status"] = "subscribed";
                        
                        conn->send_json(response);
                    }
                } else if (method == "unsubscribe") {
                    // Handle unsubscribe
                    Json::Value response;
                    response["jsonrpc"] = "2.0";
                    response["id"] = message.get("id", Json::Value::null);
                    response["result"]["status"] = "unsubscribed";
                    
                    conn->send_json(response);
                }
            } else {
                // JSON parse error - close connection
                conn->close();
                return;
            }
        }
        } else {
            // Connection error or closed
            conn->close();
        }
    }
}

void WebSocketServer::cleanup_dead_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex);
    
    auto it = connections.begin();
    while (it != connections.end()) {
        if (it->second->get_state() == WebSocketState::CLOSED) {
            it = connections.erase(it);
        } else {
            // Check for idle connections
            auto now = std::chrono::steady_clock::now();
            auto last_activity = it->second->get_last_activity();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();
            
            if (elapsed > 300) { // 5 minutes idle
                it->second->close();
                it = connections.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void WebSocketServer::dispatch_to_subscribers(SubscriptionChannel channel, const Json::Value& data) {
    Json::Value notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "dinero_subscription";
    
    Json::Value params;
    params["subscription"] = "sub-1"; // Simplified subscription ID
    params["result"] = data;
    notification["params"] = params;
    
    std::lock_guard<std::mutex> lock(connections_mutex);
    
    for (auto& [fd, conn] : connections) {
        if (conn->has_subscription(channel)) {
            if (conn->check_rate_limit()) {
                conn->send_json(notification);
            }
            conn->update_rate_limit();
        }
    }
}

// =========================
// Utility Functions
// =========================

std::string WebSocketServer::base64_encode(const std::string& input) {
    // Simple base64 implementation
    const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    
    std::string result;
    uint32_t val = 0;  // Use uint32_t to prevent overflow
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
        // Safe left shift - ensure we don't overflow
        uint32_t shift_amount = static_cast<uint32_t>(valb + 8);
        if (shift_amount < 32) {
            result.push_back(base64_chars[((val << shift_amount) >> (valb + 8)) & 0x3F]);
        }
    }
    
    return result;
}

std::string WebSocketServer::sha1_hash(const std::string& input) {
    // Use our SHA256 implementation
    std::vector<uint8_t> hash_data(input.begin(), input.end());
    
    // Simple hash for WebSocket key (not cryptographically secure, just for compatibility)
    std::vector<uint8_t> hash(32, 0);
    for (size_t i = 0; i < hash_data.size() && i < 32; ++i) {
        hash[i] = hash_data[i] ^ (i * 7); // Simple XOR-based hash
    }
    
    std::ostringstream oss;
    for (size_t i = 0; i < 20; ++i) { // First 20 bytes for SHA1 compatibility
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return oss.str();
}

} // namespace dinero
