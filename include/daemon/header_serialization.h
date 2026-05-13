/**
 * Phase N.3: Header Serialization Utilities
 *
 * Converts between Dinero's internal 112-byte header format
 * and Bitcoin's 80-byte wire format for P2P compatibility.
 */

#pragma once

#include "primitives/block.h"
#include <vector>
#include <string>
#include <stdexcept>

namespace dinero {

/**
 * Convert hex string to bytes
 */
inline std::vector<uint8_t> hexToBytes(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::runtime_error("Invalid hex string length");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);

    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(byte);
    }

    return bytes;
}

/**
 * Convert bytes to hex string
 */
inline std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);

    for (uint8_t byte : bytes) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0x0F]);
    }

    return result;
}

/**
 * Serialize BlockHeader to 80-byte Bitcoin wire format.
 *
 * Internal format: 112 bytes (with Utreexo commitment)
 * Wire format: 80 bytes (Bitcoin compatible)
 *
 * @param header The header to serialize
 * @return 80-byte wire format
 * @throws std::runtime_error if serialization fails
 */
std::vector<uint8_t> serializeHeaderForWire(const BlockHeader& header);

/**
 * Deserialize 80-byte Bitcoin wire format to BlockHeader.
 *
 * Note: Utreexo commitment will be empty (filled in when full block arrives).
 *
 * @param data 80-byte wire format
 * @return Parsed BlockHeader
 * @throws std::runtime_error if deserialization fails
 */
BlockHeader deserializeHeaderFromWire(const std::vector<uint8_t>& data);

} // namespace dinero
