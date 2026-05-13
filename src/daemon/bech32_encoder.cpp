#include "bech32_encoder.h"
#include <algorithm>
#include <cstring>

const char* Bech32Encoder::CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

uint32_t Bech32Encoder::polymod(const std::vector<uint8_t>& values) {
    uint32_t chk = 1;
    for (uint8_t value : values) {
        uint32_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ value;
        if (top & 1) chk ^= 0x3b6a57b2;
        if (top & 2) chk ^= 0x26508e6d;
        if (top & 4) chk ^= 0x1ea119fa;
        if (top & 8) chk ^= 0x3d4233dd;
        if (top & 16) chk ^= 0x2a1462b3;
    }
    return chk;
}

std::vector<uint8_t> Bech32Encoder::hrp_expand(const std::string& hrp) {
    std::vector<uint8_t> result;
    result.reserve(hrp.size() * 2 + 1);
    
    for (char c : hrp) {
        result.push_back(c >> 5);
    }
    result.push_back(0);
    for (char c : hrp) {
        result.push_back(c & 31);
    }
    
    return result;
}

bool Bech32Encoder::verify_checksum(const std::string& hrp, const std::vector<uint8_t>& data, Encoding enc) {
    std::vector<uint8_t> values = hrp_expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    
    uint32_t expected = (enc == Encoding::BECH32) ? BECH32_CONST : BECH32M_CONST;
    return polymod(values) == expected;
}

std::vector<uint8_t> Bech32Encoder::create_checksum(const std::string& hrp, const std::vector<uint8_t>& data, Encoding enc) {
    std::vector<uint8_t> values = hrp_expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), 6, 0);  // 6 zero bytes for checksum
    
    uint32_t target = (enc == Encoding::BECH32) ? BECH32_CONST : BECH32M_CONST;
    uint32_t polymod_result = polymod(values) ^ target;
    
    std::vector<uint8_t> checksum(6);
    for (int i = 0; i < 6; ++i) {
        checksum[i] = (polymod_result >> (5 * (5 - i))) & 31;
    }
    
    return checksum;
}

std::vector<uint8_t> Bech32Encoder::convert_bits(const std::vector<uint8_t>& data, int frombits, int tobits, bool pad) {
    int acc = 0;
    int bits = 0;
    std::vector<uint8_t> result;
    int maxv = (1 << tobits) - 1;
    int max_acc = (1 << (frombits + tobits - 1)) - 1;
    
    for (uint8_t value : data) {
        if (value >> frombits) {
            return {}; // Invalid input
        }
        acc = ((acc << frombits) | value) & max_acc;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            result.push_back((acc >> bits) & maxv);
        }
    }
    
    if (pad) {
        if (bits) {
            result.push_back((acc << (tobits - bits)) & maxv);
        }
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
        return {}; // Invalid padding
    }
    
    return result;
}

std::string Bech32Encoder::encode(const std::string& hrp, const std::vector<uint8_t>& data, Encoding enc) {
    std::vector<uint8_t> checksum = create_checksum(hrp, data, enc);
    
    std::string result = hrp + '1';
    for (uint8_t d : data) {
        if (d >= 32) return ""; // Invalid data
        result += CHARSET[d];
    }
    for (uint8_t c : checksum) {
        result += CHARSET[c];
    }
    
    return result;
}

std::pair<std::string, std::vector<uint8_t>> Bech32Encoder::decode(const std::string& str, Encoding enc) {
    size_t pos = str.rfind('1');
    if (pos == std::string::npos || pos == 0 || pos + 7 > str.size() || str.size() > 90) {
        return {"", {}};
    }
    
    std::string hrp = str.substr(0, pos);
    std::string data_part = str.substr(pos + 1);
    
    // Check HRP is lowercase
    for (char c : hrp) {
        if (c < 33 || c > 126) return {"", {}};
    }
    
    // Convert data part from charset to 5-bit values
    std::vector<uint8_t> data;
    for (char c : data_part) {
        const char* found = std::strchr(CHARSET, c);
        if (!found) return {"", {}};
        data.push_back(found - CHARSET);
    }
    
    // Verify checksum
    if (!verify_checksum(hrp, data, enc)) {
        return {"", {}};
    }
    
    // Remove checksum
    data.resize(data.size() - 6);
    
    return {hrp, data};
}

std::string Bech32Encoder::encode_segwit_address(const std::string& hrp, int witness_version, const std::vector<uint8_t>& witness_program) {
    if (witness_version < 0 || witness_version > 16) {
        return ""; // Invalid witness version
    }
    
    if (witness_program.size() < 2 || witness_program.size() > 40) {
        return ""; // Invalid program length
    }
    
    if (witness_version == 0 && witness_program.size() != 20 && witness_program.size() != 32) {
        return ""; // v0 must be 20 or 32 bytes
    }
    
    // Convert program from 8-bit to 5-bit
    std::vector<uint8_t> converted = convert_bits(witness_program, 8, 5, true);
    if (converted.empty()) {
        return ""; // Conversion failed
    }
    
    // Prepend witness version
    std::vector<uint8_t> data;
    data.push_back(witness_version);
    data.insert(data.end(), converted.begin(), converted.end());
    
    // Use bech32 for v0, bech32m for v1+
    Encoding enc = (witness_version == 0) ? Encoding::BECH32 : Encoding::BECH32M;
    
    return encode(hrp, data, enc);
}

Bech32Encoder::DecodeResult Bech32Encoder::decode_segwit_address(const std::string& address) {
    DecodeResult result;
    result.valid = false;
    
    // Try bech32 first (v0)
    auto [hrp, data] = decode(address, Encoding::BECH32);
    if (hrp.empty()) {
        // Try bech32m (v1+)
        auto [hrp_m, data_m] = decode(address, Encoding::BECH32M);
        if (hrp_m.empty()) {
            result.error = "Invalid bech32/bech32m encoding";
            return result;
        }
        hrp = hrp_m;
        data = data_m;
    }
    
    if (data.empty()) {
        result.error = "Empty data";
        return result;
    }
    
    int witness_version = data[0];
    if (witness_version > 16) {
        result.error = "Invalid witness version";
        return result;
    }
    
    // Convert program from 5-bit to 8-bit
    std::vector<uint8_t> program_5bit(data.begin() + 1, data.end());
    std::vector<uint8_t> program = convert_bits(program_5bit, 5, 8, false);
    
    if (program.empty()) {
        result.error = "Invalid program encoding";
        return result;
    }
    
    if (program.size() < 2 || program.size() > 40) {
        result.error = "Invalid program length";
        return result;
    }
    
    if (witness_version == 0 && program.size() != 20 && program.size() != 32) {
        result.error = "Invalid v0 program length";
        return result;
    }
    
    result.valid = true;
    result.hrp = hrp;
    result.witness_version = witness_version;
    result.witness_program = program;
    
    return result;
}
