#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

// parse a hex string into bytes (big endian)
std::vector<uint8_t> HexToBytes(const std::string& hex);

// parse a hex string and reverse to little endian (for block / tx hashes)
std::vector<uint8_t> HexToBytesLE(const std::string& hex);

}
