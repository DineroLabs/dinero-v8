#include "daemon/p2p_header_parser.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> HeaderBytes() {
    std::string bytes("\x78\x56\x34\x12", 4);
    bytes += std::string(32, '\xaa');
    bytes += std::string(32, '\xbb');
    bytes += std::string(32, '\xcc');
    bytes.append("\x08\x07\x06\x05\x04\x03\x02\x01", 8);
    bytes.append("\xef\xbe\xad\xde\xbe\xba\xfe\xca", 8);
    bytes += std::string(12, '\x42');
    return {bytes.begin(), bytes.end()};
}
}

int main() try {
    const auto golden = HeaderBytes();
    // Eight 129-byte entries exercise every scalar alignment behind the
    // count prefix. The decoder is exactly the one used by daemon P2P ingress.
    std::vector<uint8_t> payload{8};
    for (int i = 0; i < 8; ++i) {
        payload.insert(payload.end(), golden.begin(), golden.end());
        payload.push_back(0);
    }
    auto decoded = dinero::daemon::ParseHeadersPayload(payload);
    Require(decoded.size() == 8, "header count changed");
    for (const auto& header : decoded) {
        Require(header.version == 0x12345678, "version changed");
        Require(header.timestamp == 0x0102030405060708ULL, "timestamp changed");
        Require(header.difficulty == 0xdeadbeef && header.nonce == 0xcafebabe,
                "PoW fields changed");
        const auto bytes = header.SerializeForHash();
        Require(std::vector<uint8_t>(bytes.begin(), bytes.end()) == golden,
                "header/Utreexo commitment bytes changed");
    }
    Require(dinero::daemon::ParseHeadersPayload({}).empty(), "empty payload changed");
    Require(dinero::daemon::ParseHeadersPayload({0}).empty(), "zero count changed");
    Require(dinero::daemon::ParseHeadersPayload({0xfd, 0xd1, 0x07}).empty(),
            "2001-header limit changed");
    for (size_t length = 1; length < 129; ++length) {
        const std::vector<uint8_t> truncated(payload.begin(), payload.begin() + length);
        Require(dinero::daemon::ParseHeadersPayload(truncated).empty(),
                "partial first header accepted");
    }
    // Preserve the existing partial-batch behavior: a complete first header
    // followed by a truncated second yields only the complete first header.
    payload.resize(130 + 127);
    Require(dinero::daemon::ParseHeadersPayload(payload).size() == 1,
            "truncated batch framing changed");
    std::cout << "PASS real P2P header decoder, all alignments and exact bytes\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
