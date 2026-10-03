#pragma once
#include "consensus/release_profile.h"
#include <cstdint>

namespace dinero::consensus {
// This configuration enables no runtime caller. Public schedules must be
// explicit and coupled to the existing joint release; isolated regtest can
// exercise the Orchard boundary independently. Parameters stay fixed after
// network selection for the process lifetime.
inline bool OrchardProfileConfigurationValid(const ChainParams& params) {
    if (params.name != "mainnet" && params.name != "testnet" && params.name != "regtest")
        return false;
    const auto height = params.orchard_activation_height;
    if (height == UINT32_MAX) return params.orchard_branch_id == 0;
    if (height == 0 || height > uint32_t(INT32_MAX) || params.orchard_branch_id == 0)
        return false;
    return params.name == "regtest" ||
        (height == params.release_v8113_activation_height && ReleaseProfileConfigurationValid(params));
}
// Transactional, source/test configuration only. Never called from RPC or
// network input. The branch ID is supplied explicitly rather than invented by
// a default or inferred from a transaction. Public prerequisite checks apply.
inline void ConfigureOrchardRelease(ChainParams& params, uint32_t height, uint32_t branch_id) {
    auto next = params;
    ConfigureReleaseV8113(next, height);
    next.orchard_activation_height = height;
    next.orchard_branch_id = branch_id;
    if (!OrchardProfileConfigurationValid(next))
        throw std::invalid_argument("invalid Orchard release profile");
    params = std::move(next);
}
inline bool OrchardActiveForHeight(const ChainParams& params, uint32_t height) {
    return OrchardProfileConfigurationValid(params) &&
        params.orchard_activation_height != UINT32_MAX && height >= params.orchard_activation_height;
}
} // namespace dinero::consensus
