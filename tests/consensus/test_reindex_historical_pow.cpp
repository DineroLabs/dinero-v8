// Mainnet archival reindex must accept the historical ASERT work schedule.
// The real H100001 header met its encoded target, but Step 5 used to reject
// it at the assumevalid boundary because its target exceeded MAX_BITS.
#include "consensus/chainparams.h"
#include "consensus/pow.h"
#include "consensus/pow.hpp"
#include "consensus/reindexer_detail.h"
#include "primitives/block.h"

#include <array>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
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

// The contextual ASERT helper needs only the parent's eleven timestamps and
// the timing-upgrade anchor at H149999. Sparse records keep the regression
// independent of an installed datadir or network endpoint.
struct SparseHeaderDb {
    std::map<int, BlockHeader> headers;

    StatusOr<uint256> getBlockHashByHeight(int height) const {
        const auto it = headers.find(height);
        if (it == headers.end()) return Status::NotFound;
        return it->second.GetHash();
    }
    StatusOr<BlockHeader> getHeader(const uint256& hash) const {
        for (const auto& [height, header] : headers) {
            (void)height;
            if (header.GetHash() == hash) return header;
        }
        return Status::NotFound;
    }
    StatusOr<Block> getBlock(const uint256&) const { return Status::NotFound; }
};

uint32_t ExpectedFromCanonicalPrior(int32_t height, uint64_t candidate_time,
                                    const std::array<uint64_t, 11>& parent_times,
                                    uint32_t parent_bits) {
    SparseHeaderDb prior;
    BlockHeader block1;
    block1.timestamp = 1776518061;
    block1.difficulty = 0x1e00c7ff;
    prior.headers.emplace(1, block1);
    for (size_t i = 0; i < parent_times.size(); ++i) {
        BlockHeader prior_header;
        prior_header.timestamp = parent_times[i];
        prior_header.difficulty = i + 1 == parent_times.size() ? parent_bits : 0;
        prior_header.nonce = static_cast<uint32_t>(height - 11 + i);
        prior.headers.emplace(height - 11 + static_cast<int32_t>(i), prior_header);
    }
    return GetNextWorkRequiredWithChainDB(
        height, static_cast<int64_t>(candidate_time),
        GetConsensusForNetwork(Params()), &prior);
}

struct FullHeaderDb {
    std::vector<BlockHeader> headers;
    StatusOr<uint256> getBlockHashByHeight(int height) const {
        if (height < 0 || static_cast<size_t>(height) >= headers.size()) return Status::NotFound;
        uint256 hash;
        std::memcpy(hash.data, &height, sizeof(height));
        return hash;
    }
    StatusOr<BlockHeader> getHeader(const uint256& hash) const {
        int height = 0;
        std::memcpy(&height, hash.data, sizeof(height));
        if (height < 0 || static_cast<size_t>(height) >= headers.size()) return Status::NotFound;
        return headers[height];
    }
    StatusOr<Block> getBlock(const uint256&) const { return Status::NotFound; }
};

void ScanCanonicalBits(const char* path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.good(), "canonical frame file unavailable");
    FullHeaderDb db;
    db.headers.reserve(150000);
    for (int height = 0; height < 150000; ++height) {
        uint32_t magic = 0;
        uint32_t size = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        file.read(reinterpret_cast<char*>(&size), sizeof(size));
        Require(file.good() && magic == 0xd1a0c0de && size >= 128,
                "canonical frame envelope invalid");
        std::vector<uint8_t> header_bytes(128);
        file.read(reinterpret_cast<char*>(header_bytes.data()), header_bytes.size());
        Require(file.good(), "canonical header truncated");
        auto decoded = BlockHeader::Deserialize(header_bytes);
        Require(decoded.has_value(), "canonical header decode failed");
        db.headers.push_back(*decoded);
        file.seekg(static_cast<std::streamoff>(size - 128 + 4), std::ios::cur);
    }
    int mismatches = 0;
    for (int height = 100001; height < 150000; ++height) {
        const auto& actual = db.headers[height];
        const uint32_t expected = GetNextWorkRequiredWithChainDB(
            height, static_cast<int64_t>(actual.timestamp),
            GetConsensusForNetwork(Params()), &db);
        if (expected != actual.difficulty) {
            if (mismatches < 10)
                std::cerr << "bits mismatch at " << height << ": 0x" << std::hex
                          << expected << " != 0x" << actual.difficulty << std::dec << '\n';
            ++mismatches;
        }
    }
    std::cout << "canonical contextual bits scan: " << mismatches
              << " mismatches across H100001..149999\n";
    Require(mismatches == 0, "canonical contextual bits scan mismatched");
}
}

int main(int argc, char** argv) {
    try {
        SelectParams(Chain::MAINNET);
        if (argc == 2) {
            ScanCanonicalBits(argv[1]);
            return 0;
        }
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
        Require(reindex_detail::CheckReindexHeaderWork(header, 100001, true,
                                                      header.difficulty),
                "archival reindex rejects canonical mainnet H100001");
        Require(reindex_detail::CheckReindexHeaderWork(header, 100001, false,
                                                      header.difficulty),
                "full-work archival reindex rejects canonical mainnet H100001");
        Require(!reindex_detail::CheckReindexHeaderWork(header, 100001, true),
                "post-assumevalid reindex accepted missing difficulty context");

        constexpr std::array<uint64_t, 11> kLegacyParentTimes = {
            1788275058, 1788275179, 1788275295, 1788275419,
            1788275512, 1788275660, 1788275761, 1788275898,
            1788276015, 1788276135, 1788276255,
        };
        Require(ExpectedFromCanonicalPrior(100001, header.timestamp,
                                           kLegacyParentTimes, 0x1e008000u) ==
                    header.difficulty,
                "contextual H100001 bits differ from canonical header");

        constexpr std::array<uint64_t, 11> kH130000ParentTimes = {
            1791874941, 1791875057, 1791875178, 1791875281,
            1791875429, 1791875537, 1791875655, 1791875789,
            1791875794, 1791875949, 1791876135,
        };
        Require(ExpectedFromCanonicalPrior(130000, 1791876261,
                                           kH130000ParentTimes, 0x1e008000u) ==
                    0x1e008002u,
                "contextual H130000 bits differ from canonical header");

        constexpr std::array<uint64_t, 11> kH149999ParentTimes = {
            1794269468, 1794269595, 1794269740, 1794269888,
            1794270038, 1794270120, 1794270212, 1794270292,
            1794270395, 1794270422, 1794270568,
        };
        Require(ExpectedFromCanonicalPrior(149999, 1794270662,
                                           kH149999ParentTimes, 0x1d00ee76u) ==
                    0x1d00ee60u,
                "contextual H149999 bits differ from canonical header");

        auto bad = header;
        ++bad.nonce;
        Require(!CheckProofOfWork(bad, false), "mutated fixture unexpectedly meets target");
        Require(!reindex_detail::CheckReindexHeaderWork(bad, 100001, true,
                                                       header.difficulty),
                "archival reindex bypassed recent-block proof of work");
        Require(!reindex_detail::CheckReindexHeaderWork(bad, 100001, false,
                                                       header.difficulty),
                "full-work archival reindex bypassed proof of work");
        Require(reindex_detail::CheckReindexHeaderWork(bad, 100000, true),
                "historical assumevalid boundary changed");

        // A forged frame can claim an almost free target and satisfy its own
        // encoded PoW without satisfying the live chain's difficulty policy.
        // Reindex used to credit that frame with its claimed work.
        auto easy = header;
        easy.difficulty = 0x207fffff;
        for (uint32_t nonce = 0; nonce < 10000 &&
             !CheckProofOfWork(easy, false); ++nonce) {
            easy.nonce = nonce;
        }
        Require(CheckProofOfWork(easy, false), "forged easy-bits fixture did not meet PoW");
        Require(!reindex_detail::CheckReindexHeaderWork(easy, 100001, true,
                                                       header.difficulty),
                "archival reindex accepted forged easy bits after assumevalid");
        Require(!reindex_detail::CheckReindexHeaderWork(easy, 149999, false,
                                                       0x1d00ee60u),
                "full-work historical reindex accepted forged easy bits");
        Require(!reindex_detail::CheckReindexHeaderWork(easy, 150000, true,
                                                       0x1d011041u),
                "post-upgrade reindex accepted forged easy bits");
        Require(reindex_detail::CheckReindexHeaderWork(easy, 100000, true),
                "historical assumevalid bypass changed for forged bits");
        Require(!reindex_detail::CheckReindexHeaderWork(header, 100000, false),
                "pre-boundary explicit full-work policy changed");
        SelectParams(Chain::TESTNET);
        Require(!reindex_detail::CheckReindexHeaderWork(easy, 100001, true),
                "testnet standard-work policy widened by mainnet exception");
        SelectParams(Chain::MAINNET);

        // Real H150000 header: the first timing-upgrade block must remain
        // accepted with its contextual bits, while a missing or different
        // expected-bits result fails closed.
        const auto v6_bytes = DecodeHex(
            "01000000481d4ac875ef47d66c0f79f3c047261b521ff40ee34270f9bcd0558200000000"
            "d4761166df77b330c76366d9ad25e1b2ad9c29fbadfd1f85d51e51115f0fb840fd8b7"
            "8ca8b1fbb7bd022aed95491dcfbf0f170b9f745b3158cd01dccaf3bafb15986f26a00000000"
            "4110011d057e1e4b000000000000000000000000");
        Require(v6_bytes.size() == 128, "timing-upgrade header fixture is not 128 bytes");
        const auto v6_decoded = BlockHeader::Deserialize(v6_bytes);
        Require(v6_decoded.has_value(), "timing-upgrade header fixture did not decode");
        const auto& v6 = *v6_decoded;
        Require(v6.GetHash().GetHex() ==
                    "00000000c7b0a0a1f94547bf4cb6a6c8446a7ac3e50682c01477685b46939fd2",
                "timing-upgrade header identity changed");
        Require(v6.difficulty == 0x1d011041u && CheckProofOfWork(v6, false),
                "canonical H150000 header failed its encoded work");

        constexpr std::array<uint64_t, 11> kParentTimes = {
            1794269595, 1794269740, 1794269888, 1794270038,
            1794270120, 1794270212, 1794270292, 1794270395,
            1794270422, 1794270568, 1794270662,
        };
        const auto computed = ExpectedFromCanonicalPrior(
            150000, v6.timestamp, kParentTimes, 0x1d00ee60u);
        Require(computed == v6.difficulty,
                "contextual H150000 bits differ from canonical header");
        Require(reindex_detail::CheckReindexHeaderWork(v6, 150000, true, computed),
                "archival reindex rejects canonical H150000");
        Require(!reindex_detail::CheckReindexHeaderWork(v6, 150000, true),
                "post-upgrade reindex accepted missing difficulty context");
        Require(!reindex_detail::CheckReindexHeaderWork(v6, 150000, true,
                                                        v6.difficulty + 1),
                "post-upgrade reindex accepted wrong contextual bits");

        std::cout << "ReindexHistoricalPoW: canonical H100001/H150000 and forged-bits controls PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ReindexHistoricalPoW: " << e.what() << '\n';
        return 1;
    }
}
