#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <cstddef>
#include "version.h"
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define DIN_LITTLE_ENDIAN 1
#elif defined(_WIN32)
    #define DIN_LITTLE_ENDIAN 1
#else
    #define DIN_LITTLE_ENDIAN 0
#endif

namespace din::p2p {

// ---- Network constants (configurable at runtime)
struct NetworkConfig {
    // magic MUST be initialized at startup by init_network_config()
    // (src/p2p/p2p_wire_protocol.cpp) which reads dinero::Params().magic
    // after SelectParams. Default 0 means a forgot-to-init NetworkConfig
    // will produce frames rejected as bad magic — the safe failure mode
    // versus silently sending mainnet frames on testnet.
    uint32_t magic = 0;
    std::string user_agent_prefix = DineroUserAgent();
    uint32_t protocol_version = 70016;
    uint64_t services = 1;  // NODE_NETWORK
    
    // DoS protection limits
    static constexpr size_t MAX_USER_AGENT_SIZE = 256;
    static constexpr size_t MAX_MESSAGE_SIZE = 8u << 20;  // 8 MiB
    static constexpr size_t MAX_VARSTR_SIZE = 4u << 20;   // 4 MiB
    static constexpr size_t MAX_VARINT_SIZE = 9;          // Max bytes for varint
};

// Global network config (should be set during daemon initialization)
extern NetworkConfig g_network_config;

// ---- Utility functions
inline uint32_t net_magic() { 
    return g_network_config.magic; 
}

inline std::array<char,12> cmd(const char* s) {
    std::array<char,12> a{}; 
    std::strncpy(a.data(), s, 12); 
    return a;
}

// ---- Message structures
struct MsgHeader {
    uint32_t magic{0};
    std::array<char,12> command{};
    uint32_t length{0};
    uint32_t checksum{0}; // first 4 bytes of double-SHA256(payload)
};

struct Version {
    int32_t  protocol{70016};
    uint64_t services{1}; // NODE_NETWORK
    int64_t  timestamp{0};
    uint64_t nonce{0};
    std::string user_agent{DineroUserAgent()}; // will be capped to 256 bytes
    int32_t  start_height{0};
    bool     relay{true};
};

// ---- Endianness conversion helpers
template<typename T>
inline T native_to_little(T value) {
#if DIN_LITTLE_ENDIAN
    return value;
#else
    T result;
    uint8_t* src = (uint8_t*)&value;
    uint8_t* dst = (uint8_t*)&result;
    for (size_t i = 0; i < sizeof(T); ++i) {
        dst[i] = src[sizeof(T) - 1 - i];
    }
    return result;
#endif
}

template<typename T>
inline T little_to_native(T value) {
#if DIN_LITTLE_ENDIAN
    return value;
#else
    T result;
    uint8_t* src = (uint8_t*)&value;
    uint8_t* dst = (uint8_t*)&result;
    for (size_t i = 0; i < sizeof(T); ++i) {
        dst[i] = src[sizeof(T) - 1 - i];
    }
    return result;
#endif
}

// ---- Little-endian writers (production-grade)
inline void put_u8 (std::vector<uint8_t>& b, uint8_t v) { 
    b.push_back(v); 
}

inline void put_u16(std::vector<uint8_t>& b, uint16_t v) { 
    v = native_to_little(v); 
    auto p = (uint8_t*)&v; 
    b.insert(b.end(), p, p+2); 
}

inline void put_u32(std::vector<uint8_t>& b, uint32_t v) { 
    v = native_to_little(v); 
    auto p = (uint8_t*)&v; 
    b.insert(b.end(), p, p+4); 
}

inline void put_u64(std::vector<uint8_t>& b, uint64_t v) { 
    v = native_to_little(v); 
    auto p = (uint8_t*)&v; 
    b.insert(b.end(), p, p+8); 
}

inline void put_i32(std::vector<uint8_t>& b, int32_t v) { 
    put_u32(b, (uint32_t)v); 
}

inline void put_i64(std::vector<uint8_t>& b, int64_t v) { 
    put_u64(b, (uint64_t)v); 
}

// ---- VarInt / VarStr writers with DoS protection
inline void put_varint(std::vector<uint8_t>& b, uint64_t v) {
    if (v < 0xFD) {
        put_u8(b, (uint8_t)v);
    } else if (v <= 0xFFFF) { 
        put_u8(b, 0xFD); 
        put_u16(b, (uint16_t)v); 
    } else if (v <= 0xFFFFFFFF) { 
        put_u8(b, 0xFE); 
        put_u32(b, (uint32_t)v); 
    } else { 
        put_u8(b, 0xFF); 
        put_u64(b, v); 
    }
}

inline void put_varstr(std::vector<uint8_t>& b, const std::string& s) {
    // DoS protection: cap string length
    const size_t capped = std::min<size_t>(s.size(), NetworkConfig::MAX_USER_AGENT_SIZE);
    put_varint(b, capped);
    b.insert(b.end(), s.begin(), s.begin() + capped);
}

// ---- Readers with comprehensive bounds checks
struct BytesReader {
    const uint8_t* p;
    const uint8_t* e;
    
    BytesReader(const uint8_t* data, size_t size) : p(data), e(data + size) {}
    BytesReader(const std::vector<uint8_t>& data) : p(data.data()), e(data.data() + data.size()) {}
    
    size_t left() const { return (size_t)(e - p); }
    
    void need(size_t n) { 
        if (left() < n) {
            throw std::runtime_error("P2P wire protocol: insufficient data for read (" + 
                                   std::to_string(n) + " bytes needed, " + 
                                   std::to_string(left()) + " available)");
        }
    }
    
    uint8_t get_u8() { 
        need(1); 
        return *p++; 
    }
    
    uint16_t get_u16() { 
        need(2); 
        uint16_t v; 
        std::memcpy(&v, p, 2); 
        p += 2; 
        return little_to_native(v); 
    }
    
    uint32_t get_u32() { 
        need(4); 
        uint32_t v; 
        std::memcpy(&v, p, 4); 
        p += 4; 
        return little_to_native(v); 
    }
    
    uint64_t get_u64() { 
        need(8); 
        uint64_t v; 
        std::memcpy(&v, p, 8); 
        p += 8; 
        return little_to_native(v); 
    }
    
    int32_t get_i32() { return (int32_t)get_u32(); }
    int64_t get_i64() { return (int64_t)get_u64(); }
    
    uint64_t get_varint() {
        uint8_t d = get_u8();
        if (d < 0xFD) return d;
        if (d == 0xFD) return get_u16();
        if (d == 0xFE) return get_u32();
        return get_u64();
    }
    
    std::string get_varstr() {
        uint64_t n = get_varint();
        
        // DoS protection: enforce maximum string size
        if (n > NetworkConfig::MAX_VARSTR_SIZE) {
            throw std::runtime_error("P2P wire protocol: varstr too large (" + 
                                   std::to_string(n) + " bytes, max " + 
                                   std::to_string(NetworkConfig::MAX_VARSTR_SIZE) + ")");
        }
        
        need((size_t)n);
        std::string s((const char*)p, (size_t)n); 
        p += (size_t)n; 
        return s;
    }
    
    std::vector<uint8_t> get_bytes(size_t n) {
        need(n);
        std::vector<uint8_t> result(p, p + n);
        p += n;
        return result;
    }
};

// ---- Version message serialization (production-grade)
inline std::vector<uint8_t> serialize_version(const Version& v) {
    std::vector<uint8_t> b; 
    b.reserve(120);  // Reasonable initial capacity
    
    put_i32(b, v.protocol);
    put_u64(b, v.services);
    put_i64(b, v.timestamp);
    
    // addr_recv (26 bytes) zeros (services + IPv6(16) + port(be))
    b.insert(b.end(), 26, 0);
    
    // addr_from (26 bytes) zeros  
    b.insert(b.end(), 26, 0);
    
    put_u64(b, v.nonce);
    
    // DoS protection: cap user agent before encoding
    std::string capped_user_agent = v.user_agent;
    if (capped_user_agent.size() > NetworkConfig::MAX_USER_AGENT_SIZE) {
        capped_user_agent = capped_user_agent.substr(0, NetworkConfig::MAX_USER_AGENT_SIZE);
    }
    put_varstr(b, capped_user_agent);
    
    put_i32(b, v.start_height);
    put_u8(b, v.relay ? 1 : 0);
    
    return b;
}

// ---- Version message parsing (production-grade)
inline Version parse_version(const std::vector<uint8_t>& payload) {
    BytesReader r{payload};
    Version v;
    
    v.protocol = r.get_i32();
    v.services = r.get_u64();
    v.timestamp = r.get_i64();
    
    // Skip addr_recv/addr_from (26 + 26 = 52 bytes)
    r.need(52); 
    r.p += 52;
    
    v.nonce = r.get_u64();
    v.user_agent = r.get_varstr();  // Already bounds-checked in get_varstr()
    v.start_height = r.get_i32();
    
    // Relay flag is optional (added in later protocol versions)
    if (r.left() > 0) {
        v.relay = (r.get_u8() != 0);
    }
    
    return v;
}

// ---- Message header helpers (centralized path for all send/recv)
inline std::array<uint8_t,24> serialize_header(const char* command, uint32_t length, uint32_t checksum) {
    MsgHeader h;
    h.magic = net_magic();
    h.command = cmd(command);
    h.length = length;
    h.checksum = checksum;

    std::array<uint8_t,24> out{};
    uint32_t magic_le = native_to_little(h.magic);
    uint32_t len_le   = native_to_little(h.length);
    uint32_t cks_le   = native_to_little(h.checksum);
    
    std::memcpy(out.data() + 0,  &magic_le, 4);
    std::memcpy(out.data() + 4,  h.command.data(), 12);
    std::memcpy(out.data() + 16, &len_le,   4);
    std::memcpy(out.data() + 20, &cks_le,   4);
    
    return out;
}

inline bool parse_header(const uint8_t* buf24, MsgHeader& h) {
    if (!buf24) return false;
    
    std::memcpy(&h.magic,   buf24 + 0, 4);
    std::memcpy(h.command.data(), buf24 + 4, 12);
    std::memcpy(&h.length,  buf24 + 16, 4);
    std::memcpy(&h.checksum, buf24 + 20, 4);
    
    // Convert from little-endian
    h.magic = little_to_native(h.magic);
    h.length = little_to_native(h.length);
    h.checksum = little_to_native(h.checksum);
    
    // Basic sanity checks: magic match, length cap for DoS protection
    if (h.magic != net_magic() || h.length > NetworkConfig::MAX_MESSAGE_SIZE) {
        return false;
    }
    
    return true;
}

// ---- Checksum calculation (requires SHA256d implementation)
inline uint32_t checksum4(const std::vector<uint8_t>& payload,
                          const std::function<std::array<uint8_t,32>(const uint8_t*,size_t)>& sha256d) {
    auto h = sha256d(payload.data(), payload.size());
    uint32_t c; 
    std::memcpy(&c, h.data(), 4);
    return c;  // Already in native byte order from SHA256
}

// ---- Complete message building (header + payload with checksum)
inline std::vector<uint8_t> build_message(const char* command,
                                          const std::vector<uint8_t>& payload,
                                          const std::function<std::array<uint8_t,32>(const uint8_t*,size_t)>& sha256d) {
    // DoS protection: enforce message size limit
    if (payload.size() > NetworkConfig::MAX_MESSAGE_SIZE) {
        throw std::runtime_error("P2P wire protocol: message payload too large (" + 
                               std::to_string(payload.size()) + " bytes, max " + 
                               std::to_string(NetworkConfig::MAX_MESSAGE_SIZE) + ")");
    }
    
    const uint32_t csum = checksum4(payload, sha256d);
    auto hdr = serialize_header(command, (uint32_t)payload.size(), csum);
    
    std::vector<uint8_t> out; 
    out.reserve(24 + payload.size());
    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), payload.begin(), payload.end());
    
    return out;
}

// ---- Network configuration helpers
inline void set_network_magic(uint32_t magic) {
    g_network_config.magic = magic;
}

inline void set_user_agent_prefix(const std::string& prefix) {
    g_network_config.user_agent_prefix = prefix;
}

inline void set_protocol_version(uint32_t version) {
    g_network_config.protocol_version = version;
}

// ---- Command string helpers (null-terminated, 12-byte padded)
inline std::string extract_command(const std::array<char,12>& cmd_array) {
    // Find null terminator or use full 12 bytes
    size_t len = 0;
    while (len < 12 && cmd_array[len] != '\0') {
        len++;
    }
    return std::string(cmd_array.data(), len);
}

// ---- Function declarations for implementation in .cpp file
void init_network_config(const std::string& network);
bool validate_message(const uint8_t* header_buf, const std::vector<uint8_t>& payload);
std::vector<uint8_t> create_version_message(int64_t timestamp, uint64_t nonce, 
                                           const std::string& user_agent, int32_t start_height, bool relay);
std::vector<uint8_t> create_verack_message();
std::vector<uint8_t> create_ping_message(uint64_t nonce);
std::vector<uint8_t> create_pong_message(uint64_t nonce);
uint64_t parse_ping_pong_nonce(const std::vector<uint8_t>& payload);

} // namespace din::p2p
