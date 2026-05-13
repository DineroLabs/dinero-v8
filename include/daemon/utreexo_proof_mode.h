#pragma once

#include "consensus/utreexo_accumulator.h"

#include <algorithm>
#include <optional>

namespace dinero::daemon_helpers {

inline bool IsLegacyTransitionOnlyBatchPayload(
    const consensus::BlockUtreexoData& proof_data
) {
    return !proof_data.accumulator_root_before.empty() &&
           std::all_of(proof_data.accumulator_root_before.begin(),
                       proof_data.accumulator_root_before.end(),
                       [](uint8_t byte) { return byte == 0; }) &&
           proof_data.spend_proof.isEmpty() &&
           proof_data.spent_outputs.empty();
}

inline bool ShouldUseTransitionProof(
    const consensus::BlockUtreexoData& proof_data,
    const std::optional<consensus::UtreexoTransitionProof>& transition_proof
) {
    return transition_proof.has_value() &&
           (proof_data.isEmpty() || IsLegacyTransitionOnlyBatchPayload(proof_data));
}

}  // namespace dinero::daemon_helpers
