#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <tuple>

namespace bech32 {

enum class Encoding {
    BECH32,
    BECH32M
};

struct DecodeResult {
    int witver;
    std::vector<uint8_t> program;
    Encoding encoding;
};

struct RawDecodeResult {
    std::string hrp;
    std::vector<uint8_t> data;
    Encoding encoding;
};

// Decode bech32 address
std::optional<DecodeResult> Decode(const std::string& hrp, const std::string& addr);

// Encode witness program to bech32 address
std::string Encode(const std::string& hrp, int witver, const std::vector<uint8_t>& program, Encoding enc = Encoding::BECH32);

// Encode generic 5-bit Bech32/Bech32m data without witness-program validation.
std::string EncodeRaw(const std::string& hrp, const std::vector<uint8_t>& data, Encoding enc = Encoding::BECH32);

// Decode generic 5-bit Bech32/Bech32m data without witness-program validation.
std::optional<RawDecodeResult> DecodeRaw(const std::string& addr);


// Internal helper for decoding with encoding detection
std::tuple<std::string, std::vector<uint8_t>, Encoding> DecodeWithEncoding(const std::string& addr);

// Internal helper for bit conversion
bool convertbits(std::vector<uint8_t>& out, const std::vector<uint8_t>& in, int frombits, int tobits, bool pad);

} // namespace bech32
