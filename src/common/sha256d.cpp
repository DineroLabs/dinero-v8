#include "common/sha256d.h"
#include <cstring>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace Dinero {
namespace Common {

// sha256 constants (Bitcoin Core style)
const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// sha256 implementation
sha256::sha256() {
    reset();
}

void sha256::reset() {
    state[0] = 0x6a09e667; state[1] = 0xbb67ae85; state[2] = 0x3c6ef372; state[3] = 0xa54ff53a;
    state[4] = 0x510e527f; state[5] = 0x9b05688c; state[6] = 0x1f83d9ab; state[7] = 0x5be0cd19;
    length = 0;
    buffer_pos = 0;
}

uint32_t sha256::right_rotate(uint32_t value, int shift) {
    return (value >> shift) | (value << (32 - shift));
}

uint32_t sha256::choice(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

uint32_t sha256::majority(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

uint32_t sha256::sigma0(uint32_t x) {
    return right_rotate(x, 2) ^ right_rotate(x, 13) ^ right_rotate(x, 22);
}

uint32_t sha256::sigma1(uint32_t x) {
    return right_rotate(x, 6) ^ right_rotate(x, 11) ^ right_rotate(x, 25);
}

uint32_t sha256::gamma0(uint32_t x) {
    return right_rotate(x, 7) ^ right_rotate(x, 18) ^ (x >> 3);
}

uint32_t sha256::gamma1(uint32_t x) {
    return right_rotate(x, 17) ^ right_rotate(x, 19) ^ (x >> 10);
}

void sha256::transform(const uint8_t* data) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    
    // Prepare message schedule
    for (int i = 0; i < 16; i++) {
        w[i] = (data[i*4] << 24) | (data[i*4+1] << 16) | (data[i*4+2] << 8) | data[i*4+3];
    }
    
    for (int i = 16; i < 64; i++) {
        w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16];
    }
    
    // Initialize working variables
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    
    // Main loop
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sigma1(e) + choice(e, f, g) + SHA256_K[i] + w[i];
        uint32_t t2 = sigma0(a) + majority(a, b, c);
        
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    
    // Update state
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256::update(const uint8_t* data, size_t len) {
    length += len * 8;
    
    for (size_t i = 0; i < len; i++) {
        buffer[buffer_pos++] = data[i];
        if (buffer_pos == 64) {
            transform(buffer);
            buffer_pos = 0;
        }
    }
}

std::vector<uint8_t> sha256::finalize() {
    // Pad the message
    buffer[buffer_pos++] = 0x80;
    if (buffer_pos > 56) {
        while (buffer_pos < 64) buffer[buffer_pos++] = 0;
        transform(buffer);
        buffer_pos = 0;
    }
    
    while (buffer_pos < 56) buffer[buffer_pos++] = 0;
    
    // Append length
    for (int i = 0; i < 8; i++) {
        buffer[56 + i] = (length >> (56 - i * 8)) & 0xFF;
    }
    transform(buffer);
    
    // Convert to bytes
    std::vector<uint8_t> result(32);
    for (int i = 0; i < 8; i++) {
        result[i*4] = (state[i] >> 24) & 0xFF;
        result[i*4+1] = (state[i] >> 16) & 0xFF;
        result[i*4+2] = (state[i] >> 8) & 0xFF;
        result[i*4+3] = state[i] & 0xFF;
    }
    
    return result;
}

// Double sha256 returning raw bytes (for signature hashes)
std::vector<uint8_t> double_sha256_raw(const std::vector<uint8_t>& data) {
    sha256 hasher;
    hasher.update(data.data(), data.size());
    std::vector<uint8_t> hash1 = hasher.finalize();

    hasher.reset();
    hasher.update(hash1.data(), hash1.size());
    return hasher.finalize();
}

std::vector<uint8_t> double_sha256_raw(const uint8_t* data, size_t len) {
    return double_sha256_raw(std::vector<uint8_t>(data, data + len));
}

// Double sha256 function (Bitcoin Core style - reversed for txid display)
std::string double_sha256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash2 = double_sha256_raw(data);

    // Convert to hex string (little-endian for Bitcoin txid display)
    std::string result;
    for (int i = 31; i >= 0; i--) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", hash2[i]);
        result += hex;
    }
    return result;
}

std::string double_sha256(const std::string& data) {
    return double_sha256(std::vector<uint8_t>(data.begin(), data.end()));
}

std::string double_sha256(const uint8_t* data, size_t len) {
    return double_sha256(std::vector<uint8_t>(data, data + len));
}

// Utility functions
std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::string reverse_hex(const std::string& hex) {
    std::string reversed;
    for (int i = hex.length() - 2; i >= 0; i -= 2) {
        reversed += hex.substr(i, 2);
    }
    return reversed;
}

} // namespace Common
} // namespace Dinero 