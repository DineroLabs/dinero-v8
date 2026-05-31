/**
 * Real Mainnet Header Replay Test
 *
 * Regression coverage for the #174/#176 interaction:
 * HeaderChainSelector must verify hash <= target on real mainnet headers, but
 * must not apply CheckDifficultyBits' "difficulty >= 1" floor. The live chain's
 * early ASERT blocks legitimately eased below MAX_BITS; block 1 has bits
 * 0x1e00c7ff. require_standard=true would reject it and brick header sync at
 * height 0 even though block-connect accepted the chain.
 */

#include "consensus/chainparams.h"
#include "consensus/header_chain.h"
#include "consensus/pow.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {

constexpr std::array<const char*, 12> kMainnetHeaders = {{
    "01000000000000000000000000000000000000000000000000000000000000000000000064e36bfa00982463da7e9000d6e80ec32e7215ec2e2575c8e09af54fe2da40b0d5fe696dc1aa5c0a800bf800ce8fc6e26ab622c7dd7de4b9fd0c304fe40362560078e16900000000ceff311d225d8330000000000000000000000000",
    "010000006fb72815ae47a082ff3b0f45246c928888c0d00ef43f232c7ef2ab361c0000009ac4ea55ba6a0da81f58164ba9bc890c112b5f0f121ba3722ab2caeb6b74e5ff77ed0ec9b61c081cbf8a322363243913d0e0e21388f82d9c8e4a52914cad237cad83e36900000000ffc7001e20b37c00000000000000000000000000",
    "0100000043d6c481c47ea586ac8e5937371d9a321bef7819c49b928bf5bff64f190000000e0c9aaf6399fe56359f5c4c1b95c664974f21e28ea73e731518056d0101b182b90b7078bf6152f75c8fc46374f67e733c60228d58e58082ee7d6ff536ed670abc83e369000000002fa9001ec7480000000000000000000000000000",
    "01000000477f58404d530e15bc48a20083d41ae2b68746935e3862b44bb626981f000000fc79e7188000dfda14431ce52dae758f07105891d10553e226c1492f4c420f45bc40ac2af999c0a7c2b046a3985bbc9a1ba7102f647e117c0263875c2983122dc683e3690000000002a9001e66f77900000000000000000000000000",
    "01000000b698193514fd0dbc44ad235b2f216f5757a6f2c44c669804c12e4d8932000000e25805a89faa9217e47216dd9f888e1fcfb49e13cdb19005be07ade74a2721fcc5d9b48781732cb77c0147357032aaf36da2b63a025ff0c7e564ec34ea15e0d8cb83e36900000000d2a8001e0c1e2a00000000000000000000000000",
    "0100000023fb85e281c57f48f3741e9e0fa24acb1e96d8e72532181f8b04f20b12000000acafeb1afde23e0485521b0fb1f7261911a4ccedd2a0cef4e5dbf6700b2e8e40ceba35a1e697b4cacf825de758a5191eb5ca07df6420becda8e44b571ab724f4d083e36900000000a3a8001ed2784500000000000000000000000000",
    "010000008c3372a1ba4b4d1923e185e733aebf2cfc547b34b0733017d0f9360b2f000000b24453ecc2dceb95c5e2f47a9bf6955aaa84e9b5e09437492cdb47b9ce399b716ce36afb4ff2e72b299e60975c3c28c4223a15c7f0690ab5693fd5af1daa5a20da83e3690000000075a8001efc957e00000000000000000000000000",
    "010000006a687363bc93d44a23dfeaf394eddc509b73d43c3b8d165f7f5332b33c000000c5df1031b3050bad9007a989b905bf420581a190cb24fd07d581416fcd5b14795d201475255ee732943088331e3cd1bcd55cd13bf25fa5f71b6c0ca12ed25c8b97c0e36900000000ffc7001e673c7600000000000000000000000000",
    "010000009374692c32ac6ddd7a0b9488bea54ccea5a2ee325145e63ebfcc47f62a00000022930c407fcdbbf4ff195cc6e4c0beb81b2be7d7f4856d72aefc11dae18071f73565e0866e734c072033966ecc9b40a26b3e8e693e345c35766f532e82b1b51ba1c0e36900000000ffc7001ef82e1d00000000000000000000000000",
    "0100000090b425ec8ba9acbcde2351dcb85c86e179fd6387d7021484ea7883a55700000080aa570230d0ba274c84db6498529dba88acbce591427422d3db94e04ca0135c337386ddee4660b5e412567dce984e8c369d7ec1e8a3e0ce6f125e291b3faaa2b5c0e36900000000ffc7001eaa24c000000000000000000000000000",
    "01000000b8e811ab0d5d9777d26e14e520551b44e06f1874b80fc38f95149d02bc0000000097455372bc695a1e9639215ad7d9ed3c62c72593d00ca9820b0ab1be2eb18d9a4539597ff143bdde57b7b9768814e7a8a2375aa8639c1c4dc309972473f1b4bac0e36900000000ffc7001e137f2600000000000000000000000000",
    "010000003d7f9516a1b93432058c6db50c58ab6dee957d82e0efa3100cabb2184b000000b85ebd85cc213187032b11417f79ae722fc1c7dce3d817f4bb733be5ef3b19dfff3b8641c155b3543089a6ca2be7c7e5bbb8e1bada113b765d2439fa5071ac11bfc0e36900000000ffc7001e33176200000000000000000000000000",
}};

constexpr std::array<const char*, 12> kExpectedHashes = {{
    "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f",
    "000000194ff6bff58b929bc41978ef1b329a1d3737598eac86a57ec481c4d643",
    "0000001f9826b64bb462385e934687b6e21ad48300a248bc150e534d40587f47",
    "00000032894d2ec10498664cc4f2a657576f212f5b23ad44bc0dfd14351998b6",
    "000000120bf2048b1f183225e7d8961ecb4aa20f9e1e74f3487fc581e285fb23",
    "0000002f0b36f9d0173073b0347b54fc2cbfae33e785e123194d4bbaa172338c",
    "0000003cb332537f5f168d3b3cd4739b50dced94f3eadf234ad493bc6373686a",
    "0000002af647ccbf3ee6455132eea2a5ce4ca5be88940b7add6dac322c697493",
    "00000057a58378ea841402d78763fd79e1865cb8dc5123debcaca98bec25b490",
    "000000bc029d14958fc30fb874186fe0441b5520e5146ed277975d0dab11e8b8",
    "0000004b18b2ab0c10a3efe0827d95ee6dab580cb56d8c053234b9a116957f3d",
    "0000006b69776a8a1a144560e229b4c466e50f55c8d95e41d7aa9d7ed5aab67d",
}};

uint8_t HexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
    throw std::runtime_error("invalid hex character");
}

std::vector<uint8_t> ParseHex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd-length hex string");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>((HexNibble(hex[i]) << 4) |
                                            HexNibble(hex[i + 1])));
    }
    return bytes;
}

BlockHeader HeaderFromHex(std::string_view hex) {
    auto bytes = ParseHex(hex);
    auto header = BlockHeader::Deserialize(bytes);
    if (!header.has_value()) {
        throw std::runtime_error("failed to deserialize header fixture");
    }
    return header.value();
}

}  // namespace

int main() {
    SelectParams(Chain::MAINNET);
    std::cout << "=== Real Mainnet Header Replay Test ===" << std::endl;

    HeaderChainSelector selector;
    for (size_t height = 0; height < kMainnetHeaders.size(); ++height) {
        BlockHeader header = HeaderFromHex(kMainnetHeaders[height]);
        const uint256 expected_hash = uint256::FromHexUnsafe(kExpectedHashes[height]);
        assert(header.GetHash() == expected_hash &&
               "fixture framing/hash must match the known mainnet header hash");

        if (height == 1) {
            // This exact real header fails if header PoW uses require_standard=true.
            assert(header.difficulty == 0x1e00c7ff &&
                   "fixture must include the real early ASERT bits that require_standard=true rejects");
        }

        const bool accepted = selector.AddHeader(header);
        assert(accepted && "real mainnet header fixture must be accepted");

        const HeaderIndexEntry* best = selector.GetBestHeader();
        assert(best != nullptr);
        assert(best->height == height);
        assert(best->hash == header.GetHash());
    }

    const HeaderIndexEntry* best = selector.GetBestHeader();
    assert(best != nullptr);
    assert(best->hash == uint256::FromHexUnsafe(kExpectedHashes.back()));
    assert(selector.GetHeaderCount() == kMainnetHeaders.size());

    HeaderChainSelector reject_selector;
    assert(reject_selector.AddHeader(HeaderFromHex(kMainnetHeaders[0])));

    BlockHeader invalid_pow = HeaderFromHex(kMainnetHeaders[1]);
    invalid_pow.nonce ^= 1;
    assert(!CheckProofOfWork(invalid_pow, /*require_standard=*/false) &&
           "mutated block 1 fixture must be deterministically above target");
    assert(!reject_selector.AddHeader(invalid_pow) &&
           "mutated block 1 header must fail hash <= target");

    const HeaderIndexEntry* reject_best = reject_selector.GetBestHeader();
    assert(reject_best != nullptr);
    assert(reject_best->height == 0);

    std::cout << "Accepted real mainnet headers 0-" << (kMainnetHeaders.size() - 1)
              << " and rejected mutated block 1 PoW" << std::endl;
    return 0;
}
