#pragma once
#include <string>

namespace dinero {

struct MiningTarget; // Forward declaration

// Build coinbase scriptPubKey using stored witness data (preferred) or fallback to address decoding
bool BuildCoinbaseScriptPubKey(const MiningTarget& tgt, std::string& out_script, std::string* err = nullptr);

} // namespace dinero
