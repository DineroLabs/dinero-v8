#include "consensus/block_status_generation.h"

#include <cerrno>
#include <cstdlib>

namespace dinero {
namespace consensus {

std::optional<BlockStatusGeneration> NextGeneration(BlockStatusGeneration current) {
    if (current == std::numeric_limits<BlockStatusGeneration>::max()) {
        return std::nullopt;  // refuse; never wrap
    }
    return current + 1;
}

BlockStatusGeneration ParseBlockStatusGeneration(const std::string& raw) {
    if (raw.empty()) return 0;
    // Reject anything that is not a bare run of digits BEFORE handing it to
    // strtoull, which is too permissive for a value this load-bearing: it skips
    // leading whitespace (" 4" -> 4) and wraps a negative ("-1" -> UINT64_MAX).
    // A plausible-looking huge generation is the worst possible parse result,
    // because it could make a stale acceptance look current. Both were caught
    // by UnreadableCounterFailsClosed.
    for (const char c : raw) {
        if (c < '0' || c > '9') return 0;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw.c_str(), &end, 10);
    // Any junk, overflow, or partial parse reads as 0 — never as a plausible
    // generation. A wrong non-zero value could make a stale result look
    // current, which is the one outcome this must not produce.
    if (errno != 0 || end == raw.c_str() || (end && *end != '\0')) return 0;
    return static_cast<BlockStatusGeneration>(parsed);
}

std::string FormatBlockStatusGeneration(BlockStatusGeneration gen) {
    return std::to_string(gen);
}

}  // namespace consensus
}  // namespace dinero
