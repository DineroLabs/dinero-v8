#pragma once

#include "consensus/chain_bundle_generated.h"

#include <string_view>

namespace dinero::consensus {

inline constexpr std::string_view kMainnetNetworkName = "mainnet";
inline constexpr std::string_view kTestnetNetworkName = "testnet";
inline constexpr std::string_view kRegtestNetworkName = "regtest";

inline constexpr std::string_view kMainnetGenesisHash = dinero::chain_bundle::GENESIS_BLOCK_HASH;
inline constexpr std::string_view kTestnetGenesisHash =
    "4b2550cca66ef44cc63f690f8ccba331234d59693f0c0d79cd9c6a71caeb7c41";
inline constexpr std::string_view kRegtestGenesisHash = dinero::chain_bundle::GENESIS_BLOCK_HASH;

inline constexpr std::string_view kGenesisMotto = dinero::chain_bundle::GENESIS_MOTTO;

inline constexpr std::string_view NormalizeNetworkName(std::string_view raw) {
    if (raw == "main" || raw == kMainnetNetworkName) {
        return kMainnetNetworkName;
    }
    if (raw == "test" || raw == kTestnetNetworkName) {
        return kTestnetNetworkName;
    }
    if (raw == "regtest") {
        return kRegtestNetworkName;
    }
    return {};
}

inline constexpr std::string_view ExpectedGenesisForNetwork(std::string_view network) {
    if (network == kMainnetNetworkName) {
        return kMainnetGenesisHash;
    }
    if (network == kTestnetNetworkName) {
        return kTestnetGenesisHash;
    }
    if (network == kRegtestNetworkName) {
        return kRegtestGenesisHash;
    }
    return {};
}

}  // namespace dinero::consensus
