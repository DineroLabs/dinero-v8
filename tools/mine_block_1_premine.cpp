// BLOCK 1 PREMINE MINER
//
// Mines the first block after genesis containing the premine (2,627,900 DIN)
//
// This block:
// - References the canonical genesis block
// - Contains single coinbase transaction paying 2,627,900 DIN to P2TR address
// - Uses mainnet PoW difficulty
// - Height = 1

#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>

#include "consensus/chain_bundle_generated.h"

// Premine constants (from generate_premine_key output)
static const char* PREMINE_SCRIPTPUBKEY = "5120c2a63bf0587d7be826218adea70e91759f85b87ca0aa2adaa8e541e601fa0aa0";
static const char* PREMINE_ADDRESS = "din1pc2nrhuzc04a7sf3p3t02wr53wk0ctwru5z4z4k4gu4q7vq06p2sqyrrk3s";
static const uint64_t PREMINE_AMOUNT_DIN = 2627900;  // 2,627,900 DIN
static const uint64_t PREMINE_AMOUNT_UNA = PREMINE_AMOUNT_DIN * 100000000ULL;  // 262,790,000,000,000 una

// Genesis parameters come from the canonical generated bundle.
static const char* GENESIS_HASH = dinero::chain_bundle::GENESIS_BLOCK_HASH;
static const uint32_t POW_BITS = dinero::chain_bundle::GENESIS_DIFFICULTY;
static const uint64_t GENESIS_TIME = dinero::chain_bundle::GENESIS_TIMESTAMP;

// SHA256 implementation (same as genesis miner)
namespace sha256_impl {
    static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return z ^ (x & (y ^ z)); }
    static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (z & (x | y)); }
    static inline uint32_t Sigma0(uint32_t x) { return (x >> 2 | x << 30) ^ (x >> 13 | x << 19) ^ (x >> 22 | x << 10); }
    static inline uint32_t Sigma1(uint32_t x) { return (x >> 6 | x << 26) ^ (x >> 11 | x << 21) ^ (x >> 25 | x << 7); }
    static inline uint32_t sigma0(uint32_t x) { return (x >> 7 | x << 25) ^ (x >> 18 | x << 14) ^ (x >> 3); }
    static inline uint32_t sigma1(uint32_t x) { return (x >> 17 | x << 15) ^ (x >> 19 | x << 13) ^ (x >> 10); }

    static inline uint32_t ReadBE32(const uint8_t* ptr) {
        return uint32_t(ptr[0]) << 24 | uint32_t(ptr[1]) << 16 | uint32_t(ptr[2]) << 8 | uint32_t(ptr[3]);
    }

    static inline void WriteBE32(uint8_t* ptr, uint32_t x) {
        ptr[0] = x >> 24;
        ptr[1] = x >> 16;
        ptr[2] = x >> 8;
        ptr[3] = x;
    }

    static inline void WriteBE64(uint8_t* ptr, uint64_t x) {
        WriteBE32(ptr, x >> 32);
        WriteBE32(ptr + 4, x);
    }

    static void Transform(uint32_t* s, const uint8_t* chunk) {
        uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
        uint32_t w[64];

        for (int i = 0; i < 16; i++) {
            w[i] = ReadBE32(chunk + i * 4);
        }
        for (int i = 16; i < 64; i++) {
            w[i] = sigma1(w[i - 2]) + w[i - 7] + sigma0(w[i - 15]) + w[i - 16];
        }

        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + k[i] + w[i];
            uint32_t t2 = Sigma0(a) + Maj(a, b, c);
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }

        s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
    }
}

void sha256(const uint8_t* data, size_t len, uint8_t* hash) {
    uint32_t s[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    uint8_t buf[64];
    size_t bufsize = 0;
    uint64_t bytes = 0;

    while (len > 0) {
        size_t chunk_len = std::min(len, size_t(64 - bufsize));
        std::memcpy(buf + bufsize, data, chunk_len);
        bufsize += chunk_len;
        data += chunk_len;
        len -= chunk_len;
        bytes += chunk_len;

        if (bufsize == 64) {
            sha256_impl::Transform(s, buf);
            bufsize = 0;
        }
    }

    buf[bufsize++] = 0x80;
    if (bufsize > 56) {
        std::memset(buf + bufsize, 0, 64 - bufsize);
        sha256_impl::Transform(s, buf);
        bufsize = 0;
    }
    std::memset(buf + bufsize, 0, 56 - bufsize);
    sha256_impl::WriteBE64(buf + 56, bytes * 8);
    sha256_impl::Transform(s, buf);

    for (int i = 0; i < 8; i++) {
        sha256_impl::WriteBE32(hash + i * 4, s[i]);
    }
}

void sha256d(const uint8_t* data, size_t len, uint8_t* hash) {
    uint8_t tmp[32];
    sha256(data, len, tmp);
    sha256(tmp, 32, hash);
}

// Hex string to bytes
std::vector<uint8_t> hex_to_bytes(const char* hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < std::strlen(hex); i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2; j++) {
            byte <<= 4;
            char c = hex[i + j];
            if (c >= '0' && c <= '9') byte |= c - '0';
            else if (c >= 'a' && c <= 'f') byte |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') byte |= c - 'A' + 10;
        }
        bytes.push_back(byte);
    }
    return bytes;
}

// Reverse bytes (for hash endianness)
void reverse_bytes(uint8_t* data, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        std::swap(data[i], data[len - 1 - i]);
    }
}

// Write varint
void push_varint(std::vector<uint8_t>& v, uint64_t n) {
    if (n < 0xfd) {
        v.push_back((uint8_t)n);
    } else if (n <= 0xffff) {
        v.push_back(0xfd);
        v.push_back(n & 0xff);
        v.push_back((n >> 8) & 0xff);
    } else if (n <= 0xffffffff) {
        v.push_back(0xfe);
        v.push_back(n & 0xff);
        v.push_back((n >> 8) & 0xff);
        v.push_back((n >> 16) & 0xff);
        v.push_back((n >> 24) & 0xff);
    } else {
        v.push_back(0xff);
        for (int i = 0; i < 8; i++) {
            v.push_back((n >> (i * 8)) & 0xff);
        }
    }
}

// Write little-endian integer
void write_i32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

void write_i64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (v >> (i * 8)) & 0xff;
    }
}

// Check if hash satisfies PoW (hash < target)
bool satisfies_pow(const uint8_t* hash, uint32_t bits) {
    // Extract target from compact bits
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00ffffff;

    // Build target (256-bit big-endian)
    uint8_t target[32] = {0};
    if (exponent <= 3) {
        for (uint32_t i = 0; i < exponent; i++) {
            target[31 - i] = (mantissa >> (8 * i)) & 0xff;
        }
    } else {
        uint32_t offset = exponent - 3;
        for (uint32_t i = 0; i < 3 && offset + i < 32; i++) {
            target[31 - offset - i] = (mantissa >> (8 * (2 - i))) & 0xff;
        }
    }

    // Compare hash to target (both big-endian)
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return false;
}

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    BLOCK 1 PREMINE MINER                               ║\n";
    std::cout << "║                                                                        ║\n";
    std::cout << "║  Mining first block after genesis                                      ║\n";
    std::cout << "║  Premine: 2,627,900 DIN → P2TR address                                ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";

    // Parse genesis hash
    auto genesis_hash_bytes = hex_to_bytes(GENESIS_HASH);
    if (genesis_hash_bytes.size() != 32) {
        std::cerr << "❌ Invalid genesis hash\n";
        return 1;
    }
    reverse_bytes(genesis_hash_bytes.data(), 32);  // Convert to little-endian for block header

    // Parse premine scriptPubKey
    auto script_pubkey = hex_to_bytes(PREMINE_SCRIPTPUBKEY);

    // Build coinbase transaction
    std::vector<uint8_t> tx;

    // Version (4 bytes)
    uint8_t version[4] = {1, 0, 0, 0};
    tx.insert(tx.end(), version, version + 4);

    // Input count (1)
    tx.push_back(1);

    // Input 0: Coinbase
    // - Previous output (null hash + 0xffffffff)
    for (int i = 0; i < 32; i++) tx.push_back(0);
    for (int i = 0; i < 4; i++) tx.push_back(0xff);

    // - scriptSig: Height (BIP34) + arbitrary data
    std::vector<uint8_t> script_sig;
    script_sig.push_back(0x01);  // Push 1 byte (height)
    script_sig.push_back(0x01);  // Height = 1

    // Add message: "Premine: 2,627,900 DIN for development, marketing, ecosystem growth"
    const char* msg = "Premine: 2,627,900 DIN for development, marketing, ecosystem growth";
    script_sig.insert(script_sig.end(), msg, msg + std::strlen(msg));

    push_varint(tx, script_sig.size());
    tx.insert(tx.end(), script_sig.begin(), script_sig.end());

    // - Sequence (0xffffffff)
    for (int i = 0; i < 4; i++) tx.push_back(0xff);

    // Output count (1)
    tx.push_back(1);

    // Output 0: Premine to P2TR address
    uint8_t amt[8];
    write_i64_le(amt, PREMINE_AMOUNT_UNA);
    tx.insert(tx.end(), amt, amt + 8);

    push_varint(tx, script_pubkey.size());
    tx.insert(tx.end(), script_pubkey.begin(), script_pubkey.end());

    // Locktime (0)
    for (int i = 0; i < 4; i++) tx.push_back(0);

    // Compute TXID (merkle root for single-tx block)
    uint8_t merkle_root[32];
    sha256d(tx.data(), tx.size(), merkle_root);

    std::cout << "📝 Coinbase Transaction:\n";
    std::cout << "   Size: " << tx.size() << " bytes\n";
    std::cout << "   Outputs:\n";
    std::cout << "     [0] 2,627,900 DIN → " << PREMINE_ADDRESS << "\n";
    std::cout << "\n";

    std::cout << "🔨 Mining block 1...\n";
    std::cout << "   Target difficulty: 0x" << std::hex << POW_BITS << std::dec << "\n";
    std::cout << "   Previous block:    " << GENESIS_HASH << "\n";
    std::cout << "\n";

    // Build block header
    uint32_t nVersion = 1;
    uint32_t nTime = static_cast<uint32_t>(GENESIS_TIME + 120);  // Genesis time + 120 seconds
    uint32_t nBits = POW_BITS;
    uint32_t nNonce = 0;

    uint8_t header[80];
    write_i32_le(header + 0, nVersion);
    std::memcpy(header + 4, genesis_hash_bytes.data(), 32);
    std::memcpy(header + 36, merkle_root, 32);
    write_i32_le(header + 68, nTime);
    write_i32_le(header + 72, nBits);

    // Mine
    auto start = std::chrono::steady_clock::now();
    uint64_t hashes = 0;
    uint8_t hash[32];

    while (true) {
        write_i32_le(header + 76, nNonce);

        sha256d(header, 80, hash);
        hashes++;

        if (satisfies_pow(hash, nBits)) {
            break;
        }

        if (hashes % 100000 == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_sec = std::chrono::duration<double>(now - start).count();
            double hashrate = hashes / elapsed_sec / 1000.0;
            std::cout << "   Hashes: " << hashes << " | Hashrate: " << std::fixed << std::setprecision(2) << hashrate << " kH/s | Nonce: " << nNonce << "\r" << std::flush;
        }

        nNonce++;
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();

    std::cout << "\n\n✅ Block 1 mined!\n\n";

    // Reverse hash for display (big-endian)
    reverse_bytes(hash, 32);

    std::cout << "Block Hash: ";
    for (int i = 0; i < 32; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    std::cout << std::dec << "\n";

    std::cout << "Merkle Root: ";
    for (int i = 0; i < 32; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)merkle_root[i];
    }
    std::cout << std::dec << "\n";

    std::cout << "Nonce: " << nNonce << "\n";
    std::cout << "Time: " << nTime << "\n";
    std::cout << "Mining time: " << std::fixed << std::setprecision(2) << elapsed << " seconds\n";
    std::cout << "Total hashes: " << hashes << "\n";
    std::cout << "\n";

    std::cout << "Coinbase Hex:\n";
    for (size_t i = 0; i < tx.size(); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)tx[i];
        if ((i + 1) % 32 == 0) std::cout << "\n";
    }
    if (tx.size() % 32 != 0) std::cout << "\n";
    std::cout << std::dec;

    return 0;
}
