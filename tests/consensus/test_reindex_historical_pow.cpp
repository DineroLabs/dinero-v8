// Mainnet archival reindex must accept the historical ASERT work schedule.
// The real H100001 header met its encoded target, but Step 5 used to reject
// it at the assumevalid boundary because its target exceeded MAX_BITS.
#include "consensus/chainparams.h"
#include "consensus/pow.h"
#include "consensus/reindexer_detail.h"
#include "primitives/block.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint8_t> DecodeHex(std::string_view hex) {
    Require(hex.size() % 2 == 0, "odd-length header fixture");
    auto digit = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        throw std::runtime_error("invalid header fixture hex");
    };
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    return bytes;
}
}

int main() {
    try {
        SelectParams(Chain::MAINNET);
        // Canonical H100001 header from the verified H0..H149999 Dell frame
        // stream. Its hash is independently present on the NA main chain.
        const auto bytes = DecodeHex(
            "0100000049378b7a93ec14ab7fadb4219fbc9ed1680ebbaad90b7bc6459805d807000000"
            "a3e473caebc6998f478d7fffee4e5b62838fb80d32721bfa92745f8f4cf89e8e377"
            "98e53bfd40d3e028a674726a8f0db48f683f0f9faeb90af894d6d0b86ce249bee966"
            "a000000000180001e7143d300000000000000000000000000");
        Require(bytes.size() == 128, "historical header fixture is not 128 bytes");
        const auto decoded = BlockHeader::Deserialize(bytes);
        Require(decoded.has_value(), "historical header fixture did not decode");
        const auto& header = *decoded;
        Require(header.GetHash().GetHex() ==
                    "0000005e2989b7957511f9d436dd68c8e62dd513cd37aeb7d53f1a7f86b63904",
                "historical header identity changed");
        Require(header.difficulty == 0x1e008001, "historical difficulty changed");
        Require(CheckProofOfWork(header, false), "historical hash does not meet its target");
        Require(!CheckDifficultyBits(header.difficulty),
                "historical ASERT target no longer demonstrates the legacy floor mismatch");
        Require(reindex_detail::CheckReindexHeaderWork(header, 100001, true),
                "archival reindex rejects canonical mainnet H100001");
        Require(reindex_detail::CheckReindexHeaderWork(header, 100001, false),
                "full-work archival reindex rejects canonical mainnet H100001");

        auto bad = header;
        ++bad.nonce;
        Require(!CheckProofOfWork(bad, false), "mutated fixture unexpectedly meets target");
        Require(!reindex_detail::CheckReindexHeaderWork(bad, 100001, true),
                "archival reindex bypassed recent-block proof of work");
        Require(!reindex_detail::CheckReindexHeaderWork(bad, 100001, false),
                "full-work archival reindex bypassed proof of work");
        Require(reindex_detail::CheckReindexHeaderWork(bad, 100000, true),
                "historical assumevalid boundary changed");
        std::cout << "ReindexHistoricalPoW: canonical H100001 and tampered-work controls PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ReindexHistoricalPoW: " << e.what() << '\n';
        return 1;
    }
}
