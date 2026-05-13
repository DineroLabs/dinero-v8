#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include "utils/hexwriter.h"  // For DINERO_SERIALIZATION_VERSION

// ============================================================================
//  HexReader - Lightweight deserializer for Bitcoin-style hex streams
//  Complements HexWriter; works perfectly with Dinero's architecture.
// ============================================================================
//
//  Version checking: Use readVersion() to check serialization compatibility
//  For future-proof code that may encounter newer serialization formats
// ============================================================================

class HexReader {
public:
    explicit HexReader(const std::string& hex)
        : data_(stripSpaces(hex)), pos_(0) {
        if (data_.size() % 2 != 0)
            throw std::runtime_error("HexReader: malformed hex (odd length)");
    }

    // Read N bytes as a little-endian integer
    uint64_t readLE(size_t bytes = 8) {
        if (pos_ + bytes * 2 > data_.size())
            throw std::runtime_error("HexReader: out of bounds");

        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i) {
            std::string byteStr = data_.substr(pos_, 2);
            pos_ += 2;
            uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
            value |= (uint64_t)byte << (8 * i);
        }
        return value;
    }

    // Read a Bitcoin VarInt
    uint64_t readVarInt() {
        uint8_t prefix = static_cast<uint8_t>(readLE(1));
        if (prefix < 0xfd) return prefix;
        else if (prefix == 0xfd) return readLE(2);
        else if (prefix == 0xfe) return readLE(4);
        else return readLE(8);
    }

    // Read N bytes as a vector
    std::vector<uint8_t> readBytes(size_t count) {
        if (pos_ + count * 2 > data_.size())
            throw std::runtime_error("HexReader: out of bounds");

        std::vector<uint8_t> bytes;
        bytes.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            std::string byteStr = data_.substr(pos_, 2);
            pos_ += 2;
            uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
            bytes.push_back(byte);
        }
        return bytes;
    }

    // Peek at next bytes without consuming
    std::string peek(size_t count) const {
        size_t end = pos_ + count * 2;
        if (end > data_.size()) end = data_.size();
        return data_.substr(pos_, end - pos_);
    }

    // Move read position manually
    void skip(size_t countBytes) {
        size_t newPos = pos_ + countBytes * 2;
        if (newPos > data_.size())
            throw std::runtime_error("HexReader: skip out of range");
        pos_ = newPos;
    }

    // Remaining bytes
    size_t remaining() const { return (data_.size() - pos_) / 2; }

    // Return current position (in bytes)
    size_t position() const { return pos_ / 2; }

    // Reset position to start
    void rewind() { pos_ = 0; }

private:
    std::string data_;
    size_t pos_;

    // Strip spaces and newlines for clean parsing
    static std::string stripSpaces(const std::string& input) {
        std::string out;
        out.reserve(input.size());
        for (char c : input) {
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
                out.push_back(c);
        }
        return out;
    }
};
