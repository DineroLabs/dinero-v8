#pragma once

// AUTO-GENERATED from dinero/include/consensus/chain_identity.h
// Run dinero/tools/sync_chain_identity_headers.py to regenerate.

#include <string_view>

namespace dinero::solo {

inline constexpr std::string_view kMainnetGenesisHash = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";
inline constexpr std::string_view kTestnetGenesisHash = "4b2550cca66ef44cc63f690f8ccba331234d59693f0c0d79cd9c6a71caeb7c41";
inline constexpr std::string_view kRegtestGenesisHash = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";

inline constexpr std::string_view NormalizeNetworkName(std::string_view raw) {
    if (raw == "main" || raw == "mainnet") {
        return "mainnet";
    }
    if (raw == "test" || raw == "testnet") {
        return "testnet";
    }
    if (raw == "regtest") {
        return "regtest";
    }
    return {};
}

inline constexpr std::string_view ExpectedGenesisForNetwork(std::string_view network) {
    if (network == "mainnet") {
        return kMainnetGenesisHash;
    }
    if (network == "testnet") {
        return kTestnetGenesisHash;
    }
    if (network == "regtest") {
        return kRegtestGenesisHash;
    }
    return {};
}

} // namespace dinero::solo
