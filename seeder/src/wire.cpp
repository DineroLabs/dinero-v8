#include "dinero/seeder/wire.h"

#include <openssl/sha.h>

#include <chrono>
#include <cstring>

namespace dinero {
namespace seeder {

namespace {

void append_le32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

void append_le64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// First 4 bytes of double-SHA256 of payload.
uint32_t checksum(const std::vector<uint8_t>& payload) {
    uint8_t first[SHA256_DIGEST_LENGTH];
    uint8_t second[SHA256_DIGEST_LENGTH];
    SHA256(payload.data(), payload.size(), first);
    SHA256(first, SHA256_DIGEST_LENGTH, second);
    return read_le32(second);
}

}  // namespace

std::vector<uint8_t> build_frame(const std::string& command,
                                  const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(24 + payload.size());

    // 4-byte magic (little-endian).
    append_le32(frame, kMagicBytes);

    // 12-byte command, null-padded.
    for (size_t i = 0; i < 12; ++i) {
        frame.push_back(i < command.size()
                        ? static_cast<uint8_t>(command[i])
                        : 0);
    }

    // 4-byte payload length (little-endian).
    append_le32(frame, static_cast<uint32_t>(payload.size()));

    // 4-byte checksum.
    append_le32(frame, checksum(payload));

    // Payload.
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

bool parse_frame(const uint8_t* buf, size_t len, Frame& out, size_t& consumed) {
    if (len < 24) {
        // Need a full header before we can do anything.
        return false;
    }

    const uint32_t magic = read_le32(buf);
    if (magic != kMagicBytes) {
        // Bad magic — caller should slide by 1 byte and re-sync.
        consumed = 1;
        return false;
    }

    const uint32_t payload_len = read_le32(buf + 16);
    // Reject obviously oversized frames (Dinero's daemon also caps at 2 MB).
    if (payload_len > 2u * 1024u * 1024u) {
        consumed = 1;
        return false;
    }
    if (len < 24 + payload_len) {
        // Need more bytes; not a parse error.
        return false;
    }

    // Verify checksum.
    std::vector<uint8_t> payload(buf + 24, buf + 24 + payload_len);
    const uint32_t expected_checksum = read_le32(buf + 20);
    if (checksum(payload) != expected_checksum) {
        consumed = 1;
        return false;
    }

    // Extract command (strip trailing NULs).
    std::string command(reinterpret_cast<const char*>(buf + 4), 12);
    const auto first_null = command.find('\0');
    if (first_null != std::string::npos) command.resize(first_null);

    out.command = std::move(command);
    out.payload = std::move(payload);
    consumed = 24 + payload_len;
    return true;
}

namespace {

// Pack an "addr" entry in version-message addrRecv/addrFrom format
// (26 bytes): services (8) + IPv6 (16, with IPv4 marker) + port (big-endian, 2).
// Seeder always identifies as 0.0.0.0:port — we're a transient
// listener-less client.
void append_net_addr_v(std::vector<uint8_t>& buf, uint64_t services) {
    append_le64(buf, services);
    for (int i = 0; i < 10; ++i) buf.push_back(0);  // IPv6 prefix
    buf.push_back(0xff); buf.push_back(0xff);       // IPv4 marker
    buf.push_back(0); buf.push_back(0); buf.push_back(0); buf.push_back(0);  // 0.0.0.0
    buf.push_back((kDefaultMainnetPort >> 8) & 0xFF);  // port (big-endian)
    buf.push_back(kDefaultMainnetPort & 0xFF);
}

void append_varint(std::vector<uint8_t>& buf, uint64_t v) {
    if (v < 0xFD) {
        buf.push_back(static_cast<uint8_t>(v));
    } else if (v <= 0xFFFF) {
        buf.push_back(0xFD);
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    } else if (v <= 0xFFFFFFFFull) {
        buf.push_back(0xFE);
        for (int i = 0; i < 4; ++i) buf.push_back((v >> (i * 8)) & 0xFF);
    } else {
        buf.push_back(0xFF);
        for (int i = 0; i < 8; ++i) buf.push_back((v >> (i * 8)) & 0xFF);
    }
}

void append_varstring(std::vector<uint8_t>& buf, const std::string& s) {
    append_varint(buf, s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}

}  // namespace

std::vector<uint8_t> build_version_payload(uint64_t nonce,
                                            const std::string& user_agent,
                                            uint32_t best_height) {
    std::vector<uint8_t> payload;
    payload.reserve(128);

    append_le32(payload, kProtocolVersion);
    append_le64(payload, kServiceNodeNetwork);

    const uint64_t now = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count() /
        std::chrono::system_clock::period::den);
    append_le64(payload, now);

    append_net_addr_v(payload, kServiceNodeNetwork);  // addrRecv
    append_net_addr_v(payload, kServiceNodeNetwork);  // addrFrom

    append_le64(payload, nonce);
    append_varstring(payload, user_agent);
    append_le32(payload, best_height);

    // relay flag (BIP37) — seeder doesn't want tx relay
    payload.push_back(0);

    return payload;
}

std::vector<uint8_t> build_getaddr_payload() {
    // getaddr has an empty payload.
    return {};
}

std::vector<std::pair<std::string, uint16_t>>
parse_addr_payload(const std::vector<uint8_t>& payload) {
    // Match the format that src/daemon/p2p_manager.cpp:handle_addr expects
    // for parsing: 1 byte count, then for each entry: 1 byte addr length
    // (IPv4-string length, typically 7-15), addr bytes, 2 byte port (LE).
    // This is Dinero-specific and simpler than Bitcoin's CAddress wire format.
    std::vector<std::pair<std::string, uint16_t>> out;
    if (payload.empty()) return out;

    const uint8_t count = payload[0];
    size_t offset = 1;
    for (uint8_t i = 0; i < count && offset < payload.size(); ++i) {
        if (offset >= payload.size()) break;
        const uint8_t addr_len = payload[offset++];
        if (offset + addr_len + 2 > payload.size()) break;

        std::string addr(payload.begin() + offset,
                         payload.begin() + offset + addr_len);
        offset += addr_len;

        const uint16_t port = static_cast<uint16_t>(payload[offset]) |
                              (static_cast<uint16_t>(payload[offset + 1]) << 8);
        offset += 2;

        if (addr.empty() || port == 0) continue;
        out.emplace_back(std::move(addr), port);
    }
    return out;
}

}  // namespace seeder
}  // namespace dinero
