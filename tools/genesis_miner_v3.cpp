// tools/genesis_miner_v3.cpp
// Dinero BlockHeader v1 genesis miner (128-byte headers, Phase 3)
//
// CRITICAL: This miner produces BlockHeader v1 (128 bytes) with:
//   - 12-byte reserved field (all zeros)
//   - Exact layout matching primitives/block.h
//   - Field names: difficulty (not bits), timestamp (not time), utreexo_root
//   - Explicit assertions to prevent 112-byte regression
//
// Motto (preserved exactly): "Dinero: Real Money For Free People"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <cassert>

#include "crypto/sha256.h"

// ============================================================================
// BLOCKHEADER V1 CONSTANTS (Phase 3: FROZEN)
// ============================================================================

constexpr size_t BLOCKHEADER_V1_SIZE = 128;  // Must match primitives/block.h

// Compile-time verification (preflight check #1)
static_assert(BLOCKHEADER_V1_SIZE == 128, "BlockHeader v1 MUST be 128 bytes");

// ============================================================================
// UTILITIES
// ============================================================================

static inline void write_u32_le(uint8_t* p, uint32_t x) {
    p[0] = (uint8_t)(x);
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static inline void write_u64_le(uint8_t* p, uint64_t x) {
    for (int i=0; i<8; ++i) p[i] = (uint8_t)(x >> (8*i));
}

static inline void write_i64_le(uint8_t* p, int64_t x) {
    for (int i=0; i<8; ++i) p[i] = (uint8_t)(x >> (8*i));
}

static inline std::string hex(const uint8_t* b, size_t n) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i=0; i<n; ++i)
        os << std::setw(2) << (unsigned)b[i];
    return os.str();
}

static inline std::string hex_le(const uint8_t* b, size_t n) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i=0; i<n; ++i)
        os << std::setw(2) << (unsigned)b[n-1-i];
    return os.str();
}

static inline std::vector<uint8_t> from_hex(const std::string& h) {
    std::vector<uint8_t> out;
    if (h.size() % 2) return out;
    for (size_t i=0; i<h.size(); i+=2) {
        unsigned v=0;
        std::stringstream ss;
        ss << std::hex << h.substr(i,2);
        ss >> v;
        out.push_back((uint8_t)v);
    }
    return out;
}

static inline void push_varint(std::vector<uint8_t>& out, uint64_t n) {
    if (n < 0xfd) {
        out.push_back((uint8_t)n);
    } else if (n <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(n & 0xff);
        out.push_back((n >> 8) & 0xff);
    } else if (n <= 0xffffffffULL) {
        out.push_back(0xfe);
        for(int i=0; i<4; ++i) out.push_back((uint8_t)(n >> (8*i)));
    } else {
        out.push_back(0xff);
        for(int i=0; i<8; ++i) out.push_back((uint8_t)(n >> (8*i)));
    }
}

static inline void sha256d(const uint8_t* data, size_t n, uint8_t out32[32]) {
    using dinero::crypto::CSHA256;
    CSHA256 h1;
    h1.Write(data, n);
    uint8_t tmp[32];
    h1.Finalize(tmp);

    CSHA256 h2;
    h2.Write(tmp, 32);
    h2.Finalize(out32);
}

// ============================================================================
// GENESIS COINBASE TRANSACTION (PRESERVED EXACTLY FROM V2)
// ============================================================================

static std::string build_genesis_coinbase_v3(const std::string& motto) {
    std::vector<uint8_t> tx;

    // Version (4 bytes)
    tx.resize(4);
    write_u32_le(tx.data(), 1);

    // Input count (1)
    push_varint(tx, 1);

    // Input 0: Coinbase
    // prev_txid: 32 bytes of 0x00
    tx.insert(tx.end(), 32, 0x00);

    // prev_vout: 0xffffffff
    uint8_t vout[4];
    write_u32_le(vout, 0xffffffff);
    tx.insert(tx.end(), vout, vout+4);

    // scriptSig: <height> <motto>
    std::vector<uint8_t> scriptSig;

    // Height 0 (1 byte: 0x00)
    scriptSig.push_back(0x00);

    // Motto bytes (54 bytes)
    std::vector<uint8_t> motto_bytes(motto.begin(), motto.end());
    scriptSig.insert(scriptSig.end(), motto_bytes.begin(), motto_bytes.end());

    // Push scriptSig with varint length
    push_varint(tx, scriptSig.size());
    tx.insert(tx.end(), scriptSig.begin(), scriptSig.end());

    // Sequence: 0xffffffff
    uint8_t seq[4];
    write_u32_le(seq, 0xffffffff);
    tx.insert(tx.end(), seq, seq+4);

    // Output count (1)
    push_varint(tx, 1);

    // Output 0: 100 DIN burn via OP_RETURN (with motto for double commitment)
    // Amount: 10,000,000,000 una (100 DIN)
    int64_t amount = 10000000000LL;
    uint8_t amt[8];
    write_i64_le(amt, amount);
    tx.insert(tx.end(), amt, amt+8);

    // scriptPubKey: OP_RETURN <motto bytes> (DOUBLE COMMITMENT)
    // This commits the motto to the merkle root (in addition to scriptSig)
    std::vector<uint8_t> spk;
    spk.push_back(0x6a);  // OP_RETURN
    spk.push_back((uint8_t)motto_bytes.size());  // Push motto length
    spk.insert(spk.end(), motto_bytes.begin(), motto_bytes.end());  // Motto bytes

    push_varint(tx, spk.size());
    tx.insert(tx.end(), spk.begin(), spk.end());

    // Locktime (4 bytes: 0)
    uint8_t locktime[4] = {0, 0, 0, 0};
    tx.insert(tx.end(), locktime, locktime+4);

    return hex(tx.data(), tx.size());
}

// ============================================================================
// MERKLE ROOT CALCULATION
// ============================================================================

static std::string merkle_root_from_coinbase_hex(const std::string& coinbase_hex) {
    auto raw = from_hex(coinbase_hex);

    // Hash coinbase tx
    uint8_t txid[32];
    sha256d(raw.data(), raw.size(), txid);

    // For single transaction, merkle root = txid (reversed to big-endian)
    return hex_le(txid, 32);
}

// ============================================================================
// BLOCKHEADER V1 SERIALIZATION (128 BYTES - PHASE 3)
// ============================================================================
//
// Layout (FROZEN):
//   Offset 0x00 (4 bytes):   version
//   Offset 0x04 (32 bytes):  prev_block_hash
//   Offset 0x24 (32 bytes):  merkle_root
//   Offset 0x44 (32 bytes):  utreexo_root
//   Offset 0x64 (8 bytes):   timestamp
//   Offset 0x6C (4 bytes):   difficulty
//   Offset 0x70 (4 bytes):   nonce
//   Offset 0x74 (12 bytes):  reserved (MUST be zero)
// ============================================================================

static std::vector<uint8_t> serialize_blockheader_v1(
    uint32_t version,
    const std::string& prev_block_hash_hex,
    const std::string& merkle_root_hex,
    const std::string& utreexo_root_hex,
    uint64_t timestamp,
    uint32_t difficulty,
    uint32_t nonce
) {
    std::vector<uint8_t> out;
    out.resize(BLOCKHEADER_V1_SIZE);  // 128 bytes

    // Preflight check #8: Runtime assertion
    assert(out.size() == 128 && "BlockHeader v1 MUST be 128 bytes");

    // Zero-initialize (ensures reserved field is zero)
    std::memset(out.data(), 0, BLOCKHEADER_V1_SIZE);

    // version (4 bytes, offset 0x00)
    write_u32_le(out.data() + 0x00, version);

    // prev_block_hash (32 bytes, offset 0x04, little-endian)
    auto prev = from_hex(prev_block_hash_hex);
    if (prev.size() != 32) throw std::runtime_error("prev_block_hash must be 32 bytes");
    // Genesis: all zeros, no reversal needed
    for (int i=0; i<32; ++i) out[0x04 + i] = prev[i];

    // merkle_root (32 bytes, offset 0x24, little-endian)
    auto mr = from_hex(merkle_root_hex);
    if (mr.size() != 32) throw std::runtime_error("merkle_root must be 32 bytes");
    // Reverse from big-endian display to little-endian storage
    for (int i=0; i<32; ++i) out[0x24 + i] = mr[31-i];

    // utreexo_root (32 bytes, offset 0x44, little-endian)
    auto utx = from_hex(utreexo_root_hex);
    if (utx.size() != 32) throw std::runtime_error("utreexo_root must be 32 bytes");
    // Genesis: all zeros, no reversal needed
    for (int i=0; i<32; ++i) out[0x44 + i] = utx[i];

    // timestamp (8 bytes, offset 0x64, little-endian)
    write_u64_le(out.data() + 0x64, timestamp);

    // difficulty (4 bytes, offset 0x6C, little-endian)
    write_u32_le(out.data() + 0x6C, difficulty);

    // nonce (4 bytes, offset 0x70, little-endian)
    write_u32_le(out.data() + 0x70, nonce);

    // reserved (12 bytes, offset 0x74, MUST be zero)
    // Already zeroed by memset above (preflight check #3)

    // Preflight check #3: Verify reserved field is zero
    for (int i = 0; i < 12; i++) {
        assert(out[0x74 + i] == 0 && "Reserved field MUST be all zeros");
    }

    return out;
}

// ============================================================================
// MINER
// ============================================================================

static inline void compact_to_target(uint32_t bits, uint8_t out[32]) {
    std::memset(out, 0, 32);
    uint32_t exp = bits >> 24;
    uint32_t mant = bits & 0x00ffffff;

    if (exp <= 3) {
        uint32_t r = mant >> (8*(3-exp));
        for (int i=0; i<4 && r; ++i) {
            out[31 - i] = (uint8_t)(r & 0xff);
            r >>= 8;
        }
    } else {
        int idx = 32 - exp;
        if (idx < 0) {
            std::memset(out, 0xff, 32);
            return;
        }
        if (idx <= 29) {
            out[idx]     = (uint8_t)((mant >> 16) & 0xff);
            out[idx + 1] = (uint8_t)((mant >>  8) & 0xff);
            out[idx + 2] = (uint8_t)((mant      ) & 0xff);
        }
    }
}

static inline bool leq_256_be(const uint8_t a[32], const uint8_t b[32]) {
    for (int i=0; i<32; ++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return true;
}

struct FoundResult {
    bool found = false;
    uint32_t nonce = 0;
    std::string genesis_hash_le;
    std::string header_hex;
};

static void mine_worker(
    uint32_t start, uint32_t end,
    uint32_t version,
    const std::string& merkle_root_hex,
    uint64_t timestamp,
    uint32_t difficulty,
    const std::string& utreexo_root_hex,
    const uint8_t target_be[32],
    std::atomic<bool>& stop,
    FoundResult& out,
    std::mutex& out_mu
) {
    const std::string prev_block_hash_hex(64, '0');  // Genesis: all zeros
    uint8_t h32[32];

    for (uint32_t nonce = start; nonce != end && !stop.load(); ++nonce) {
        // Serialize BlockHeader v1 (128 bytes)
        auto header = serialize_blockheader_v1(
            version,
            prev_block_hash_hex,
            merkle_root_hex,
            utreexo_root_hex,
            timestamp,
            difficulty,
            nonce
        );

        // Preflight check #1 & #2: Verify we're hashing exactly 128 bytes
        assert(header.size() == 128 && "CRITICAL: Must hash exactly 128 bytes");

        // Double SHA-256 of full 128-byte header
        sha256d(header.data(), header.size(), h32);

        // Convert to big-endian for comparison
        uint8_t h_be[32];
        for (int i=0; i<32; ++i) h_be[i] = h32[31-i];

        if (leq_256_be(h_be, target_be)) {
            std::lock_guard<std::mutex> lk(out_mu);
            if (!out.found) {
                out.found = true;
                out.nonce = nonce;
                out.header_hex = hex(header.data(), header.size());
                out.genesis_hash_le = hex_le(h32, 32);
                stop.store(true);
            }
            return;
        }

        if ((nonce & 0xfffff) == 0) {
            std::printf(".");
            std::fflush(stdout);
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [--threads <N>] [--output <file.json>]\n";
        std::cerr << "\n";
        std::cerr << "Genesis parameters are hardcoded (Phase 3 frozen values):\n";
        std::cerr << "  Timestamp:  1772496000 (2026-03-03 00:00:00 UTC)\n";
        std::cerr << "  Difficulty: 0x1d31ffce\n";
        std::cerr << "  Motto:      \"Dinero: Real Money For Free People\"\n";
        return 1;
    }

    // ========================================================================
    // FROZEN GENESIS PARAMETERS (Phase 3)
    // ========================================================================
    const uint32_t version = 1;
    const uint64_t timestamp = 1772496000;  // 2026-03-03 00:00:00 UTC (FROZEN)
    const uint32_t difficulty = 0x1d31ffce;  // Phase 3.1 unified difficulty (50x easier than Bitcoin)
    const std::string motto = "Dinero: Real Money For Free People";  // FROZEN
    const std::string utreexo_root_hex(64, '0');  // All zeros for genesis (FROZEN)

    unsigned threads = std::thread::hardware_concurrency();
    std::string output_file = "genesis_blockheader_v1.json";

    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i+1 < argc) {
            threads = std::stoul(argv[++i]);
        } else if (a == "--output" && i+1 < argc) {
            output_file = argv[++i];
        }
    }

    std::printf("\n");
    std::printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  DINERO BLOCKHEADER V1 GENESIS MINER (PHASE 3)                    ║\n");
    std::printf("╚═══════════════════════════════════════════════════════════════════╝\n");
    std::printf("\n");
    std::printf("  Header Format:  BlockHeader v1 (128 bytes, FROZEN)\n");
    std::printf("  Version:        %u\n", version);
    std::printf("  Timestamp:      %lu (2026-03-03 00:00:00 UTC)\n", timestamp);
    std::printf("  Difficulty:     0x%08x\n", difficulty);
    std::printf("  Threads:        %u\n", threads);
    std::printf("  Motto:          %s\n", motto.c_str());
    std::printf("  Utreexo Root:   %s\n", utreexo_root_hex.c_str());
    std::printf("\n");
    std::printf("  ⚠️  PREFLIGHT CHECKS:\n");
    std::printf("      ✓ BlockHeader v1 size: %zu bytes (compile-time verified)\n", BLOCKHEADER_V1_SIZE);
    std::printf("      ✓ Reserved field: 12 bytes (will be zeroed)\n");
    std::printf("      ✓ Hash input: Full 128 bytes (runtime verified)\n");
    std::printf("\n");

    // Build coinbase
    std::printf("  [1/4] Building genesis coinbase...\n");
    std::string coinbase_hex = build_genesis_coinbase_v3(motto);
    std::printf("        Coinbase: %zu bytes\n", coinbase_hex.length() / 2);

    // Calculate merkle root
    std::printf("  [2/4] Computing merkle root...\n");
    std::string merkle_hex = merkle_root_from_coinbase_hex(coinbase_hex);
    std::printf("        Merkle:   %s\n", merkle_hex.c_str());

    // Compute target
    uint8_t target_be[32];
    compact_to_target(difficulty, target_be);

    // Mine
    std::printf("  [3/4] Mining genesis block (BlockHeader v1)...\n");
    std::printf("        ");
    std::fflush(stdout);

    auto start_time = std::chrono::steady_clock::now();

    std::atomic<bool> stop{false};
    FoundResult result;
    std::mutex result_mu;

    std::vector<std::thread> workers;
    uint32_t range = 0xffffffffu / threads;

    for (unsigned t=0; t<threads; ++t) {
        uint32_t start = t * range;
        uint32_t end = (t == threads-1) ? 0xffffffffu : (start + range);

        workers.emplace_back(mine_worker,
            start, end, version, merkle_hex, timestamp, difficulty, utreexo_root_hex,
            target_be, std::ref(stop), std::ref(result), std::ref(result_mu)
        );
    }

    for (auto& w : workers) w.join();

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;

    std::printf("\n\n");

    if (!result.found) {
        std::printf("  ❌ Genesis block not found\n\n");
        return 1;
    }

    // Success!
    std::printf("  [4/4] Genesis block found!\n\n");
    std::printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  DINERO GENESIS BLOCK - BLOCKHEADER V1                            ║\n");
    std::printf("╚═══════════════════════════════════════════════════════════════════╝\n");
    std::printf("\n");
    std::printf("  Genesis Hash:   %s\n", result.genesis_hash_le.c_str());
    std::printf("  Merkle Root:    %s\n", merkle_hex.c_str());
    std::printf("  Version:        %u\n", version);
    std::printf("  Timestamp:      %lu (2026-03-03 00:00:00 UTC)\n", timestamp);
    std::printf("  Difficulty:     0x%08x\n", difficulty);
    std::printf("  Nonce:          %u\n", result.nonce);
    std::printf("  Utreexo Root:   %s\n", utreexo_root_hex.c_str());
    std::printf("  Reserved:       [12 bytes, all zeros]\n");
    std::printf("\n");
    std::printf("  Header Size:    %zu bytes (BlockHeader v1)\n", result.header_hex.length() / 2);
    std::printf("  Coinbase:       100 DIN burned (OP_RETURN) - NO PREMINE\n");
    std::printf("  Motto:          %s\n", motto.c_str());
    std::printf("  Commitment:     scriptSig + OP_RETURN (double commitment)\n");
    std::printf("\n");
    std::printf("  Elapsed:        %.2f seconds\n", elapsed);
    std::printf("\n");

    // Verification
    std::printf("  ✅ VERIFICATION:\n");
    std::printf("      Header size: %zu bytes (must be 128)\n", result.header_hex.length() / 2);
    if (result.header_hex.length() / 2 != 128) {
        std::printf("      ❌ ERROR: Header is not 128 bytes!\n");
        return 1;
    }
    std::printf("      ✓ BlockHeader v1 format verified\n");
    std::printf("\n");

    // Save to JSON
    std::ofstream out(output_file);
    out << "{\n";
    out << "  \"network\": \"mainnet\",\n";
    out << "  \"protocol_version\": \"3.0.0\",\n";
    out << "  \"blockheader_version\": \"v1\",\n";
    out << "  \"header_size_bytes\": " << (result.header_hex.length() / 2) << ",\n";
    out << "  \"genesis_hash\": \"" << result.genesis_hash_le << "\",\n";
    out << "  \"merkle_root\": \"" << merkle_hex << "\",\n";
    out << "  \"version\": " << version << ",\n";
    out << "  \"timestamp\": " << timestamp << ",\n";
    out << "  \"difficulty\": \"0x" << std::hex << std::setfill('0') << std::setw(8) << difficulty << "\",\n";
    out << "  \"nonce\": " << std::dec << result.nonce << ",\n";
    out << "  \"utreexo_root\": \"" << utreexo_root_hex << "\",\n";
    out << "  \"reserved\": \"000000000000000000000000\",\n";
    out << "  \"motto\": \"" << motto << "\",\n";
    out << "  \"coinbase_hex\": \"" << coinbase_hex << "\",\n";
    out << "  \"header_hex_128\": \"" << result.header_hex << "\"\n";
    out << "}\n";
    out.close();

    std::printf("  ✅ Saved to: %s\n\n", output_file.c_str());

    return 0;
}
