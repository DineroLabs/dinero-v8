#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

/**
 * Common helper to build scriptPubKey from address
 * Used by both mining.setaddress and generatetoaddress to ensure consistency
 */
bool BuildScriptPubKeyFromAddress(const std::string& addr, std::vector<uint8_t>& spk, std::string& error);

/**
 * Convert scriptPubKey bytes to hex string
 */
std::string ScriptPubKeyToHex(const std::vector<uint8_t>& spk);

} // namespace dinero
