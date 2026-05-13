#pragma once
#include <string>
#include <optional>

namespace dinero::p2p {

// Append " CIC:<8-hex>" to a UA string if not already present.
std::string AppendCicToUserAgent(const std::string& ua, const std::string& cicPrefixHex);

// Extract 8-hex CIC prefix from a UA string (if present).
std::optional<std::string> ExtractCicPrefixFromUserAgent(const std::string& ua);

} // namespace dinero::p2p
