#include "p2p/messages.h"
#include "consensus/chainparams.h"  // dinero::Params().magic — canonical
#include <cstring>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>
#include <arpa/inet.h>

namespace dinero {
namespace p2p {

// MessageHeader implementation
MessageHeader::MessageHeader() : magic(0), length(0), checksum(0) {
    command.fill(0);
}

MessageHeader::MessageHeader(uint32_t magic, const std::string& cmd, uint32_t len, uint32_t csum)
    : magic(magic), length(len), checksum(csum) {
    setCommand(cmd);
}

bool MessageHeader::isValid() const {
    // Bitcoin-style: only accept the active chain's magic. The old
    // "magic == MAGIC_MAINNET || MAGIC_TESTNET || MAGIC_REGTEST"
    // permissive check accepted cross-network frames, which is wrong —
    // a mainnet node should drop testnet frames at the wire. The
    // canonical magic for the active chain comes from chainparams,
    // set up by SelectParams() during daemon startup.
    return magic == ::dinero::Params().magic;
}

std::string MessageHeader::getCommand() const {
    size_t len = 0;
    while (len < command.size() && command[len] != 0) {
        len++;
    }
    return std::string(command.data(), len);
}

void MessageHeader::setCommand(const std::string& cmd) {
    command.fill(0);
    size_t copy_len = std::min(cmd.length(), command.size() - 1);
    std::memcpy(command.data(), cmd.c_str(), copy_len);
}

std::vector<uint8_t> MessageHeader::serialize() const {
    std::vector<uint8_t> data(MESSAGE_HEADER_SIZE);
    size_t offset = 0;
    
    // Magic (4 bytes, little endian)
    uint32_t magic_le = MessageSerializer::htole32(magic);
    std::memcpy(data.data() + offset, &magic_le, 4);
    offset += 4;
    
    // Command (12 bytes)
    std::memcpy(data.data() + offset, command.data(), 12);
    offset += 12;
    
    // Length (4 bytes, little endian)
    uint32_t length_le = MessageSerializer::htole32(length);
    std::memcpy(data.data() + offset, &length_le, 4);
    offset += 4;
    
    // Checksum (4 bytes, little endian)
    uint32_t checksum_le = MessageSerializer::htole32(checksum);
    std::memcpy(data.data() + offset, &checksum_le, 4);
    
    return data;
}

MessageHeader MessageHeader::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < MESSAGE_HEADER_SIZE) {
        throw std::runtime_error("Invalid message header size");
    }
    
    MessageHeader header;
    size_t offset = 0;
    
    // Magic
    std::memcpy(&header.magic, data.data() + offset, 4);
    header.magic = MessageSerializer::le32toh(header.magic);
    offset += 4;
    
    // Command
    std::memcpy(header.command.data(), data.data() + offset, 12);
    offset += 12;
    
    // Length
    std::memcpy(&header.length, data.data() + offset, 4);
    header.length = MessageSerializer::le32toh(header.length);
    offset += 4;
    
    // Checksum
    std::memcpy(&header.checksum, data.data() + offset, 4);
    header.checksum = MessageSerializer::le32toh(header.checksum);
    
    return header;
}

// NetworkAddress implementation
NetworkAddress::NetworkAddress() : services(0), port(0), timestamp(0) {
    ip.fill(0);
}

NetworkAddress::NetworkAddress(const std::string& ip_str, uint16_t port, uint64_t services)
    : services(services), port(port) {
    timestamp = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    ip.fill(0);
    
    // Parse IPv4 address and map to IPv6
    if (ip_str.find(':') == std::string::npos) {
        // IPv4
        struct in_addr addr4;
        if (inet_pton(AF_INET, ip_str.c_str(), &addr4) == 1) {
            // IPv4-mapped IPv6: ::ffff:x.x.x.x
            ip[10] = 0xff;
            ip[11] = 0xff;
            std::memcpy(&ip[12], &addr4, 4);
        }
    } else {
        // IPv6
        struct in6_addr addr6;
        if (inet_pton(AF_INET6, ip_str.c_str(), &addr6) == 1) {
            std::memcpy(ip.data(), &addr6, 16);
        }
    }
}

std::string NetworkAddress::getIPString() const {
    if (isIPv4()) {
        struct in_addr addr4;
        std::memcpy(&addr4, &ip[12], 4);
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr4, str, INET_ADDRSTRLEN);
        return std::string(str);
    } else {
        struct in6_addr addr6;
        std::memcpy(&addr6, ip.data(), 16);
        char str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &addr6, str, INET6_ADDRSTRLEN);
        return std::string(str);
    }
}

bool NetworkAddress::isIPv4() const {
    // Check for IPv4-mapped IPv6 (::ffff:x.x.x.x)
    for (int i = 0; i < 10; i++) {
        if (ip[i] != 0) return false;
    }
    return ip[10] == 0xff && ip[11] == 0xff;
}

bool NetworkAddress::isValid() const {
    return port > 0 && services != 0;
}

std::vector<uint8_t> NetworkAddress::serialize(bool include_timestamp) const {
    std::vector<uint8_t> data;
    
    if (include_timestamp) {
        uint32_t ts_le = MessageSerializer::htole32(timestamp);
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&ts_le), 
                   reinterpret_cast<uint8_t*>(&ts_le) + 4);
    }
    
    // Services (8 bytes)
    uint64_t services_le = MessageSerializer::htole64(services);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&services_le),
               reinterpret_cast<uint8_t*>(&services_le) + 8);
    
    // IP (16 bytes)
    data.insert(data.end(), ip.begin(), ip.end());
    
    // Port (2 bytes, big endian for network addresses)
    uint16_t port_be = htons(port);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&port_be),
               reinterpret_cast<uint8_t*>(&port_be) + 2);
    
    return data;
}

NetworkAddress NetworkAddress::deserialize(const std::vector<uint8_t>& data, size_t& offset, bool has_timestamp) {
    NetworkAddress addr;
    
    if (has_timestamp) {
        if (offset + 4 > data.size()) throw std::runtime_error("Invalid network address data");
        std::memcpy(&addr.timestamp, data.data() + offset, 4);
        addr.timestamp = MessageSerializer::le32toh(addr.timestamp);
        offset += 4;
    }
    
    if (offset + 26 > data.size()) throw std::runtime_error("Invalid network address data");
    
    // Services
    std::memcpy(&addr.services, data.data() + offset, 8);
    addr.services = MessageSerializer::le64toh(addr.services);
    offset += 8;
    
    // IP
    std::memcpy(addr.ip.data(), data.data() + offset, 16);
    offset += 16;
    
    // Port (big endian)
    uint16_t port_be;
    std::memcpy(&port_be, data.data() + offset, 2);
    addr.port = ntohs(port_be);
    offset += 2;
    
    return addr;
}

din::Json NetworkAddress::toJson() const {
    din::Json json;
    json["ip"] = getIPString();
    json["port"] = port;
    json["services"] = services;
    json["timestamp"] = timestamp;
    return json;
}

// VersionMessage implementation
VersionMessage::VersionMessage() 
    : version(PROTOCOL_VERSION), services(NODE_NETWORK), timestamp(0), nonce(0), start_height(0), relay(true) {
    timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    
    // Generate random nonce
    std::random_device rd;
    std::mt19937_64 gen(rd());
    nonce = gen();
}

VersionMessage::VersionMessage(const NetworkAddress& recv_addr, const NetworkAddress& from_addr, 
                              int32_t height, const std::string& agent)
    : version(PROTOCOL_VERSION), services(NODE_NETWORK), addr_recv(recv_addr), addr_from(from_addr),
      user_agent(agent), start_height(height), relay(true) {
    timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    
    std::random_device rd;
    std::mt19937_64 gen(rd());
    nonce = gen();
}

std::vector<uint8_t> VersionMessage::serialize() const {
    std::vector<uint8_t> data;
    
    // Version (4 bytes)
    uint32_t version_le = MessageSerializer::htole32(version);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&version_le),
               reinterpret_cast<uint8_t*>(&version_le) + 4);
    
    // Services (8 bytes)
    uint64_t services_le = MessageSerializer::htole64(services);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&services_le),
               reinterpret_cast<uint8_t*>(&services_le) + 8);
    
    // Timestamp (8 bytes)
    int64_t timestamp_le = MessageSerializer::htole64(timestamp);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&timestamp_le),
               reinterpret_cast<uint8_t*>(&timestamp_le) + 8);
    
    // Addresses (no timestamp in version message)
    auto recv_data = addr_recv.serialize(false);
    data.insert(data.end(), recv_data.begin(), recv_data.end());
    
    auto from_data = addr_from.serialize(false);
    data.insert(data.end(), from_data.begin(), from_data.end());
    
    // Nonce (8 bytes)
    uint64_t nonce_le = MessageSerializer::htole64(nonce);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&nonce_le),
               reinterpret_cast<uint8_t*>(&nonce_le) + 8);
    
    // User agent
    auto agent_data = MessageSerializer::encodeString(user_agent);
    data.insert(data.end(), agent_data.begin(), agent_data.end());
    
    // Start height (4 bytes)
    int32_t height_le = MessageSerializer::htole32(start_height);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&height_le),
               reinterpret_cast<uint8_t*>(&height_le) + 4);
    
    // Relay (1 byte)
    data.push_back(relay ? 1 : 0);
    
    return data;
}

VersionMessage VersionMessage::deserialize(const std::vector<uint8_t>& data) {
    VersionMessage msg;
    size_t offset = 0;
    
    if (data.size() < 85) throw std::runtime_error("Invalid version message size");
    
    // Version
    std::memcpy(&msg.version, data.data() + offset, 4);
    msg.version = MessageSerializer::le32toh(msg.version);
    offset += 4;
    
    // Services
    std::memcpy(&msg.services, data.data() + offset, 8);
    msg.services = MessageSerializer::le64toh(msg.services);
    offset += 8;
    
    // Timestamp
    std::memcpy(&msg.timestamp, data.data() + offset, 8);
    msg.timestamp = MessageSerializer::le64toh(msg.timestamp);
    offset += 8;
    
    // Addresses
    msg.addr_recv = NetworkAddress::deserialize(data, offset, false);
    msg.addr_from = NetworkAddress::deserialize(data, offset, false);
    
    // Nonce
    std::memcpy(&msg.nonce, data.data() + offset, 8);
    msg.nonce = MessageSerializer::le64toh(msg.nonce);
    offset += 8;
    
    // User agent
    msg.user_agent = MessageSerializer::decodeString(data, offset);
    
    // Start height
    std::memcpy(&msg.start_height, data.data() + offset, 4);
    msg.start_height = MessageSerializer::le32toh(msg.start_height);
    offset += 4;
    
    // Relay (optional)
    if (offset < data.size()) {
        msg.relay = data[offset] != 0;
    }
    
    return msg;
}

din::Json VersionMessage::toJson() const {
    din::Json json;
    json["version"] = version;
    json["services"] = services;
    json["timestamp"] = timestamp;
    json["addr_recv"] = addr_recv.toJson();
    json["addr_from"] = addr_from.toJson();
    json["nonce"] = nonce;
    json["user_agent"] = user_agent;
    json["start_height"] = start_height;
    json["relay"] = relay;
    return json;
}

// Simple message implementations
din::Json VerackMessage::toJson() const {
    din::Json json;
    json["type"] = "verack";
    return json;
}

std::vector<uint8_t> PingMessage::serialize() const {
    std::vector<uint8_t> data(8);
    uint64_t nonce_le = MessageSerializer::htole64(nonce);
    std::memcpy(data.data(), &nonce_le, 8);
    return data;
}

PingMessage PingMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 8) throw std::runtime_error("Invalid ping message size");
    PingMessage msg;
    std::memcpy(&msg.nonce, data.data(), 8);
    msg.nonce = MessageSerializer::le64toh(msg.nonce);
    return msg;
}

din::Json PingMessage::toJson() const {
    din::Json json;
    json["type"] = "ping";
    json["nonce"] = nonce;
    return json;
}

std::vector<uint8_t> PongMessage::serialize() const {
    std::vector<uint8_t> data(8);
    uint64_t nonce_le = MessageSerializer::htole64(nonce);
    std::memcpy(data.data(), &nonce_le, 8);
    return data;
}

PongMessage PongMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 8) throw std::runtime_error("Invalid pong message size");
    PongMessage msg;
    std::memcpy(&msg.nonce, data.data(), 8);
    msg.nonce = MessageSerializer::le64toh(msg.nonce);
    return msg;
}

din::Json PongMessage::toJson() const {
    din::Json json;
    json["type"] = "pong";
    json["nonce"] = nonce;
    return json;
}

din::Json SendHeadersMessage::toJson() const {
    din::Json json;
    json["type"] = "sendheaders";
    return json;
}

std::vector<uint8_t> SendCmpctMessage::serialize() const {
    std::vector<uint8_t> data(9);
    data[0] = announce ? 1 : 0;
    uint64_t version_le = MessageSerializer::htole64(version);
    std::memcpy(data.data() + 1, &version_le, 8);
    return data;
}

SendCmpctMessage SendCmpctMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 9) throw std::runtime_error("Invalid sendcmpct message size");
    SendCmpctMessage msg;
    msg.announce = data[0] != 0;
    std::memcpy(&msg.version, data.data() + 1, 8);
    msg.version = MessageSerializer::le64toh(msg.version);
    return msg;
}

din::Json SendCmpctMessage::toJson() const {
    din::Json json;
    json["type"] = "sendcmpct";
    json["announce"] = announce;
    json["version"] = version;
    return json;
}

std::vector<uint8_t> AddrMessage::serialize() const {
    std::vector<uint8_t> data;
    
    // Count
    auto count_data = MessageSerializer::encodeVarInt(addresses.size());
    data.insert(data.end(), count_data.begin(), count_data.end());
    
    // Addresses
    for (const auto& addr : addresses) {
        auto addr_data = addr.serialize(true); // Include timestamp
        data.insert(data.end(), addr_data.begin(), addr_data.end());
    }
    
    return data;
}

AddrMessage AddrMessage::deserialize(const std::vector<uint8_t>& data) {
    AddrMessage msg;
    size_t offset = 0;
    
    uint64_t count = MessageSerializer::decodeVarInt(data, offset);
    if (count > 1000) throw std::runtime_error("Too many addresses in addr message");
    
    msg.addresses.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        msg.addresses.push_back(NetworkAddress::deserialize(data, offset, true));
    }
    
    return msg;
}

din::Json AddrMessage::toJson() const {
    din::Json json;
    json["type"] = "addr";
    json["addresses"] = din::Json::array();
    for (const auto& addr : addresses) {
        json["addresses"].append(addr.toJson());
    }
    return json;
}

// MessageSerializer implementation
uint32_t MessageSerializer::calculateChecksum(const std::vector<uint8_t>& payload) {
    // Simplified checksum - in real implementation would use double SHA256
    uint32_t checksum = 0;
    for (uint8_t byte : payload) {
        checksum = (checksum << 1) ^ byte;
    }
    return checksum;
}

std::vector<uint8_t> MessageSerializer::encodeVarInt(uint64_t value) {
    std::vector<uint8_t> data;
    if (value < 0xfd) {
        data.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        data.push_back(0xfd);
        uint16_t val_le = htole16(static_cast<uint16_t>(value));
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&val_le),
                   reinterpret_cast<uint8_t*>(&val_le) + 2);
    } else if (value <= 0xffffffff) {
        data.push_back(0xfe);
        uint32_t val_le = htole32(static_cast<uint32_t>(value));
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&val_le),
                   reinterpret_cast<uint8_t*>(&val_le) + 4);
    } else {
        data.push_back(0xff);
        uint64_t val_le = htole64(value);
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&val_le),
                   reinterpret_cast<uint8_t*>(&val_le) + 8);
    }
    return data;
}

uint64_t MessageSerializer::decodeVarInt(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset >= data.size()) throw std::runtime_error("Invalid varint data");
    
    uint8_t first = data[offset++];
    if (first < 0xfd) {
        return first;
    } else if (first == 0xfd) {
        if (offset + 2 > data.size()) throw std::runtime_error("Invalid varint data");
        uint16_t value;
        std::memcpy(&value, data.data() + offset, 2);
        offset += 2;
        return le16toh(value);
    } else if (first == 0xfe) {
        if (offset + 4 > data.size()) throw std::runtime_error("Invalid varint data");
        uint32_t value;
        std::memcpy(&value, data.data() + offset, 4);
        offset += 4;
        return le32toh(value);
    } else {
        if (offset + 8 > data.size()) throw std::runtime_error("Invalid varint data");
        uint64_t value;
        std::memcpy(&value, data.data() + offset, 8);
        offset += 8;
        return le64toh(value);
    }
}

std::vector<uint8_t> MessageSerializer::encodeString(const std::string& str) {
    std::vector<uint8_t> data;
    auto len_data = encodeVarInt(str.length());
    data.insert(data.end(), len_data.begin(), len_data.end());
    data.insert(data.end(), str.begin(), str.end());
    return data;
}

std::string MessageSerializer::decodeString(const std::vector<uint8_t>& data, size_t& offset) {
    uint64_t length = decodeVarInt(data, offset);
    if (offset + length > data.size()) throw std::runtime_error("Invalid string data");
    
    std::string str(reinterpret_cast<const char*>(data.data() + offset), length);
    offset += length;
    return str;
}

// Endianness conversion (simplified - assumes little endian host)
uint16_t MessageSerializer::htole16(uint16_t value) { return value; }
uint32_t MessageSerializer::htole32(uint32_t value) { return value; }
uint64_t MessageSerializer::htole64(uint64_t value) { return value; }
uint16_t MessageSerializer::le16toh(uint16_t value) { return value; }
uint32_t MessageSerializer::le32toh(uint32_t value) { return value; }
uint64_t MessageSerializer::le64toh(uint64_t value) { return value; }

// Message type utilities
MessageType getMessageType(const std::string& command) {
    if (command == "version") return MessageType::VERSION;
    if (command == "verack") return MessageType::VERACK;
    if (command == "ping") return MessageType::PING;
    if (command == "pong") return MessageType::PONG;
    if (command == "sendheaders") return MessageType::SENDHEADERS;
    if (command == "sendcmpct") return MessageType::SENDCMPCT;
    if (command == "addr") return MessageType::ADDR;
    if (command == "getheaders") return MessageType::GETHEADERS;
    if (command == "headers") return MessageType::HEADERS;
    if (command == "getdata") return MessageType::GETDATA;
    if (command == "block") return MessageType::BLOCK;
    if (command == "cmpctblock") return MessageType::CMPCTBLOCK;
    if (command == "getblocktxn") return MessageType::GETBLOCKTXN;
    if (command == "blocktxn") return MessageType::BLOCKTXN;
    return MessageType::UNKNOWN;
}

std::string getMessageCommand(MessageType type) {
    switch (type) {
        case MessageType::VERSION: return "version";
        case MessageType::VERACK: return "verack";
        case MessageType::PING: return "ping";
        case MessageType::PONG: return "pong";
        case MessageType::SENDHEADERS: return "sendheaders";
        case MessageType::SENDCMPCT: return "sendcmpct";
        case MessageType::ADDR: return "addr";
        case MessageType::GETHEADERS: return "getheaders";
        case MessageType::HEADERS: return "headers";
        case MessageType::GETDATA: return "getdata";
        case MessageType::BLOCK: return "block";
        case MessageType::CMPCTBLOCK: return "cmpctblock";
        case MessageType::GETBLOCKTXN: return "getblocktxn";
        case MessageType::BLOCKTXN: return "blocktxn";
        default: return "unknown";
    }
}

} // namespace p2p
} // namespace dinero
