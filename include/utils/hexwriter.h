#pragma once
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

// ============================================================================
//  HexWriter - Lightweight helper for consistent Bitcoin-style serialization
//  Works with Dinero's string-based block builder architecture.
// ============================================================================
//
//  SERIALIZATION VERSION: 1
//  - Genesis block and all current serialization uses this version
//  - Future upgrades (SegWit, Taproot, custom scripts) will use version 2+
//  - Versioning allows backward-compatible protocol upgrades
// ============================================================================

constexpr uint8_t DINERO_SERIALIZATION_VERSION = 1;

class HexWriter {
public:
    HexWriter() = default;

    // Write a raw hex string (no spaces, already little-endian)
    HexWriter& write(const std::string& hex) {
        buffer_ << hex;
        return *this;
    }

    // Write an unsigned integer (little-endian, fixed byte width)
    HexWriter& writeLE(uint64_t value, size_t bytes = 8) {
        for (size_t i = 0; i < bytes; ++i) {
            uint8_t byte = static_cast<uint8_t>(value & 0xFF);
            buffer_ << std::hex << std::setfill('0') << std::setw(2)
                     << static_cast<int>(byte);
            value >>= 8;
        }
        return *this;
    }

    // Write a variable-length integer (Bitcoin VarInt style)
    HexWriter& writeVarInt(uint64_t value) {
        if (value < 0xfd) {
            writeLE(value, 1);
        } else if (value <= 0xffff) {
            buffer_ << "fd";
            writeLE(value, 2);
        } else if (value <= 0xffffffff) {
            buffer_ << "fe";
            writeLE(value, 4);
        } else {
            buffer_ << "ff";
            writeLE(value, 8);
        }
        return *this;
    }

    // Convert byte vector to hex
    HexWriter& writeBytes(const std::vector<uint8_t>& bytes) {
        for (auto b : bytes)
            buffer_ << std::hex << std::setfill('0') << std::setw(2)
                     << static_cast<int>(b);
        return *this;
    }

    // Get final hex string
    std::string str() const { return buffer_.str(); }

    // Reset the buffer
    void clear() { buffer_.str(""); }

private:
    std::ostringstream buffer_;
};
