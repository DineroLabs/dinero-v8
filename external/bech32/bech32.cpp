#include "bech32.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <tuple>

namespace bech32 {

namespace {
const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

std::vector<uint8_t> hrpExpand(const std::string& hrp) {
    std::vector<uint8_t> ret;
    ret.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) {
        char lower_c = std::tolower(c);
        ret.push_back(lower_c >> 5);
    }
    ret.push_back(0);
    for (char c : hrp) {
        char lower_c = std::tolower(c);
        ret.push_back(lower_c & 31);
    }
    return ret;
}

uint32_t polymod(const std::vector<uint8_t>& values) {
    uint32_t c = 1;
    for (auto v : values) {
        uint8_t c0 = c >> 25;
        c = (c & 0x1ffffff) << 5 ^ v;
        if (c0 & 1) c ^= 0x3b6a57b2;
        if (c0 & 2) c ^= 0x26508e6d;
        if (c0 & 4) c ^= 0x1ea119fa;
        if (c0 & 8) c ^= 0x3d4233dd;
        if (c0 & 16) c ^= 0x2a1462b3;
    }
    return c;
}


} // namespace

// Wrapper function for external access
bool convertbits(std::vector<uint8_t>& out, const std::vector<uint8_t>& in, int frombits, int tobits, bool pad) {
    int acc = 0;
    int bits = 0;
    int maxv = (1 << tobits) - 1;
    int max_acc = (1 << (frombits + tobits - 1)) - 1;
    for (auto value : in) {
        if (value >> frombits) {
            return false;
        }
        acc = ((acc << frombits) | value) & max_acc;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            out.push_back((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits) {
            out.push_back((acc << (tobits - bits)) & maxv);
        }
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
        return false;
    }
    return true;
}

std::vector<uint8_t> hrpExpandBech32m(const std::string& hrp) {
    std::vector<uint8_t> ret;
    ret.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) {
        char lower_c = std::tolower(c);
        ret.push_back(lower_c >> 5);
    }
    ret.push_back(0);
    for (char c : hrp) {
        char lower_c = std::tolower(c);
        ret.push_back(lower_c & 31);
    }
    return ret;
}

uint32_t polymodBech32m(const std::vector<uint8_t>& values) {
    uint32_t chk = 1;
    for (auto x : values) {
        uint8_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ x;
        if (top & 1) chk ^= 0x3b6a57b2;
        if (top & 2) chk ^= 0x26508e6d;
        if (top & 4) chk ^= 0x1ea119fa;
        if (top & 8) chk ^= 0x3d4233dd;
        if (top & 16) chk ^= 0x2a1462b3;
    }
    return chk;
}

std::tuple<std::string, std::vector<uint8_t>, Encoding> DecodeWithEncoding(const std::string& addr) {
    // Convert to lowercase
    std::string lower_addr = addr;
    std::transform(lower_addr.begin(), lower_addr.end(), lower_addr.begin(), ::tolower);

    // Find separator
    size_t pos = lower_addr.rfind('1');
    if (pos == std::string::npos || pos == 0 || pos + 7 > lower_addr.size()) {
        return {"", {}, Encoding::BECH32};
    }

    // Extract HRP from address
    std::string addr_hrp = lower_addr.substr(0, pos);

    // Decode the data part
    std::vector<uint8_t> data;
    for (size_t i = pos + 1; i < lower_addr.size(); ++i) {
        char c = lower_addr[i];
        const char* p = std::strchr(CHARSET, c);
        if (!p) {
            return {"", {}, Encoding::BECH32};
        }
        data.push_back(p - CHARSET);
    }

    // Try bech32 checksum first
    auto hrp_expand_bech32 = hrpExpand(addr_hrp);
    std::vector<uint8_t> combined_bech32 = hrp_expand_bech32;
    combined_bech32.insert(combined_bech32.end(), data.begin(), data.end());
    uint32_t checksum_bech32 = polymod(combined_bech32);

    if (checksum_bech32 == 1) {
        return {addr_hrp, data, Encoding::BECH32};
    }

    // Try bech32m checksum
    auto hrp_expand_bech32m_local = hrpExpandBech32m(addr_hrp);
    std::vector<uint8_t> combined_bech32m = hrp_expand_bech32m_local;
    combined_bech32m.insert(combined_bech32m.end(), data.begin(), data.end());
    uint32_t checksum_bech32m = polymodBech32m(combined_bech32m);

    if (checksum_bech32m == 0x2bc830a3) { // Bech32m generator constant
        return {addr_hrp, data, Encoding::BECH32M};
    }

    return {"", {}, Encoding::BECH32};
}

std::optional<DecodeResult> Decode(const std::string& hrp, const std::string& addr) {
    std::string hrp_result;
    std::vector<uint8_t> data;
    Encoding enc;
    std::tie(hrp_result, data, enc) = DecodeWithEncoding(addr);
    
    if (hrp_result != hrp || data.empty()) return std::nullopt;

    int witver = data[0];
    
    // Exclude the 6-byte checksum from the data before converting
    if (data.size() < 7) return std::nullopt;
    
    std::vector<uint8_t> program_data(data.begin() + 1, data.end() - 6); // Exclude version and 6-byte checksum
    
    std::vector<uint8_t> program;
    if (!convertbits(program, program_data, 5, 8, false)) {
        return std::nullopt;
    }

    // Validate program length
    if (program.size() < 2 || program.size() > 40) {
        return std::nullopt;
    }

    // Validate v0 program length
    if (witver == 0 && program.size() != 20 && program.size() != 32) {
        return std::nullopt;
    }

    return DecodeResult{witver, program, enc};
}

std::optional<RawDecodeResult> DecodeRaw(const std::string& addr) {
    std::string hrp_result;
    std::vector<uint8_t> data_with_checksum;
    Encoding enc;
    std::tie(hrp_result, data_with_checksum, enc) = DecodeWithEncoding(addr);

    if (hrp_result.empty() || data_with_checksum.size() < 6) {
        return std::nullopt;
    }

    std::vector<uint8_t> data(data_with_checksum.begin(), data_with_checksum.end() - 6);
    return RawDecodeResult{hrp_result, data, enc};
}

std::string EncodeRaw(const std::string& hrp, const std::vector<uint8_t>& data, Encoding enc) {
    for (uint8_t value : data) {
        if (value >= 32) {
            return "";
        }
    }

    std::vector<uint8_t> hrp_expand_local;
    uint32_t generator;

    if (enc == Encoding::BECH32) {
        hrp_expand_local = hrpExpand(hrp);
        generator = 1;
    } else {
        hrp_expand_local = hrpExpandBech32m(hrp);
        generator = 0x2bc830a3;
    }

    std::vector<uint8_t> combined = hrp_expand_local;
    combined.insert(combined.end(), data.begin(), data.end());
    combined.insert(combined.end(), 6, 0);
    uint32_t mod = polymod(combined) ^ generator;

    std::vector<uint8_t> full_data = data;
    for (int i = 0; i < 6; ++i) {
        full_data.push_back((mod >> (5 * (5 - i))) & 31);
    }

    std::string result = hrp + "1";
    for (auto d : full_data) {
        result += CHARSET[d];
    }

    return result;
}

std::string Encode(const std::string& hrp, int witver, const std::vector<uint8_t>& program, Encoding enc) {
    if (witver < 0 || witver > 16) {
        return "";
    }

    if (program.size() < 2 || program.size() > 40) {
        return "";
    }

    if (witver == 0 && program.size() != 20 && program.size() != 32) {
        return "";
    }

    std::vector<uint8_t> spec;
    if (!convertbits(spec, program, 8, 5, true)) {
        return "";
    }

    std::vector<uint8_t> data;
    data.push_back(witver);
    data.insert(data.end(), spec.begin(), spec.end());
    return EncodeRaw(hrp, data, enc);
}

} // namespace bech32
