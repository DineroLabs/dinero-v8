#pragma once
#include <vector>
#include <cstdint>

// Confidential transaction OP_CTCOMMIT script builder
// Returns scriptPubKey: OP_2 <64-byte commitment>
inline std::vector<uint8_t> BuildCTCommitScript(const std::vector<uint8_t>& commitment)
{
    std::vector<uint8_t> script;
    script.push_back(0x52);  // OP_2 (witness version 2)
    script.push_back(static_cast<uint8_t>(commitment.size()));  // push length
    script.insert(script.end(), commitment.begin(), commitment.end());
    return script;
}
