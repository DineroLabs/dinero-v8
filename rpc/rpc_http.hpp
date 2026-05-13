// rpc_http.hpp  (C++17)
#pragma once
#include <string>
#include <string_view>
#include <map>
#include <functional>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <set>
#include <ctime>

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

// If you prefer Qt JSON, swap this for QJsonDocument/QJsonObject wire-up.
// This header uses JsonCpp with nlohmann-style 
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <functional>
#include <utility>
#include <optional>
#include <sstream>

// Canonical JSON alias for the RPC layer
using json = nlohmann::json;

// WebSocket metrics
#include "daemon/websocket_metrics.hpp"
// WebSocket accept key calculation (self-contained, no external deps)
#include "daemon/ws_accept_no_ssl.hpp"
// #include "core/broadcast.hpp"  // Removed - using new ws_bus implementation
#include "daemon/ws_frame_handler.hpp"
#include "daemon/ws_connection.hpp"

namespace rpc {

inline std::string tolower_copy(std::string s){
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c){ return std::tolower(c); });
  return s;
}



// WebSocket accept key calculation
inline std::string calculate_websocket_accept_key(const std::string& key) {
  // Use our self-contained SHA1 + base64 implementation
  return ws::websocket_accept(key);
}

// WebSocket frame structure
struct WebSocketFrame {
  bool fin;
  uint8_t opcode;
  bool masked;
  uint64_t payload_length;
  std::string payload;
  
  WebSocketFrame() : fin(false), opcode(0), masked(false), payload_length(0) {}
};

// Parse WebSocket frame
inline bool parse_websocket_frame(const std::string& data, WebSocketFrame& frame) {
  if (data.size() < 2) return false;
  
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
  
  frame.fin = (bytes[0] & 0x80) != 0;
  frame.opcode = bytes[0] & 0x0F;
  frame.masked = (bytes[1] & 0x80) != 0;
  frame.payload_length = bytes[1] & 0x7F;
  
  size_t offset = 2;
  
  if (frame.payload_length == 126) {
    if (data.size() < offset + 2) return false;
    frame.payload_length = (bytes[offset] << 8) | bytes[offset + 1];
    offset += 2;
  } else if (frame.payload_length == 127) {
    if (data.size() < offset + 8) return false;
    frame.payload_length = 0;
    for (int i = 0; i < 8; i++) {
      frame.payload_length = (frame.payload_length << 8) | bytes[offset + i];
    }
    offset += 8;
  }
  
  if (frame.masked) {
    if (data.size() < offset + 4) return false;
    offset += 4; // Skip mask
  }
  
  if (data.size() < offset + frame.payload_length) return false;
  
  frame.payload = data.substr(offset, frame.payload_length);
  
  // Unmask if needed
  if (frame.masked) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data() + offset - 4);
    for (size_t i = 0; i < frame.payload.size(); i++) {
      frame.payload[i] ^= bytes[i % 4];
    }
  }
  
  return true;
}

// Create WebSocket frame (SERVER → CLIENT: no masking)
inline std::string create_websocket_frame(uint8_t opcode, const std::string& payload, bool fin = true) {
  std::string frame;
  
  // First byte: FIN + opcode
  frame.push_back(static_cast<unsigned char>((fin ? 0x80 : 0x00) | opcode));
  
  // Second byte: MASK=0 + payload length (servers don't mask)
  if (payload.size() < 126) {
    frame.push_back(static_cast<unsigned char>(payload.size())); // No mask, small payload
  } else if (payload.size() < 65536) {
    frame.push_back(static_cast<unsigned char>(126)); // No mask, 16-bit length
    frame.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xFF));
    frame.push_back(static_cast<unsigned char>(payload.size() & 0xFF));
  } else {
    frame.push_back(static_cast<unsigned char>(127)); // No mask, 64-bit length
    for (int i = 7; i >= 0; i--) {
      frame.push_back(static_cast<unsigned char>((payload.size() >> (i * 8)) & 0xFF));
    }
  }
  
  // Add payload directly (no masking for server → client)
  frame.append(payload);
  
  return frame;
}

inline bool write_all(int fd, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  while (len) {
    #ifndef _WIN32
      ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
    #else
      int n = ::send(fd, p, (int)len, 0);
    #endif
    if (n <= 0) return false;
    p += n; len -= (size_t)n;
  }
  return true;
}
inline bool write_all(int fd, const std::string& s){ return write_all(fd, s.data(), s.size()); }

inline std::string read_until(int fd, std::string_view delim, size_t max_bytes = 1<<20) {
  std::string out; out.reserve(1024);
  char c; size_t matched = 0, need = delim.size();
  while (out.size() < max_bytes) {
    #ifndef _WIN32
      ssize_t n = ::recv(fd, &c, 1, 0);
    #else
      int n = ::recv(fd, &c, 1, 0);
    #endif
    if (n <= 0) break;
    out.push_back(c);
    matched = (c == delim[matched]) ? matched+1 : (c == delim[0] ? 1 : 0);
    if (matched == need) break;
  }
  return out;
}

inline std::string read_exact(int fd, size_t len) {
  std::string out; out.resize(len);
  size_t got = 0;
  while (got < len) {
    #ifndef _WIN32
      ssize_t n = ::recv(fd, &out[got], len-got, 0);
    #else
      int n = ::recv(fd, &out[got], (int)(len-got), 0);
    #endif
    if (n <= 0) { out.resize(got); return out; }
    got += (size_t)n;
  }
  return out;
}

inline std::map<std::string,std::string> parse_headers(const std::string& head, std::string& reqline) {
  std::map<std::string,std::string> h;
  size_t pos = 0, e = head.find("\r\n");
  reqline = head.substr(0, e);
  pos = e + 2;
  while (pos < head.size()) {
    e = head.find("\r\n", pos);
    if (e == std::string::npos || e == pos) break;
    std::string line = head.substr(pos, e-pos);
    pos = e + 2;
    auto kpos = line.find(':'); if (kpos == std::string::npos) continue;
    std::string k = tolower_copy(line.substr(0, kpos));
    std::string v = line.substr(kpos+1);
    while (!v.empty() && (v[0]==' '||v[0]=='\t')) v.erase(v.begin());
    h[k] = v;
  }
  return h;
}

inline std::string b64_decode(std::string_view s) {
  static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int val=0, valb=-8; std::string out; out.reserve(s.size()*3/4);
  for (unsigned char c : std::string(s)) {
    if (c=='=') break;
    int d = (c<'+'||c>'z')? -1 : (int)tbl.find(c);
    if (d<0) continue;
    val = (val<<6) + d; valb += 6;
    if (valb>=0){ out.push_back(char((val>>valb)&0xFF)); valb-=8; }
  }
  return out;
}

inline bool send_http(int fd, int status, std::string_view msg,
                      std::string_view ctype, const std::string& body, 
                      const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) {
  std::string hdr = "HTTP/1.1 " + std::to_string(status) + " " + std::string(msg) + "\r\n"
                    "Content-Type: " + std::string(ctype) + "\r\n"
                    "Connection: close\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "X-Dinero-RPC-Engine: v2\r\n";
  
  // Add extra headers if provided
  for (const auto& [key, value] : extra_headers) {
    hdr += key + ": " + value + "\r\n";
  }
  
  hdr += "\r\n";
  return write_all(fd, hdr) && write_all(fd, body);
}
inline bool send_json_ok(int fd, const json& j) {
  return send_http(fd, 200, "OK", "application/json", j.dump());
}

inline bool send_json_ok_with_rate_limit(int fd, const json& j,
                                         double remaining_tokens = -1,
                                         int reset_ms = -1) {
  std::vector<std::pair<std::string,std::string>> extra_headers;
  if (remaining_tokens >= 0) extra_headers.emplace_back("X-RateLimit-Remaining", std::to_string(remaining_tokens));
  if (reset_ms >= 0)         extra_headers.emplace_back("X-RateLimit-Reset",     std::to_string(reset_ms));
  return send_http(fd, 200, "OK", "application/json", j.dump(), extra_headers);
}

inline bool send_json_err(int fd, int code, const std::string& msg, const json& id) {
  json err(json::object());
  err["jsonrpc"] = "2.0";
  err["id"] = id;
  json e(json::object());
  e["code"] = code;
  e["message"] = msg;
  err["error"] = e;
  return send_http(fd, 200, "OK", "application/json", err.dump());
}

inline bool send_plain(int fd, int status, std::string_view msg, std::string_view text){
  return send_http(fd, status, msg, "text/plain; charset=utf-8", std::string(text));
}

// ...
using RpcDispatch = std::function<json(const std::string& path,
                                       const std::string& method,
                                       const json& params,
                                       const json& id)>;

struct RpcContext {              // keep this **struct** everywhere to avoid mismatches
  std::string cookie_creds;      // "__cookie__:abc123..."
  RpcDispatch dispatch;
};

// Simple token bucket for rate limiting
struct TokenBucket {
    double tokens;
    double rate;   // tokens per second
    double burst;  // max tokens
    std::chrono::steady_clock::time_point last;

    TokenBucket(double r, double b)
        : tokens(b), rate(r), burst(b), last(std::chrono::steady_clock::now()) {}

    bool allow(double cost = 1.0) {
        using clk = std::chrono::steady_clock;
        auto now = clk::now();
        double delta = std::chrono::duration<double>(now - last).count();
        tokens = std::min(burst, tokens + delta * rate);
        last = now;
        if (tokens >= cost) { 
            tokens -= cost; 
            return true; 
        }
        return false;
    }
    
    double get_remaining() const { return tokens; }
    double get_capacity() const { return burst; }
    double get_rate() const { return rate; }
};

// Rate limiting configuration
struct RateLimitConfig {
    double rate = 25.0;           // tokens per second
    double burst = 50.0;          // max tokens
    bool local_bypass = true;     // bypass 127.0.0.1
    bool charge_per_item = false; // charge per batch item (default: per request)
    int max_batch_size = 100;     // max batch items
};

// Rate limiting manager
class RateLimitManager {
private:
    std::mutex mutex_;
    std::unordered_map<std::string, TokenBucket> buckets_; // key = remote IP
    RateLimitConfig config_;
    
public:
    RateLimitManager(const RateLimitConfig& config) : config_(config) {}
    
    bool check_rate_limit(const std::string& key, double cost, std::string& why) {
        std::lock_guard<std::mutex> lk(mutex_);
        std::cout << "[rl] check_rate_limit called: key=" << key << ", cost=" << cost << std::endl;
        
        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            std::cout << "[rl] Creating new bucket for key=" << key << std::endl;
            it = buckets_.emplace(key, TokenBucket{config_.rate, config_.burst}).first;
        }
        
        bool allowed = it->second.allow(cost);
        std::cout << "[rl] Bucket result: allowed=" << allowed << ", remaining=" << it->second.get_remaining() << std::endl;
        
        if (!allowed) {
            std::ostringstream oss;
            oss << "Rate limit exceeded for " << key
                << " (rate=" << config_.rate << "/s, burst=" << config_.burst 
                << ", cost=" << cost << ")";
            why = oss.str();
            std::cout << "[rl] Rate limit exceeded: " << why << std::endl;
            return false;
        }
        return true;
    }
    
    void update_config(const RateLimitConfig& new_config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = new_config;
        // Clear existing buckets so they get new config
        buckets_.clear();
    }
    
    const RateLimitConfig& get_config() const { return config_; }
    
    // Clean up old buckets (optional)
    void cleanup_old_buckets(int max_age_seconds = 300) {
        // For now, keep it simple - buckets persist
        // Could add timestamp tracking later if needed
    }
};

// Global rate limiting instance - declare as extern
extern RateLimitManager g_rate_limiter;

// WebSocket connection management
struct WebSocketConnection {
  int fd;
  std::map<std::string, std::string> subscriptions; // channel -> subscription_id
  time_t last_activity;
  std::string request_id;
  
  WebSocketConnection(int socket_fd) : fd(socket_fd), last_activity(time(nullptr)) {
    // Generate unique request ID for this connection
    static int counter = 0;
    request_id = "ws-" + std::to_string(++counter) + "-" + std::to_string(time(nullptr));
  }
  
  std::string add_subscription(const std::string& channel) {
    std::string sub_id = "sub-" + request_id + "-" + channel;
    subscriptions[channel] = sub_id;
    return sub_id;
  }
  
  void remove_subscription(const std::string& channel) {
    subscriptions.erase(channel);
  }
  
  bool has_subscription(const std::string& channel) const {
    return subscriptions.find(channel) != subscriptions.end();
  }
  
  std::string get_subscription_id(const std::string& channel) const {
    auto it = subscriptions.find(channel);
    return (it != subscriptions.end()) ? it->second : "";
  }
  
  void update_activity() {
    last_activity = time(nullptr);
  }
};

// Global WebSocket connections registry
extern std::map<int, WebSocketConnection> g_websocket_connections;
extern std::mutex g_websocket_mutex;
extern WebSocketMetrics g_websocket_metrics;

// WebSocket subscription channels
enum class WebSocketChannel {
  NEW_HEADS,
  MEMPOOL_TX,
  MINING_INFO,
  NEW_BLOCKS
};

inline std::string channel_to_string(WebSocketChannel channel) {
  switch (channel) {
    case WebSocketChannel::NEW_HEADS: return "newHeads";
    case WebSocketChannel::MEMPOOL_TX: return "mempoolTx";
    case WebSocketChannel::MINING_INFO: return "miningInfo";
    case WebSocketChannel::NEW_BLOCKS: return "newBlocks";
    default: return "unknown";
  }
}

// Simple broadcast to all WebSocket connections
inline void broadcast_to_websockets(const std::string& message) {
  std::cout << "[ws] 🔔 broadcast_to_websockets called with: " << message.substr(0, 100) << "..." << std::endl;
  
  // Broadcast bus disabled - using new ws_bus implementation
  // try {
  //   json msg = json::parse(message);
  //   if (msg.contains("params") && msg["params"].contains("channel")) {
  //     std::string channel = msg["params"]["channel"].get<std::string>();
  //     dinero::post_event(channel, message);
  //   }
  // } catch (...) {
  //   // Message parsing failed, ignore broadcast bus
  // }
  
  std::lock_guard<std::mutex> lock(g_websocket_mutex);
  
  // Track metrics based on message content
  try {
    json msg = json::parse(message);
    if (msg.contains("channel")) {
      std::string channel = msg["channel"];
      if (channel == "newBlocks") {
        g_websocket_metrics.out_newBlocks.fetch_add(1, std::memory_order_relaxed);
      } else if (channel == "miningInfo") {
        g_websocket_metrics.out_miningInfo.fetch_add(1, std::memory_order_relaxed);
      } else if (channel == "mempoolTx") {
        g_websocket_metrics.out_mempoolTx.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } catch (...) {
    // Message parsing failed, ignore metrics
  }
  
  std::cout << "[ws] 🔔 Found " << g_websocket_connections.size() << " WebSocket connections to broadcast to" << std::endl;
  
  for (auto& [fd, conn] : g_websocket_connections) {
    try {
      // Parse message to get channel for backpressure handling
      json msg;
      std::string channel;
      try {
        msg = json::parse(message);
        if (msg.contains("params") && msg["params"].contains("channel")) {
          channel = msg["params"]["channel"].get<std::string>();
        }
      } catch (...) {
        channel = "unknown";
      }
      
      // For now, use direct sending until we integrate the new WsConnection class
      // TODO: Integrate backpressure handling with WsConnection class
      if (!ws_send_text(fd, message)) {
        std::cout << "[ws] Failed to send event to fd=" << fd << std::endl;
      } else {
        std::cout << "[ws] 🔔 Successfully sent event to fd=" << fd << std::endl;
      }
    } catch (...) {
      // Connection failed, ignore
    }
  }
}

// Handle WebSocket connection after upgrade (ROBUST VERSION - FIXES EAGAIN BUG)
inline void handle_websocket_connection(int fd) {
  std::cout << "[ws] Starting ROBUST WebSocket frame handling for fd=" << fd << std::endl;
  
  // Increment connection count
  g_websocket_metrics.ws_connections_current.fetch_add(1, std::memory_order_relaxed);
  g_websocket_metrics.ws_connections_accepted_total.fetch_add(1, std::memory_order_relaxed);
  
  // Create WsConnection for this client
  dinero::WsConnection ws_conn(fd);
  
  while (true) {
    // Read WebSocket frame (FIXED: use proper frame reading, not HTTP line reading)
    WsFrame ws_frame;
    if (!ws_recv_frame(fd, ws_frame)) {
      std::cout << "[ws] Connection closed or error for fd=" << fd << std::endl;
      break;
    }
    
    // Convert to old format for compatibility
    WebSocketFrame frame;
    frame.opcode = ws_frame.opcode;
    frame.payload = std::string(ws_frame.payload.begin(), ws_frame.payload.end());
    frame.payload_length = ws_frame.payload.size();
    frame.fin = ws_frame.fin;
    
    std::cout << "[ws] Received frame: opcode=" << (int)frame.opcode 
              << ", payload_length=" << frame.payload_length << std::endl;
    
    // Handle different opcodes
    switch (frame.opcode) {
      case 0x01: { // Text frame
        // ✅ IGNORE empty text frames - not a protocol error
        if (frame.payload.empty()) {
          std::cout << "[ws] Ignoring empty text frame (not fatal)" << std::endl;
          break;
        }
        
        try {
          json msg = json::parse(frame.payload);
          std::cout << "[ws] Received JSON message: " << msg.dump() << std::endl;
          
                    // Handle subscription messages
          if (msg.contains("method") && msg["method"] == "subscribe") {
            if (msg.contains("params") && msg["params"].is_array()) {
              std::vector<std::string> accepted;
              for (const auto& param : msg["params"]) {
                std::string channel = param.get<std::string>();
                std::cout << "[ws] Subscribing fd=" << fd << " to channel: " << channel << std::endl;
                
                std::string sub_id;
                {
                  std::lock_guard<std::mutex> lock(g_websocket_mutex);
                  auto it = g_websocket_connections.find(fd);
                  if (it != g_websocket_connections.end()) {
                    sub_id = it->second.add_subscription(channel);
                  }
                }
                accepted.push_back(channel);
              }
              
              // Send subscription confirmation
              json response(json::object());
              response["jsonrpc"] = "2.0";
              response["id"] = msg.contains("id") ? msg["id"] : json(nullptr);
              response["result"] = json(json::object());
              response["result"]["accepted"] = accepted;
              response["result"]["status"] = "subscribed";
              // Send subscription ACK using robust frame sending
              json 
              // builder indentation = "";
              ws_send_text(fd, response.dump());
            }
          } else if (msg.contains("method") && msg["method"] == "unsubscribe") {
            if (msg.contains("params") && msg["params"].is_array()) {
              for (const auto& param : msg["params"]) {
                std::string channel = param.get<std::string>();
                std::cout << "[ws] Unsubscribing fd=" << fd << " from channel: " << channel << std::endl;
                
                std::string sub_id;
                {
                  std::lock_guard<std::mutex> lock(g_websocket_mutex);
                  auto it = g_websocket_connections.find(fd);
                  if (it != g_websocket_connections.end()) {
                    sub_id = it->second.get_subscription_id(channel);
                    it->second.remove_subscription(channel);
                  }
                }
                
                // Send unsubscription confirmation
                json response = {
                  {"jsonrpc", "2.0"},
                  {"id", msg.value("id", json(nullptr))},
                  {"result", {
                    {"subId", sub_id},
                    {"channel", channel},
                    {"status", "unsubscribed"}
                  }}
                };
                // Send unsubscribe ACK using robust frame sending
                json 
                // builder indentation = "";
                ws_send_text(fd, response.dump());
              }
            }
          }
        } catch (const std::exception& e) {
          // ✅ DO NOT CLOSE on JSON parse error - just log and continue
          std::cout << "[ws] JSON parse error (ignoring, not fatal): " << e.what() << std::endl;
        }
        // ✅ DO NOT break out of the loop here — keep listening
        break;
      }
      
      case 0x08: // Close frame
        std::cout << "[ws] Close frame received for fd=" << fd << std::endl;
        goto cleanup;
        
      case 0x09: // Ping frame
        std::cout << "[ws] Ping received for fd=" << fd << std::endl;
        // Send pong response using robust frame sending
        ws_send_pong(fd, reinterpret_cast<const uint8_t*>(frame.payload.data()), frame.payload.size());
        break;
    }
    
    // Update activity timestamp
    {
      std::lock_guard<std::mutex> lock(g_websocket_mutex);
      auto it = g_websocket_connections.find(fd);
      if (it != g_websocket_connections.end()) {
        it->second.update_activity();
      }
    }
  }
  
cleanup:
  // Clean up connection with graceful close
  std::cout << "[ws] Cleaning up WebSocket connection fd=" << fd << std::endl;
  
  // Send close frame to prevent client-side 1006 error
  ws_send_close(fd, 1000, "Server closing", 14);
  
  {
    std::lock_guard<std::mutex> lock(g_websocket_mutex);
    g_websocket_connections.erase(fd);
    g_websocket_metrics.ws_connections_current.fetch_sub(1, std::memory_order_relaxed);
    g_websocket_metrics.ws_connections_closed_total.fetch_add(1, std::memory_order_relaxed);
  }
  shutdown(fd, SHUT_WR);
  close(fd);
}

// Function to update rate limiting from daemon config
inline void update_rate_limits_from_config(double rate, double burst, bool local_bypass = true, bool charge_per_item = false) {
    std::cout << "[rl] Updating config: rate=" << rate << ", burst=" << burst 
              << ", local_bypass=" << local_bypass << ", charge_per_item=" << charge_per_item << std::endl;
    
    RateLimitConfig config;
    config.rate = rate;
    config.burst = burst;
    config.local_bypass = local_bypass;
    config.charge_per_item = charge_per_item;
    g_rate_limiter.update_config(config);
    
    std::cout << "[rl] Config updated. Current config: rate=" << g_rate_limiter.get_config().rate 
              << ", burst=" << g_rate_limiter.get_config().burst 
              << ", local_bypass=" << g_rate_limiter.get_config().local_bypass << std::endl;
}

// Rate limit error response
inline json make_rate_limit_error(const json& id, const std::string& reason) {
    json result(json::object());
    result["jsonrpc"] = "2.0";
    result["id"] = id;
    result["error"] = json(json::object());
    result["error"]["code"] = -32005;
    result["error"]["message"] = "Rate limit exceeded";
    result["error"]["data"] = json(json::object());
    result["error"]["data"]["reason"] = reason;
    return result;
}

// Helper function to get canonical remote IP (no port)
inline std::string CanonicalRemoteIP(int fd) {
    sockaddr_storage ss; 
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (sockaddr*)&ss, &sl) != 0) return "unknown";
    
    char ip[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((sockaddr_in*)&ss)->sin_addr, ip, sizeof(ip));
    } else if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((sockaddr_in6*)&ss)->sin6_addr, ip, sizeof(ip));
    }
    return std::string(ip);
}

inline void handle_rpc_connection(int fd, const RpcContext& ctx) {
  std::cout << "[rl] handle_rpc_connection called with fd=" << fd << std::endl;
  
  #ifndef _WIN32
    timeval tv{.tv_sec=10, .tv_usec=0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    // macOS: avoid SIGPIPE on write (MSG_NOSIGNAL is 0); ignoring SIGPIPE globally also fine.
  #endif

  // Use early returns instead of goto to avoid variable initialization issues
  std::string head = read_until(fd, "\r\n\r\n");
  if (head.empty()) { 
    send_plain(fd, 400, "Bad Request", "empty request"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }

  std::string reqline;
  auto headers = parse_headers(head, reqline);
  auto a = reqline.find(' '), b = reqline.find(' ', a+1);
  if (a==std::string::npos || b==std::string::npos) { 
    send_plain(fd, 400, "Bad Request", "malformed request line"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  
  std::string method = reqline.substr(0,a);
  std::string path   = reqline.substr(a+1, b-(a+1));
  
  // HTTP RPC only - WebSocket is handled by separate server
  if (method != "POST") { 
    send_plain(fd, 405, "Method Not Allowed", "POST required"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  
  // Regular HTTP RPC authentication
  auto ita = headers.find("authorization");
  if (ita==headers.end() || ita->second.rfind("Basic ",0)!=0) { 
    send_plain(fd, 401, "Unauthorized", "missing auth"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  if (b64_decode(std::string_view(ita->second).substr(6)) != ctx.cookie_creds) { 
    send_plain(fd, 401, "Unauthorized", "bad auth"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }

  auto itexp = headers.find("expect");
  if (itexp != headers.end() && tolower_copy(itexp->second) == "100-continue") {
    write_all(fd, "HTTP/1.1 100 Continue\r\n\r\n");
  }

  size_t cl = 0;
  auto itcl = headers.find("content-length");
  auto itte = headers.find("transfer-encoding");
  if (itte != headers.end() && tolower_copy(itte->second).find("chunked") != std::string::npos) { 
    send_plain(fd, 501, "Not Implemented", "chunked not supported"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  if (itcl == headers.end()) { 
    send_plain(fd, 411, "Length Required", "missing Content-Length"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  try { 
    cl = std::stoull(itcl->second); 
  } catch (...) { 
    send_plain(fd, 400, "Bad Request", "bad Content-Length"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  if (cl > (1u<<20)) { 
    send_plain(fd, 413, "Payload Too Large", "body too large"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }

  std::string body = read_exact(fd, cl);
  if (body.size() != cl) { 
    send_plain(fd, 400, "Bad Request", "short read"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }

  json req;
  bool success = true;
  try {
    req = json::parse(body);
  } catch (...) {
    success = false;
  }
  if (!success) { 
    send_plain(fd, 400, "Bad Request", "invalid JSON"); 
    shutdown(fd, SHUT_WR); close(fd); return;
  }

  // Extract client IP for rate limiting
  std::string rl_key = CanonicalRemoteIP(fd);
  std::string rl_reason;
  std::cout << "[rl] key=" << rl_key << std::endl;
  std::cout << "[rl] config: rate=" << g_rate_limiter.get_config().rate 
            << ", burst=" << g_rate_limiter.get_config().burst 
            << ", local_bypass=" << g_rate_limiter.get_config().local_bypass << std::endl;

  // Enforce rate limiting before dispatching to any JSON-RPC method
  bool rate_limit_allowed = g_rate_limiter.check_rate_limit(rl_key, /*weight=*/1.0, rl_reason);
  std::cout << "[rl] check result: allowed=" << rate_limit_allowed << ", reason=" << rl_reason << std::endl;
  
  if (!rate_limit_allowed) {
    std::cout << "[rl] RATE LIMIT EXCEEDED for key=" << rl_key << ", reason=" << rl_reason << std::endl;
    json null_id = nullptr;
    auto err = make_rate_limit_error(null_id, rl_reason);
    auto resp = send_http(fd, 429, "Too Many Requests", "application/json", err.dump());
    shutdown(fd, SHUT_WR); close(fd); return; // do NOT dispatch
  }

  // Helper function for consistent error responses
  auto make_error = [](const json& id, int code, const std::string& msg) -> json {
    json result(json::object());
    result["jsonrpc"] = "2.0";
    result["id"] = id.is_null() ? nlohmann::json(nullptr) : id;
    result["error"] = json(json::object());
    result["error"]["code"] = code;
    result["error"]["message"] = msg;
    return result;
  };

  // Helper function to validate parameter types
  auto valid_params = [](const json& v) -> bool {
    return v.is_null() || v.is_array() || v.is_object();
  };

  // Helper function to handle single RPC request with validation
  auto handle_single_request = [&](const json& req) -> json {
    if (!req.is_object()) {
      json null_id;
      null_id = nullptr;
      return make_error(null_id, -32600, "Invalid Request: expected object");
    }
    
    // Check jsonrpc version
    if (req.value<std::string>("jsonrpc", "") != "2.0") {
      json id = req.contains("id") ? req["id"] : json(nullptr);
      return make_error(id, -32600, "Invalid Request: jsonrpc must be '2.0'");
    }
    
    // Extract and validate request components
    json id = req.contains("id") ? req["id"] : json(nullptr);
    
    // Validate id type (must be string, number, or null)
    if (!id.is_null() && !id.is_string() && !id.is_number()) {
      json null_id;
      null_id = nullptr;
      return make_error(null_id, -32600, "Invalid Request: id must be string, number, or null");
    }
    
    if (!req.contains("method") || !req["method"].is_string()) {
      return make_error(id, -32600, "Invalid Request: method must be string");
    }
    
    std::string method = req["method"].get<std::string>();
    if (method.empty()) {
      return make_error(id, -32600, "Invalid Request: method cannot be empty");
    }
    
    // Validate params type (must be array, object, or null if present)
    json params = json::array();
    if (req.contains("params")) {
      const auto& p = req["params"];
      if (!valid_params(p)) {
        return make_error(id, -32602, "Invalid params: must be array, object, or null");
      }
      params = p;
    }
    
    try {
      // Execute the RPC method
      json result = ctx.dispatch(path, method, params, id);
      
      // Format response
      json response = {
        {"jsonrpc", "2.0"},
        {"id", id}
      };
      
      if (result.contains("error") && !result["error"].is_null()) {
        // This is an error response
        response["error"] = result["error"];
      } else {
        // This is a success response
        response["result"] = result;
      }
      
      return response;
      
    } catch (const std::exception& e) {
      // Handle all standard exceptions
      return make_error(id, -32603, std::string("Internal error: ") + e.what());
    } catch (...) {
      // Handle any other unexpected exceptions
      return make_error(id, -32603, "Unknown internal error");
    }
  };

  // Handle batch RPC requests (top-level array)
  if (req.is_array()) {
    // Standard JSON-RPC 2.0 batch request
    if (req.empty()) {
      // Empty batch - return error (most JSON-RPC 2.0 implementations do this)
      json error_resp = make_error(nlohmann::json(nullptr), -32600, "Invalid Request: empty batch");
      send_json_ok(fd, error_resp);
      shutdown(fd, SHUT_WR); close(fd); return;
    }
    
    if (req.size() > 100) {
      // Batch too large
      json error_resp = make_error(nlohmann::json(nullptr), -32600, "Invalid Request: batch too large (maximum 100 requests allowed)");
      send_json_ok(fd, error_resp);
      shutdown(fd, SHUT_WR); close(fd); return;
    }
    
    // Process batch requests (rate limiting already checked above)
    json batch_results = json::array();
    
    for (const auto& batch_req : req) {
      // Use the helper function for consistent validation and error handling
      batch_results.push_back(handle_single_request(batch_req));
    }
    
    // Send batch response
    send_json_ok(fd, batch_results);
    shutdown(fd, SHUT_WR); close(fd); return;
  }
  
  // Handle single JSON-RPC request (rate limiting already checked above)
  json result = handle_single_request(req);
  send_json_ok(fd, result);

  shutdown(fd, SHUT_WR);
  close(fd);
}

} // namespace rpc
