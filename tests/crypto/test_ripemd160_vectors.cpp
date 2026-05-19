#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(DINERO_RIPEMD160_STANDALONE)
#include "crypto/ripemd160_standalone.h"
#elif defined(DINERO_HASH_WRAPPER)
#include "crypto/hash.h"
#elif defined(DINERO_RIPEMD160_CORE_HEADER)
#include "dinero/core/crypto/ripemd160.h"
#else
#include "crypto/ripemd160.h"
#endif

namespace {

std::string hex(const uint8_t* data, size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return out.str();
}

std::array<uint8_t, 20> ripemd160(const std::string& input) {
    std::array<uint8_t, 20> out{};
#if defined(DINERO_RIPEMD160_STANDALONE)
    dinero::crypto::CRIPEMD160 hasher;
    hasher.Write(reinterpret_cast<const unsigned char*>(input.data()), input.size());
    hasher.Finalize(out.data());
#elif defined(DINERO_HASH_WRAPPER)
    out = din::crypto::RIPEMD160(reinterpret_cast<const uint8_t*>(input.data()), input.size());
#else
    out = dinero::RIPEMD160(reinterpret_cast<const uint8_t*>(input.data()), input.size());
#endif
    return out;
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {"", "9c1185a5c5e9fc54612808977ee8f548b2258d31"},
        {"a", "0bdc9d2d256b3ee9daae347be6f4dc835a467ffe"},
        {"abc", "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc"},
        {"message digest", "5d0689ef49d2fae572b881b123a85ffa21595f36"},
        {"abcdefghijklmnopqrstuvwxyz", "f71c27109c692c1b56bbdceb5b9d2865b3708dbc"},
    };

    for (const auto& [input, expected] : vectors) {
        const auto digest = ripemd160(input);
        const auto actual = hex(digest.data(), digest.size());
        if (actual != expected) {
            std::cerr << "RIPEMD160 vector mismatch for input '" << input
                      << "': expected " << expected << ", got " << actual << "\n";
            return 1;
        }
    }

#if defined(DINERO_HASH_WRAPPER)
    const std::string hash160_input = "hello";
    const auto hash160 = din::crypto::HASH160(
        reinterpret_cast<const uint8_t*>(hash160_input.data()),
        hash160_input.size());
    const auto hash160_actual = hex(hash160.data(), hash160.size());
    const std::string hash160_expected = "b6a9c8c230722b7c748331a8b450f05566dc7d0f";
    if (hash160_actual != hash160_expected) {
        std::cerr << "HASH160 vector mismatch for input 'hello': expected "
                  << hash160_expected << ", got " << hash160_actual << "\n";
        return 1;
    }
#endif

    return 0;
}
