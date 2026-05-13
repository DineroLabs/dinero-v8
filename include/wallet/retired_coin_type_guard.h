#pragma once

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "consensus/coin_type.h"

namespace dinero {
namespace wallet {

constexpr uint32_t RETIRED_LEGACY_COIN_TYPE = 1447;
constexpr uint32_t HARDENED_DERIVATION_BIT = 0x80000000u;

inline bool PathUsesRetiredLegacyCoinType(const std::vector<uint32_t>& path) {
    return path.size() >= 2 &&
           ((path[1] & ~HARDENED_DERIVATION_BIT) == RETIRED_LEGACY_COIN_TYPE);
}

inline bool PathUsesNonCanonicalCoinType(const std::vector<uint32_t>& path) {
    return path.size() >= 2 &&
           ((path[1] & ~HARDENED_DERIVATION_BIT) != dinero::consensus::DINERO_COIN_TYPE);
}

inline bool TextContainsRetiredLegacyCoinTypePathComponent(const std::string& text) {
    std::size_t pos = text.find("1447");
    while (pos != std::string::npos) {
        const bool prev_ok = (pos > 0 && text[pos - 1] == '/');
        std::size_t end = pos + 4;
        if (end < text.size() && (text[end] == '\'' || text[end] == 'h' || text[end] == 'H')) {
            ++end;
        }

        const bool next_ok =
            end == text.size() ||
            text[end] == '/' ||
            text[end] == ']' ||
            text[end] == ')' ||
            text[end] == ',' ||
            std::isspace(static_cast<unsigned char>(text[end])) != 0;

        if (prev_ok && next_ok) {
            return true;
        }

        pos = text.find("1447", pos + 4);
    }
    return false;
}

inline bool TextPathUsesNonCanonicalCoinType(const std::string& text, uint32_t* coin_type_out = nullptr) {
    if (text.rfind("m/", 0) != 0) {
        return false;
    }

    const std::size_t purpose_end = text.find('/', 2);
    if (purpose_end == std::string::npos || purpose_end + 1 >= text.size()) {
        return false;
    }

    std::size_t pos = purpose_end + 1;
    std::size_t end = pos;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
        ++end;
    }
    if (end == pos) {
        return false;
    }

    const uint32_t coin_type = static_cast<uint32_t>(std::stoul(text.substr(pos, end - pos)));
    if (coin_type_out) {
        *coin_type_out = coin_type;
    }
    return coin_type != dinero::consensus::DINERO_COIN_TYPE;
}

inline std::string RetiredLegacyCoinTypeError(const std::string& context) {
    return context + ": coin_type 1447 is permanently retired; v7 accepts coin_type 1448 only";
}

inline std::string NonCanonicalCoinTypeError(const std::string& context, uint32_t coin_type) {
    return context + ": unsupported coin_type " + std::to_string(coin_type) +
           "; v7 accepts coin_type 1448 only";
}

inline void RejectRetiredLegacyCoinTypePath(const std::vector<uint32_t>& path,
                                            const std::string& context) {
    if (PathUsesRetiredLegacyCoinType(path)) {
        throw std::invalid_argument(RetiredLegacyCoinTypeError(context));
    }
}

inline void RejectNonCanonicalCoinTypePath(const std::vector<uint32_t>& path,
                                           const std::string& context) {
    if (PathUsesNonCanonicalCoinType(path)) {
        throw std::invalid_argument(
            NonCanonicalCoinTypeError(context, path[1] & ~HARDENED_DERIVATION_BIT));
    }
}

inline void RejectRetiredLegacyCoinTypeText(const std::string& text,
                                            const std::string& context) {
    if (TextContainsRetiredLegacyCoinTypePathComponent(text)) {
        throw std::invalid_argument(RetiredLegacyCoinTypeError(context));
    }
}

inline void RejectNonCanonicalCoinTypeTextPath(const std::string& text,
                                               const std::string& context) {
    uint32_t coin_type = 0;
    if (TextPathUsesNonCanonicalCoinType(text, &coin_type)) {
        throw std::invalid_argument(NonCanonicalCoinTypeError(context, coin_type));
    }
}

} // namespace wallet
} // namespace dinero
