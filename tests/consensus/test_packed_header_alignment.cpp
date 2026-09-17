// BlockHeader is packed: all placements allowed by alignof(BlockHeader) must
// serialize without binding an aligned reference to an unaligned member.
#include "common/serialization.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string Golden() {
    // Literal little-endian header layout; distinct hash/root patterns expose
    // field swaps. High timestamp bits must survive, including at offset 100.
    std::string bytes("\x78\x56\x34\x12", 4);
    bytes += std::string(32, '\xaa');
    bytes += std::string(32, '\xbb');
    bytes += std::string(32, '\xcc'); // Utreexo root at [68, 100).
    bytes.append("\x08\x07\x06\x05\x04\x03\x02\x01", 8);
    bytes.append("\xef\xbe\xad\xde\xbe\xba\xfe\xca", 8);
    bytes += std::string(12, '\x42');
    return bytes;
}
} // namespace

int main() try {
    static_assert(alignof(dinero::BlockHeader) == 1);
    static_assert(sizeof(dinero::BlockHeader) == 128);
    const auto expected = Golden();
    Require(expected.size() == 128, "bad independent vector");
    for (size_t offset = 0; offset < alignof(uint64_t); ++offset) {
        alignas(uint64_t) std::array<unsigned char, 128 + alignof(uint64_t)> storage{};
        auto* header = new (storage.data() + offset) dinero::BlockHeader{};
        header->version = 0x12345678;
        std::memset(header->prev_block_hash.data, 0xaa, 32);
        std::memset(header->merkle_root.data, 0xbb, 32);
        std::memset(header->utreexo_root.data, 0xcc, 32);
        header->timestamp = 0x0102030405060708ULL;
        header->difficulty = 0xdeadbeef;
        header->nonce = 0xcafebabe;
        std::memset(header->reserved, 0x42, 12);
        dinero::VectorWriter writer;
        dinero::Serialize(writer, *header);
        Require(writer.release_string() == expected, "serialized header bytes changed");

        // Deserialize into every permitted placement as well, then serialize
        // again. No assertion helper may itself bind to a packed scalar.
        alignas(uint64_t) std::array<unsigned char, 128 + alignof(uint64_t)> decoded_storage{};
        auto* decoded = new (decoded_storage.data() + offset) dinero::BlockHeader{};
        dinero::Reader reader(expected);
        dinero::Deserialize(reader, *decoded);
        Require(reader.eof(), "decoder did not consume exactly one header");
        Require(decoded->timestamp == 0x0102030405060708ULL, "timestamp changed");
        Require(std::memcmp(decoded->utreexo_root.data, header->utreexo_root.data, 32) == 0,
                "Utreexo root changed");
        dinero::VectorWriter roundtrip;
        dinero::Serialize(roundtrip, *decoded);
        Require(roundtrip.release_string() == expected, "roundtrip bytes changed");
        decoded->~BlockHeader();
        header->~BlockHeader();
    }
    Require(dinero::VerifyBlockHeaderSerializationRoundTrip(), "startup self-check failed");
    std::cout << "PASS packed header placements, exact 128 bytes and Utreexo root\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
