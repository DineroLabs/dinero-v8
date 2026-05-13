#include "p2p/cic_tag.h"
#include <algorithm>
#include <cctype>

namespace dinero::p2p {

static inline bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string AppendCicToUserAgent(const std::string& ua, const std::string& cicPrefixHex) {
    // Avoid duplicating if already present
    if (ua.find("CIC:") != std::string::npos) return ua;
    std::string prefix = cicPrefixHex;
    if (prefix.size() > 8) prefix.resize(8);
    return ua + " CIC:" + prefix;
}

std::optional<std::string> ExtractCicPrefixFromUserAgent(const std::string& ua) {
    const std::string tag = "CIC:";
    auto pos = ua.find(tag);
    if (pos == std::string::npos) return std::nullopt;
    pos += tag.size();
    std::string out;
    for (size_t i = pos; i < ua.size() && out.size() < 8; ++i) {
        if (!is_hex(ua[i])) break;
        out.push_back(static_cast<char>(std::tolower(ua[i])));
    }
    if (out.size() == 0) return std::nullopt;
    return out;
}

} // namespace dinero::p2p
