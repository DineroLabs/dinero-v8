#pragma once

// One formatter and one parser for every "host:port" string in the node.
//
// "ip:port" is ambiguous for IPv6 -- 2600::1:20999 could be a host with port
// 20999 or an address ending in :1:20999 -- so IPv6 hosts are always written
// "[addr]:port" and parsed by the bracket, never by the last colon. Peer keys,
// ban targets, config endpoints and RPC node arguments all go through here.

#include <cstdint>
#include <string>

namespace dinero::network {

// "1.2.3.4:20999" or "[2600::1]:20999" (any host containing ':' is bracketed).
inline std::string FormatEndpoint(const std::string& host, uint16_t port) {
    if (host.find(':') != std::string::npos) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

// Accepts "host", "host:port", "[v6]", "[v6]:port" and a bare IPv6 ("2600::1",
// taken as a host with no port). Leading/trailing ASCII spaces are ignored.
// False on malformed input (empty host, bad brackets, bad port).
inline bool SplitEndpoint(const std::string& input, std::string* host, uint16_t* port,
                          bool* has_port) {
    if (!host || !port || !has_port) return false;
    size_t b = 0, e = input.size();
    while (b < e && (input[b] == ' ' || input[b] == '\t')) ++b;
    while (e > b && (input[e - 1] == ' ' || input[e - 1] == '\t')) --e;
    const std::string s = input.substr(b, e - b);
    *port = 0;
    *has_port = false;
    if (s.empty()) return false;

    auto parse_port = [](const std::string& t, uint16_t* out) {
        if (t.empty() || t.size() > 5) return false;
        uint32_t v = 0;
        for (char c : t) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint32_t>(c - '0');
        }
        if (v == 0 || v > 65535) return false;
        *out = static_cast<uint16_t>(v);
        return true;
    };

    if (s.front() == '[') {
        const size_t close = s.find(']');
        if (close == std::string::npos || close == 1) return false;
        *host = s.substr(1, close - 1);
        if (close + 1 == s.size()) return true;
        if (s[close + 1] != ':') return false;
        *has_port = parse_port(s.substr(close + 2), port);
        return *has_port;
    }

    const size_t first = s.find(':');
    if (first == std::string::npos) {
        *host = s;
        return true;
    }
    if (first != s.rfind(':')) {
        *host = s;  // bare IPv6, no port
        return true;
    }
    *host = s.substr(0, first);
    if (host->empty()) return false;
    *has_port = parse_port(s.substr(first + 1), port);
    return *has_port;
}

}  // namespace dinero::network
