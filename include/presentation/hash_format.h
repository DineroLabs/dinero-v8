#pragma once

#include "primitives/uint256.h"
#include <string>

namespace dinero {
namespace presentation {

/**
 * @brief Format hash as full 64-character hex string
 *
 * Phase M.2: Canonical presentation-layer conversion.
 * Use this instead of direct .GetHex() for clarity.
 *
 * @param hash The uint256 to format
 * @return Full hex string (64 chars, lowercase)
 */
inline std::string FormatHash(const uint256& hash) {
    return hash.GetHex();
}

/**
 * @brief Format hash as truncated hex for logging
 *
 * Returns first 16 characters + "..." for human-readable logs.
 * Standard format: "0123456789abcdef..."
 *
 * @param hash The uint256 to format
 * @return Truncated hex string (19 chars)
 */
inline std::string FormatHashShort(const uint256& hash) {
    return hash.GetHex().substr(0, 16) + "...";
}

/**
 * @brief Format outpoint as "txid:vout" for logging
 *
 * @param txid Transaction ID
 * @param vout Output index
 * @return String like "0123456789abcdef...:2"
 */
inline std::string FormatOutpoint(const uint256& txid, uint32_t vout) {
    return FormatHashShort(txid) + ":" + std::to_string(vout);
}

} // namespace presentation
} // namespace dinero
