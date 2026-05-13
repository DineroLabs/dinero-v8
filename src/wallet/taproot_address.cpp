#include <cstring>
#include "wallet/taproot_address.h"
#include "common/sha256d.h"
#include <algorithm>
#include <stdexcept>

namespace din {

// Bech32m constants (BIP-350)
static const char* BECH32M_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static const uint32_t BECH32M_CONST = 0x2bc830a3;

// Taproot constants (BIP-341)
static const uint8_t TAPROOT_VERSION = 0x01;
static const uint8_t TAPROOT_LEAF_VERSION = 0xC0;

std::string TaprootAddress::fromPubkey(const std::vector<uint8_t>& pubkey,
                                      const std::string& network) {
    if (!isValidPubkey(pubkey)) {
        throw std::invalid_argument("Invalid Taproot public key");
    }

    std::string hrp = getHrp(network);

    // Convert 32-byte pubkey from 8-bit to 5-bit
    auto converted_program = convertBits(pubkey, 8, 5, true);

    // Prepend witness version (1) as a 5-bit value
    std::vector<uint8_t> data;
    data.push_back(TAPROOT_VERSION); // Witness version 1 (5-bit value)
    data.insert(data.end(), converted_program.begin(), converted_program.end());

    // Encode with Bech32m (data is already in 5-bit)
    return bech32mEncodeSegWit(hrp, data);
}

bool TaprootAddress::isValid(const std::string& address, const std::string& network) {
    if (address.empty()) return false;
    
    auto decoded = bech32mDecode(address);
    if (!decoded) return false;
    
    std::string hrp = decoded->first;
    std::vector<uint8_t> data = decoded->second;
    
    // Check HRP matches network
    if (hrp != getHrp(network)) return false;
    
    // Check data length (32 bytes for P2TR)
    if (data.size() != 32) return false;
    
    return true;
}

std::optional<std::vector<uint8_t>> TaprootAddress::extractPubkey(const std::string& address) {
    auto decoded = bech32mDecode(address);
    if (!decoded) return std::nullopt;
    
    std::vector<uint8_t> data = decoded->second;
    if (data.size() != 32) return std::nullopt;
    
    return data;
}

std::vector<uint8_t> TaprootAddress::createScriptPubkey(const std::vector<uint8_t>& pubkey) {
    if (!isValidPubkey(pubkey)) {
        throw std::invalid_argument("Invalid Taproot public key");
    }
    
    std::vector<uint8_t> script;
    script.push_back(0x51); // OP_1
    script.push_back(0x20); // Push 32 bytes
    script.insert(script.end(), pubkey.begin(), pubkey.end());
    
    return script;
}

bool TaprootAddress::isValidPubkey(const std::vector<uint8_t>& pubkey) {
    // Taproot uses 32-byte x-only public keys
    if (pubkey.size() != 32) return false;
    
    // Basic validation - full implementation would use secp256k1 curve validation
    // For now, just check it's not all zeros
    return !std::all_of(pubkey.begin(), pubkey.end(), [](uint8_t b) { return b == 0; });
}

std::string TaprootAddress::getHrp(const std::string& network) {
    if (network == "mainnet") return "din";
    if (network == "testnet") return "tdin";
    if (network == "regtest") return "rdin";
    throw std::invalid_argument("Unknown network: " + network);
}

std::string TaprootAddress::bech32mEncode(const std::string& hrp,
                                        const std::vector<uint8_t>& data) {
    // Convert 8-bit data to 5-bit
    auto converted = convertBits(data, 8, 5, true);
    return bech32mEncodeSegWit(hrp, converted);
}

std::string TaprootAddress::bech32mEncodeSegWit(const std::string& hrp,
                                               const std::vector<uint8_t>& data) {
    // data is already in 5-bit format
    // Create values array for checksum calculation
    std::vector<uint8_t> values;

    // Add HRP expansion
    for (char c : hrp) {
        values.push_back(c >> 5);
    }
    values.push_back(0);
    for (char c : hrp) {
        values.push_back(c & 0x1f);
    }

    // Add data (already in 5-bit)
    values.insert(values.end(), data.begin(), data.end());

    // Add 6 zero bytes for checksum
    values.insert(values.end(), 6, 0);

    // Calculate checksum
    uint32_t checksum = 1;
    for (uint8_t v : values) {
        uint8_t c0 = checksum >> 25;
        checksum = ((checksum & 0x1ffffff) << 5) ^ v;

        if (c0 & 1) checksum ^= 0x3b6a57b2;
        if (c0 & 2) checksum ^= 0x26508e6d;
        if (c0 & 4) checksum ^= 0x1ea119fa;
        if (c0 & 8) checksum ^= 0x3d4233dd;
        if (c0 & 16) checksum ^= 0x2a1462b3;
    }

    checksum ^= BECH32M_CONST;

    // Replace zero bytes with checksum
    for (int i = 0; i < 6; ++i) {
        values[values.size() - 6 + i] = (checksum >> (5 * (5 - i))) & 0x1f;
    }

    // Build result string
    std::string result = hrp + "1";
    // Skip HRP expansion (hrp.size() * 2 + 1 elements) and encode data + checksum
    for (size_t i = hrp.size() * 2 + 1; i < values.size(); ++i) {
        result += BECH32M_CHARSET[values[i]];
    }

    return result;
}

std::optional<std::pair<std::string, std::vector<uint8_t>>> 
TaprootAddress::bech32mDecode(const std::string& address) {
    if (address.empty()) return std::nullopt;
    
    // Find separator
    size_t sep_pos = address.find_last_of('1');
    if (sep_pos == std::string::npos || sep_pos == 0 || sep_pos >= address.size() - 6) {
        return std::nullopt;
    }
    
    std::string hrp = address.substr(0, sep_pos);
    std::string data_part = address.substr(sep_pos + 1);
    
    // Validate HRP
    for (char c : hrp) {
        if (c < 33 || c > 126) return std::nullopt;
    }
    
    // Validate data characters
    for (char c : data_part) {
        if (std::strchr(BECH32M_CHARSET, c) == nullptr) return std::nullopt;
    }
    
    // Convert characters to values
    std::vector<uint8_t> values;
    for (char c : data_part) {
        const char* pos = std::strchr(BECH32M_CHARSET, c);
        if (pos == nullptr) return std::nullopt;
        values.push_back(pos - BECH32M_CHARSET);
    }
    
    // Verify checksum
    std::vector<uint8_t> check_values;
    
    // Add HRP expansion
    for (char c : hrp) {
        check_values.push_back(c >> 5);
    }
    check_values.push_back(0);
    for (char c : hrp) {
        check_values.push_back(c & 0x1f);
    }
    
    // Add data values
    check_values.insert(check_values.end(), values.begin(), values.end());
    
    // Calculate checksum
    uint32_t checksum = 1;
    for (uint8_t v : check_values) {
        uint8_t c0 = checksum >> 25;
        checksum = ((checksum & 0x1ffffff) << 5) ^ v;
        
        if (c0 & 1) checksum ^= 0x3b6a57b2;
        if (c0 & 2) checksum ^= 0x26508e6d;
        if (c0 & 4) checksum ^= 0x1ea119fa;
        if (c0 & 8) checksum ^= 0x3d4233dd;
        if (c0 & 16) checksum ^= 0x2a1462b3;
    }
    
    if (checksum != BECH32M_CONST) return std::nullopt;
    
    // Convert 5-bit data back to 8-bit
    auto converted = convertBits(values, 5, 8, false);
    if (converted.empty()) return std::nullopt;
    
    return std::make_pair(hrp, converted);
}

std::vector<uint8_t> TaprootAddress::convertBits(const std::vector<uint8_t>& data, 
                                                int frombits, int tobits, bool pad) {
    int acc = 0;
    int bits = 0;
    std::vector<uint8_t> ret;
    int maxv = (1 << tobits) - 1;
    int max_acc = (1 << (frombits + tobits - 1)) - 1;
    
    for (uint8_t value : data) {
        if (value < 0 || (value >> frombits) != 0) {
            return {}; // Invalid input
        }
        
        acc = ((acc << frombits) | value) & max_acc;
        bits += frombits;
        
        while (bits >= tobits) {
            bits -= tobits;
            ret.push_back((acc >> bits) & maxv);
        }
    }
    
    if (pad) {
        if (bits > 0) {
            ret.push_back((acc << (tobits - bits)) & maxv);
        }
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv) != 0) {
        return {}; // Invalid padding
    }
    
    return ret;
}

// TaprootKeys implementation
std::vector<uint8_t> TaprootKeys::generateInternalKey(const std::vector<uint8_t>& seed) {
    if (seed.size() != 32) {
        throw std::invalid_argument("Seed must be 32 bytes");
    }
    
    // Basic key generation - full implementation would use secp256k1
    // For now, return a test key derived from seed
    std::vector<uint8_t> internal_key(32);
    for (size_t i = 0; i < 32; ++i) {
        internal_key[i] = seed[i] ^ (i + 1);
    }
    
    return internal_key;
}

std::vector<uint8_t> TaprootKeys::computeOutputKey(
    const std::vector<uint8_t>& internal_pubkey,
    const std::optional<std::vector<uint8_t>>& script_root) {
    
    if (internal_pubkey.size() != 32) {
        throw std::invalid_argument("Internal pubkey must be 32 bytes");
    }
    
    // Basic Taproot tweaking - full implementation would use secp256k1
    // For now, return a test tweaked key
    std::vector<uint8_t> output_key = internal_pubkey;
    
    if (script_root) {
        // XOR with script root for tweaking
        for (size_t i = 0; i < 32; ++i) {
            output_key[i] ^= script_root->at(i);
        }
    }
    
    return output_key;
}

bool TaprootKeys::isValidKeyPair(const std::vector<uint8_t>& pubkey,
                               const std::vector<uint8_t>& privkey) {
    if (pubkey.size() != 32 || privkey.size() != 32) return false;
    
    // Basic key pair validation - full implementation would use secp256k1
    // For now, just check they're not all zeros
    bool pubkey_valid = !std::all_of(pubkey.begin(), pubkey.end(), [](uint8_t b) { return b == 0; });
    bool privkey_valid = !std::all_of(privkey.begin(), privkey.end(), [](uint8_t b) { return b == 0; });
    
    return pubkey_valid && privkey_valid;
}

} // namespace din
