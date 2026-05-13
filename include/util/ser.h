#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace ser {

// Variable integer decoding result
struct VarIntDecode {
    uint64_t value;
    size_t size;
    size_t consumed;  // Alias for size for daemon compatibility
};

// Little-endian write functions
template<typename T>
void writeLE(T value, std::vector<uint8_t>& out);

// Little-endian read functions  
template<typename T>
bool readLE(const uint8_t* data, size_t len, T& out);

// Compact size serialization
void writeCompactSize(uint64_t v, std::vector<uint8_t>& out);
bool readCompactSize(const uint8_t* data, size_t len, VarIntDecode& out);

// Template implementations
template<typename T>
void writeLE(T value, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

template<typename T>
bool readLE(const uint8_t* data, size_t len, T& out) {
    if (len < sizeof(T)) return false;
    
    out = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        out |= static_cast<T>(data[i]) << (i * 8);
    }
    return true;
}

} // namespace ser
} // namespace dinero

// Serialization utilities
std::string serialize(const std::vector<uint8_t>& data);
std::vector<uint8_t> deserialize(const std::string& data);