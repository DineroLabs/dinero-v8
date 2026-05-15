#pragma once

// AUTO-GENERATED from src/consensus/chainparams_impl.cpp.
// DO NOT EDIT BY HAND — run tools/sync_network_constants_headers.py
// to regenerate. The drift test in tests/integration/test_network_magic_sync.sh
// fails the build if the contents of this file disagree with the
// canonical chainparams source.

#include <cstdint>
#include <string_view>

namespace dinero::seeder {

// P2P wire magic, per network. Mirrors the .magic field on the
// ChainParams struct for each Dinero chain. The seeder picks one of
// these based on its CLI --network flag (defaults to mainnet).
inline constexpr uint32_t kMagicMainnet = 0xD1A0C0DEu;
inline constexpr uint32_t kMagicTestnet = 0xDAB5BFFAu;
inline constexpr uint32_t kMagicRegtest = 0xFABFB5DAu;

inline constexpr uint32_t MagicForNetwork(std::string_view network) {
    if (network == "mainnet" || network == "main") {
        return kMagicMainnet;
    }
    if (network == "testnet" || network == "test") {
        return kMagicTestnet;
    }
    if (network == "regtest") {
        return kMagicRegtest;
    }
    return 0u;  // unknown — caller should treat as error
}

} // namespace dinero::seeder
