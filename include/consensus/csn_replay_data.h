#pragma once

#include "consensus/utreexo_accumulator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::consensus {

struct CsnReplayData {
    std::vector<UtreexoHash> spend_targets;
    std::vector<SpentOutputData> spent_outputs;
    bool has_spent_outputs = false;
};

// Reads either historical u32-count/hash-only records or CSN2 records with
// SpentOutputData format 4, 5, or 6. Counts are capped at 1,000,000 and the
// complete record must be consumed. Failure clears every field in out;
// hash-only success also clears metadata left by a previous CSN2 decode.
bool DecodeCsnReplayData(const std::string& blob, CsnReplayData& out);

// Writes the existing CSN2 wire format byte-for-byte. Throws invalid_argument
// for an unsupported format or a target other than 32 bytes, and length_error
// for an excessive count or a field that cannot fit its u32 wire length.
std::string SerializeCsnReplayData(
    const std::vector<UtreexoHash>& targets,
    const std::vector<SpentOutputData>& spent_outputs,
    uint8_t format_version);

}  // namespace dinero::consensus
