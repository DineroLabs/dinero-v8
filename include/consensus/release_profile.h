#pragma once
#include "consensus/shielded/compact.h"
#include <stdexcept>
#include <utility>

namespace dinero::consensus {
inline constexpr const char* kReleaseProfileV8113 = "compact-v1-60s-v1";

// Individual regtest experiments can still use their existing independent
// switches. A scheduled release must couple both rules and the service cutoff;
// a public network must not accidentally schedule only part of the release.
inline bool ReleaseProfileConfigurationValid(const ChainParams& params) {
    const auto height = params.release_v8113_activation_height;
    if (height == UINT32_MAX) {
        return params.name == "regtest" ||
            (params.shielded_compact_activation_height == UINT32_MAX &&
             params.sixty_second_activation_height == UINT32_MAX);
    }
    return height != 0 && params.shielded_compact_activation_height == height &&
        params.sixty_second_activation_height == height &&
        shielded::CompactActivationConfigurationValid(params);
}

// Transactional configuration for source-defined public profiles and isolated
// regtest qualification. Failed prerequisite checks leave the caller unchanged.
inline void ConfigureReleaseV8113(ChainParams& params, uint32_t height) {
    auto candidate = params;
    candidate.release_v8113_activation_height = height;
    candidate.shielded_compact_activation_height = height;
    candidate.sixty_second_activation_height = height;
    if (!ReleaseProfileConfigurationValid(candidate))
        throw std::invalid_argument("invalid joint compact/60-second release profile");
    params = std::move(candidate);
}

// Service policy, not transaction validity. Evaluate the NEXT block using a
// locally validated tip, not peer-advertised headers or a wall-clock date. The
// widened addition and explicit sentinel avoid wraparound/accidental activation.
inline bool ReleaseServiceCutoffActive(const ChainParams& params, uint32_t validated_tip) {
    return ReleaseProfileConfigurationValid(params) &&
        params.release_v8113_activation_height != UINT32_MAX &&
        uint64_t(validated_tip) + 1 >= params.release_v8113_activation_height;
}
} // namespace dinero::consensus
