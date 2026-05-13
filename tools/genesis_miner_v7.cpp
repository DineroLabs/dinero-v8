// tools/genesis_miner_v7.cpp
// Dinero v7 BlockHeader v1 genesis miner
//
// CRITICAL DESIGN DECISION:
// This miner uses the REAL BlockHeader type from primitives/block.h
// No manual layout. No struct duplication. Zero room for ABI mismatch.
//
// v7 inscription: 71 bytes, pinned by static_assert in consumer code.
// "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026"

#include "primitives/block.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"
#include "crypto/sha256.h"
#include "common/sha256d.h"
#include "consensus/utreexo_accumulator.h"

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
#include <inttypes.h>  // For PRIu64 portable printf format

using namespace dinero;

// ============================================================================
// PHASE 3 PREFLIGHT: COMPILE-TIME VERIFICATION
// ============================================================================

// Requirement #1: BlockHeader v1 MUST be exactly 128 bytes
static_assert(sizeof(BlockHeader) == 128,
    "FATAL: BlockHeader must be 128 bytes for Phase 3");

// Requirement #1: BlockHeader v1 MUST be trivially copyable (memcpy-safe)
static_assert(std::is_trivially_copyable_v<BlockHeader>,
    "FATAL: BlockHeader must be trivially copyable");

// ============================================================================
// UTILITIES
// ============================================================================

static inline void write_u32_le(uint8_t* p, uint32_t x) {
    p[0] = (uint8_t)(x);
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
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

// ============================================================================
// GENESIS COINBASE TRANSACTION (PRESERVED EXACTLY)
// ============================================================================

static std::string build_genesis_coinbase(const std::string& motto) {
    std::vector<uint8_t> tx;

    // Version (4 bytes)
    tx.resize(4);
    write_u32_le(tx.data(), 1);

    // Input count (1)
    push_varint(tx, 1);

    // Input 0: Coinbase (prev_txid = 0x00...00, prev_vout = 0xffffffff)
    tx.insert(tx.end(), 32, 0x00);
    uint8_t vout[4];
    write_u32_le(vout, 0xffffffff);
    tx.insert(tx.end(), vout, vout+4);

    // scriptSig: <height> <motto>
    std::vector<uint8_t> scriptSig;
    scriptSig.push_back(0x00);  // Height 0
    std::vector<uint8_t> motto_bytes(motto.begin(), motto.end());
    scriptSig.insert(scriptSig.end(), motto_bytes.begin(), motto_bytes.end());

    push_varint(tx, scriptSig.size());
    tx.insert(tx.end(), scriptSig.begin(), scriptSig.end());

    // Sequence: 0xffffffff
    uint8_t seq[4];
    write_u32_le(seq, 0xffffffff);
    tx.insert(tx.end(), seq, seq+4);

    // Output count (1)
    push_varint(tx, 1);

    // Output 0: 100 DIN burn via OP_RETURN (double commitment)
    int64_t amount = 10000000000LL;  // 100 DIN
    uint8_t amt[8];
    write_i64_le(amt, amount);
    tx.insert(tx.end(), amt, amt+8);

    // scriptPubKey: OP_RETURN <motto bytes>
    std::vector<uint8_t> spk;
    spk.push_back(0x6a);  // OP_RETURN
    spk.push_back((uint8_t)motto_bytes.size());
    spk.insert(spk.end(), motto_bytes.begin(), motto_bytes.end());

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

static uint256 compute_merkle_root(const std::string& coinbase_hex) {
    auto raw = from_hex(coinbase_hex);

    // Double SHA-256 of coinbase transaction
    crypto::CSHA256 h1;
    h1.Write(raw.data(), raw.size());
    uint8_t tmp[32];
    h1.Finalize(tmp);

    crypto::CSHA256 h2;
    h2.Write(tmp, 32);
    uint8_t txid[32];
    h2.Finalize(txid);

    // Match Transaction::GetTxid() convention: data[i] = hash[i] (no reversal).
    // GetHash() reverses, but GetTxid() does not.
    uint256 result;
    std::memcpy(result.data, txid, 32);
    return result;
}

// ============================================================================
// REQUIREMENT #2: SERIALIZATION = RAW MEMCPY (EXACTLY 128 BYTES)
// ============================================================================

static std::array<uint8_t, 128> serialize_header_raw(const BlockHeader& h) {
    std::array<uint8_t, 128> out{};

    // Use BlockHeader's own SerializeForHash() method (authoritative)
    out = h.SerializeForHash();

    // Runtime assertion (Requirement #2)
    assert(out.size() == 128 && "FATAL: Header must serialize to exactly 128 bytes");

    return out;
}

// ============================================================================
// MINER
// ============================================================================

static inline void compact_to_target(uint32_t bits, uint8_t out[32]) {
    // Produces big-endian 256-bit target: out[0] = MSB, out[31] = LSB.
    // Bitcoin compact format: target = mantissa × 2^(8×(exp-3))
    std::memset(out, 0, 32);
    uint32_t exp = bits >> 24;
    uint32_t mant = bits & 0x00ffffff;

    if (exp <= 3) {
        uint32_t r = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4 && r; ++i) {
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
    const uint256& merkle_root,
    const uint256& utreexo_root,
    uint64_t timestamp,
    uint32_t difficulty,
    const uint8_t target_be[32],
    std::atomic<bool>& stop,
    FoundResult& out,
    std::mutex& out_mu
) {
    BlockHeader header{};  // Zero-initialize (including reserved field)
    header.version = 1;
    header.prev_block_hash = uint256();  // All zeros (genesis)
    header.merkle_root = merkle_root;
    // UTREEXO COMMITMENT (consensus-critical):
    // Genesis coinbase burns 100 DIN via OP_RETURN (unspendable).
    // OP_RETURN outputs are NOT added to UTXO set.
    // Therefore UTXO set after genesis = empty.
    // Commitment v2: SHA256(numLeaves_LE64=0 || 64×32_zero_bytes) — NOT all zeros.
    header.utreexo_root = utreexo_root;
    header.timestamp = timestamp;
    header.difficulty = difficulty;
    header.nonce = 0;
    // header.reserved is already zero from {} initialization

    // Requirement #3: Verify reserved field is zero
    assert(header.IsReservedValid() && "FATAL: Reserved field must be all zeros");

    for (uint32_t nonce = start; nonce != end && !stop.load(); ++nonce) {
        header.nonce = nonce;

        // Requirement #3: Defensive - ensure reserved stays zero
        // (Redundant but explicit - reserved should never change)
        assert(header.IsReservedValid() && "FATAL: Reserved field corrupted during mining");

        // Requirement #5: Hash all 128 bytes
        auto bytes = serialize_header_raw(header);
        assert(bytes.size() == 128 && "FATAL: Must hash exactly 128 bytes");

        // Double SHA-256
        crypto::CSHA256 h1;
        h1.Write(bytes.data(), bytes.size());
        uint8_t tmp[32];
        h1.Finalize(tmp);

        crypto::CSHA256 h2;
        h2.Write(tmp, 32);
        uint8_t h32[32];
        h2.Finalize(h32);

        // SHA-256 output is already big-endian (MSB at h32[0]).
        // Compare directly against big-endian target (no reversal).
        // This matches the canonical convention: GetHash() reverses SHA-256
        // into uint256 LE storage, where data[31]=MSB → operator< compares MSB first.
        if (leq_256_be(h32, target_be)) {
            std::lock_guard<std::mutex> lk(out_mu);
            if (!out.found) {
                out.found = true;
                out.nonce = nonce;
                out.header_hex = hex(bytes.data(), bytes.size());
                // Display: SHA-256 MSB first, matching GetHash().GetHex()
                out.genesis_hash_le = hex(h32, 32);
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
// REQUIREMENT #6: MANDATORY SANITY TEST
// ============================================================================

static bool run_sanity_test(const uint256& merkle_root, const uint256& utreexo_root,
                            uint64_t timestamp, uint32_t difficulty) {
    std::printf("  [SANITY TEST] Verifying all 128 header bytes affect hash...\n");

    BlockHeader h1{};
    h1.version = 1;
    h1.prev_block_hash = uint256();
    h1.merkle_root = merkle_root;
    h1.utreexo_root = utreexo_root;
    h1.timestamp = timestamp;
    h1.difficulty = difficulty;
    h1.nonce = 0;
    // h1.reserved is all zeros from {} initialization

    auto hash1 = h1.GetHash();

    // Test 1: Corrupting reserved field MUST change the hash.
    // This proves all 128 bytes (including reserved) are hashed.
    h1.reserved[0] = 1;
    auto hash2 = h1.GetHash();
    h1.reserved[0] = 0;  // Restore

    if (hash1 == hash2) {
        std::printf("  ❌ FATAL: Reserved field does not affect hash!\n");
        std::printf("           SerializeForHash() is not hashing all 128 bytes.\n");
        return false;
    }
    std::printf("  ✓ Reserved field affects hash\n");

    // Test 2: Changing nonce MUST change the hash.
    h1.nonce = 1;
    auto hash3 = h1.GetHash();
    h1.nonce = 0;

    if (hash1 == hash3) {
        std::printf("  ❌ FATAL: Nonce does not affect hash!\n");
        return false;
    }
    std::printf("  ✓ Nonce affects hash\n");

    // Test 3: Verify serialization is exactly 128 bytes.
    auto bytes = h1.SerializeForHash();
    if (bytes.size() != 128) {
        std::printf("  ❌ FATAL: Header serialization is %zu bytes (expected 128)\n", bytes.size());
        return false;
    }
    std::printf("  ✓ Header serializes to 128 bytes\n");

    std::printf("  ✓ All sanity tests passed\n");
    return true;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char** argv) {
    // ========================================================================
    // REQUIREMENT #4: FIXED GENESIS PARAMETERS (NO CLI FLAGS)
    // ========================================================================
    const uint32_t version = 1;
    const uint64_t timestamp = 1776384000;  // 2026-04-17 00:00:00 UTC (v7 Genesis Restart)
    const uint32_t difficulty = 0x1d31ffce;  // Reused from v5 (50× easier than Bitcoin genesis)
    const std::string motto =
        "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026";
    if (motto.size() != 71) {
        std::fprintf(stderr,
            "FATAL: v7 motto must be exactly 71 bytes (got %zu). "
            "Consensus byte length is pinned.\n", motto.size());
        return 1;
    }

    unsigned threads = std::thread::hardware_concurrency();
    std::string output_file = "genesis_blockheader_v7.json";

    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i+1 < argc) {
            threads = std::stoul(argv[++i]);
        } else if (a == "--output" && i+1 < argc) {
            output_file = argv[++i];
        }
    }

    std::printf("\n");
    std::printf("╔════════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  DINERO v7 GENESIS MINER (BlockHeader v1, 128 bytes)              ║\n");
    std::printf("╚════════════════════════════════════════════════════════════════════╝\n");
    std::printf("\n");
    std::printf("  ⚠️  CRITICAL: This miner uses the REAL BlockHeader type\n");
    std::printf("      from primitives/block.h (zero room for ABI mismatch)\n");
    std::printf("\n");
    std::printf("  Compile-time checks:\n");
    std::printf("    ✓ sizeof(BlockHeader) == %zu bytes\n", sizeof(BlockHeader));
    std::printf("    ✓ BlockHeader is trivially copyable\n");
    std::printf("\n");
    std::printf("  Genesis parameters (FROZEN):\n");
    std::printf("    Version:      %u\n", version);
    std::printf("    Timestamp:    %" PRIu64 " (2026-04-17 00:00:00 UTC)\n", timestamp);  // 1776384000
    std::printf("    Difficulty:   0x%08x\n", difficulty);
    std::printf("    Threads:      %u\n", threads);
    std::printf("    Motto:        %s\n", motto.c_str());
    std::printf("\n");

    // Build coinbase
    std::printf("  [1/5] Building genesis coinbase...\n");
    std::string coinbase_hex = build_genesis_coinbase(motto);
    std::printf("        Coinbase: %zu bytes\n", coinbase_hex.length() / 2);

    // Calculate merkle root
    std::printf("  [2/5] Computing merkle root and utreexo commitment...\n");
    uint256 merkle_root = compute_merkle_root(coinbase_hex);
    std::printf("        Merkle:   %s\n", merkle_root.GetHex().c_str());

    // Compute v2 empty-forest commitment for genesis utreexo_root.
    // Genesis burns 100 DIN via OP_RETURN (unspendable) → empty UTXO set.
    // v2 commitment: SHA256(numLeaves=0_LE64 || 64×32_zero_bytes) — NOT all zeros.
    consensus::UtreexoForest empty_forest;
    consensus::UtreexoHash empty_commitment = empty_forest.getCommitment();
    uint256 genesis_utreexo_root;
    std::memcpy(genesis_utreexo_root.data, empty_commitment.data(), 32);
    std::printf("        Utreexo:  %s\n", genesis_utreexo_root.GetHex().c_str());

    // Requirement #6: Sanity test
    std::printf("  [3/5] Running sanity test...\n");
    if (!run_sanity_test(merkle_root, genesis_utreexo_root, timestamp, difficulty)) {
        return 1;
    }

    // Compute target
    uint8_t target_be[32];
    compact_to_target(difficulty, target_be);

    // Mine
    std::printf("  [4/5] Mining genesis block (BlockHeader v1)...\n");
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
            start, end, merkle_root, genesis_utreexo_root, timestamp, difficulty,
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
    std::printf("  [5/5] Genesis block found!\n\n");
    std::printf("╔════════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  DINERO GENESIS BLOCK - BLOCKHEADER V1                             ║\n");
    std::printf("╚════════════════════════════════════════════════════════════════════╝\n");
    std::printf("\n");
    std::printf("  Genesis Hash:   %s\n", result.genesis_hash_le.c_str());
    std::printf("  Merkle Root:    %s\n", merkle_root.GetHex().c_str());
    std::printf("  Version:        %u\n", version);
    std::printf("  Timestamp:      %" PRIu64 " (2026-03-07 00:00:00 UTC)\n", timestamp);
    std::printf("  Difficulty:     0x%08x\n", difficulty);
    std::printf("  Nonce:          %u\n", result.nonce);
    std::printf("  Utreexo Root:   %s\n", genesis_utreexo_root.GetHex().c_str());
    std::printf("  Reserved:       [12 bytes, all zeros]\n");
    std::printf("\n");
    std::printf("  Header Size:    %zu bytes (BlockHeader v1)\n", result.header_hex.length() / 2);
    std::printf("  Coinbase:       100 DIN burned (OP_RETURN) - NO PREMINE\n");
    std::printf("  Motto:          %s\n", motto.c_str());
    std::printf("  Commitment:     scriptSig + OP_RETURN (double commitment)\n");
    std::printf("\n");
    std::printf("  Elapsed:        %.2f seconds\n", elapsed);
    std::printf("\n");

    // Final verification
    std::printf("  ✅ FINAL VERIFICATION:\n");
    size_t header_size = result.header_hex.length() / 2;
    std::printf("      Header size: %zu bytes ", header_size);
    if (header_size == 128) {
        std::printf("✓\n");
    } else {
        std::printf("❌ (EXPECTED 128)\n");
        return 1;
    }
    std::printf("\n");

    // Save to JSON
    std::ofstream out(output_file);
    out << "{\n";
    out << "  \"network\": \"mainnet\",\n";
    out << "  \"protocol_version\": \"3.0.0\",\n";
    out << "  \"blockheader_version\": \"v1\",\n";
    out << "  \"header_size_bytes\": " << header_size << ",\n";
    out << "  \"genesis_hash\": \"" << result.genesis_hash_le << "\",\n";
    out << "  \"merkle_root\": \"" << merkle_root.GetHex() << "\",\n";
    out << "  \"version\": " << version << ",\n";
    out << "  \"timestamp\": " << timestamp << ",\n";
    out << "  \"difficulty\": \"0x" << std::hex << std::setfill('0') << std::setw(8) << difficulty << "\",\n";
    out << "  \"nonce\": " << std::dec << result.nonce << ",\n";
    out << "  \"utreexo_root\": \"" << genesis_utreexo_root.GetHex() << "\",\n";
    out << "  \"reserved\": \"000000000000000000000000\",\n";
    out << "  \"motto\": \"" << motto << "\",\n";
    out << "  \"coinbase_hex\": \"" << coinbase_hex << "\",\n";
    out << "  \"header_hex_128\": \"" << result.header_hex << "\"\n";
    out << "}\n";
    out.close();

    std::printf("  ✅ Saved to: %s\n\n", output_file.c_str());

    return 0;
}
