// tools/premine_miner.cpp
// Dinero Premine Block Miner (Height 1)
//
// CRITICAL: Premine block must have valid Utreexo commitment.
// utreexo_root = LeafHash(premine UTXO) - computed BEFORE mining.
//
// This miner:
// 1. Builds premine coinbase with 2,627,900 DIN to P2TR address
// 2. Computes merkle root = coinbase txid
// 3. Computes utreexo leaf hash from the premine output
// 4. Mines with utreexo_root committed in header
// 5. Outputs all constants for hardcoding

#include "primitives/block.h"
#include "primitives/uint256.h"
#include "primitives/transaction.h"
#include "crypto/sha256.h"
#include "consensus/chain_bundle_generated.h"
#include "consensus/utreexo_accumulator.h"  // For HashUTXO
#include "daemon/bech32_encoder.h"

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
#include <inttypes.h>

using namespace dinero;

// ============================================================================
// PREMINE CONSTANTS (IMMUTABLE)
// ============================================================================

// Genesis parameters come from the canonical generated bundle.
static constexpr const char* GENESIS_HASH_HEX = dinero::chain_bundle::GENESIS_BLOCK_HASH;
static constexpr uint64_t GENESIS_TIMESTAMP = dinero::chain_bundle::GENESIS_TIMESTAMP;

// Premine parameters
static constexpr uint64_t PREMINE_AMOUNT_UNA = 262790000000000ULL;  // 2,627,900 DIN
// BIP86 Taproot: m/86'/1447'/0'/0/0 - controlled by BIP39 seed phrase
static constexpr const char* PREMINE_ADDRESS =
    "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0";
static constexpr uint32_t PREMINE_DIFFICULTY = dinero::chain_bundle::GENESIS_DIFFICULTY;

// ============================================================================
// COMPILE-TIME VERIFICATION
// ============================================================================

static_assert(sizeof(BlockHeader) == 128,
    "FATAL: BlockHeader must be 128 bytes");

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

static inline void write_u64_le(uint8_t* p, uint64_t x) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(x >> (8 * i));
}

static inline std::string hex(const uint8_t* b, size_t n) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < n; ++i)
        os << std::setw(2) << (unsigned)b[i];
    return os.str();
}

static inline std::string hex_le(const uint8_t* b, size_t n) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < n; ++i)
        os << std::setw(2) << (unsigned)b[n - 1 - i];
    return os.str();
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
        for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(n >> (8 * i)));
    } else {
        out.push_back(0xff);
        for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(n >> (8 * i)));
    }
}

// ============================================================================
// PREMINE COINBASE TRANSACTION
// ============================================================================

struct PremineCoinbase {
    std::vector<uint8_t> raw_tx;
    uint256 txid;
    std::vector<uint8_t> scriptPubKey;
};

static PremineCoinbase build_premine_coinbase() {
    PremineCoinbase result;
    std::vector<uint8_t>& tx = result.raw_tx;

    // Version (4 bytes)
    tx.resize(4);
    write_u32_le(tx.data(), 1);

    // Input count (1)
    push_varint(tx, 1);

    // Input 0: Coinbase (prev_txid = 0x00...00, prev_vout = 0xffffffff)
    tx.insert(tx.end(), 32, 0x00);
    uint8_t vout[4];
    write_u32_le(vout, 0xffffffff);
    tx.insert(tx.end(), vout, vout + 4);

    // scriptSig: BIP34 height encoding + message
    // BIP34 format: [push_length] [height_LE_bytes] [message...]
    // For height 1: 0x01 (push 1 byte) 0x01 (height=1)
    std::vector<uint8_t> scriptSig;
    scriptSig.push_back(0x01);  // BIP34: push 1 byte
    scriptSig.push_back(0x01);  // BIP34: height = 1
    std::string msg = "Dinero Premine - 2,627,900 DIN";
    scriptSig.insert(scriptSig.end(), msg.begin(), msg.end());

    push_varint(tx, scriptSig.size());
    tx.insert(tx.end(), scriptSig.begin(), scriptSig.end());

    // Sequence: 0xffffffff
    uint8_t seq[4];
    write_u32_le(seq, 0xffffffff);
    tx.insert(tx.end(), seq, seq + 4);

    // Output count (1)
    push_varint(tx, 1);

    // Output 0: 2,627,900 DIN to P2TR address
    uint8_t amt[8];
    write_u64_le(amt, PREMINE_AMOUNT_UNA);
    tx.insert(tx.end(), amt, amt + 8);

    // Decode premine address to get scriptPubKey
    auto decode_result = Bech32Encoder::decode_segwit_address(PREMINE_ADDRESS);
    if (!decode_result.valid || decode_result.witness_version != 1 ||
        decode_result.witness_program.size() != 32) {
        std::cerr << "FATAL: Invalid premine address\n";
        std::exit(1);
    }

    // P2TR scriptPubKey: OP_1 (0x51) + OP_PUSHBYTES_32 (0x20) + 32 bytes
    result.scriptPubKey.push_back(0x51);  // OP_1
    result.scriptPubKey.push_back(0x20);  // Push 32 bytes
    result.scriptPubKey.insert(result.scriptPubKey.end(),
                               decode_result.witness_program.begin(),
                               decode_result.witness_program.end());

    push_varint(tx, result.scriptPubKey.size());
    tx.insert(tx.end(), result.scriptPubKey.begin(), result.scriptPubKey.end());

    // Locktime (4 bytes: 0)
    uint8_t locktime[4] = {0, 0, 0, 0};
    tx.insert(tx.end(), locktime, locktime + 4);

    // Compute txid (double SHA256)
    crypto::CSHA256 h1;
    h1.Write(tx.data(), tx.size());
    uint8_t tmp[32];
    h1.Finalize(tmp);

    crypto::CSHA256 h2;
    h2.Write(tmp, 32);
    uint8_t txid_bytes[32];
    h2.Finalize(txid_bytes);

    // Match Transaction::GetTxid() convention: data[i] = hash[i] (no reversal).
    // GetHash() reverses, but GetTxid() does not.
    std::memcpy(result.txid.data, txid_bytes, 32);

    return result;
}

// ============================================================================
// UTREEXO COMMITMENT
// ============================================================================

// Compute Utreexo commitment v2 for a single-leaf forest (the premine UTXO).
// 1. Hash the UTXO into a leaf: SHA256("DINERO-UTXO-LEAF-v1" || txid || vout || amount || CompactSize(scriptLen) || scriptPubKey)
// 2. Add to forest → single tree of height 0
// 3. Commitment = SHA256(numLeaves=1_LE64 || root[0]=leaf || 63×32_zero_bytes)
static uint256 compute_utreexo_commitment(const uint256& txid, uint32_t vout,
                                          uint64_t amount,
                                          const std::vector<uint8_t>& scriptPubKey) {
    consensus::UtreexoHash leaf_hash = consensus::HashUTXOLegacy(txid, vout, amount, scriptPubKey);
    consensus::UtreexoForest forest;
    forest.add(leaf_hash);
    consensus::UtreexoHash commitment = forest.getCommitment();
    uint256 result;
    if (commitment.size() == 32)
        std::memcpy(result.data, commitment.data(), 32);
    return result;
}

// ============================================================================
// TARGET CONVERSION
// ============================================================================

static inline void compact_to_target(uint32_t bits, uint8_t out[32]) {
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
            out[idx] = (uint8_t)((mant >> 16) & 0xff);
            out[idx + 1] = (uint8_t)((mant >> 8) & 0xff);
            out[idx + 2] = (uint8_t)((mant) & 0xff);
        }
    }
}

static inline bool leq_256_be(const uint8_t a[32], const uint8_t b[32]) {
    for (int i = 0; i < 32; ++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return true;
}

// ============================================================================
// MINER
// ============================================================================

struct FoundResult {
    bool found = false;
    uint32_t nonce = 0;
    std::string block_hash_le;
    std::string header_hex;
};

static void mine_worker(
    uint32_t start, uint32_t end,
    const BlockHeader& template_header,
    const uint8_t target_be[32],
    std::atomic<bool>& stop,
    FoundResult& out,
    std::mutex& out_mu
) {
    BlockHeader header = template_header;

    for (uint32_t nonce = start; nonce != end && !stop.load(); ++nonce) {
        header.nonce = nonce;

        auto bytes = header.SerializeForHash();
        assert(bytes.size() == 128);

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
                out.block_hash_le = hex(h32, 32);
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
    unsigned threads = std::thread::hardware_concurrency();
    std::string output_file = "premine_block.json";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads = std::stoul(argv[++i]);
        } else if (a == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        }
    }

    std::printf("\n");
    std::printf("====================================================================\n");
    std::printf("  DINERO PREMINE BLOCK MINER (Height 1)\n");
    std::printf("====================================================================\n");
    std::printf("\n");
    std::printf("  CRITICAL: Utreexo root computed BEFORE mining.\n");
    std::printf("            Block hash commits to utreexo_root forever.\n");
    std::printf("\n");
    std::printf("  Compile-time checks:\n");
    std::printf("    sizeof(BlockHeader) == %zu bytes\n", sizeof(BlockHeader));
    std::printf("\n");
    std::printf("  Premine parameters:\n");
    std::printf("    Prev hash:    %s\n", GENESIS_HASH_HEX);
    std::printf("    Amount:       %" PRIu64 " una (2,627,900 DIN)\n", PREMINE_AMOUNT_UNA);
    std::printf("    Address:      %s\n", PREMINE_ADDRESS);
    std::printf("    Difficulty:   0x%08x\n", PREMINE_DIFFICULTY);
    std::printf("    Threads:      %u\n", threads);
    std::printf("\n");

    // ========================================================================
    // STEP 1: Build premine coinbase
    // ========================================================================
    std::printf("  [1/5] Building premine coinbase...\n");
    PremineCoinbase coinbase = build_premine_coinbase();
    std::printf("        Coinbase: %zu bytes\n", coinbase.raw_tx.size());
    std::printf("        Txid:     %s\n", coinbase.txid.GetHex().c_str());

    // ========================================================================
    // STEP 2: Compute merkle root (single tx = txid)
    // ========================================================================
    std::printf("  [2/5] Computing merkle root...\n");
    // Consensus convention: use TXID display hex and parse via FromHexUnsafe().
    // This stores txid raw bytes in the header field while keeping the canonical
    // human-readable merkle_root string aligned with txid.GetHex().
    std::string coinbase_txid_display_hex = coinbase.txid.GetHex();
    uint256 merkle_root = uint256::FromHexUnsafe(coinbase_txid_display_hex);
    std::printf("        Merkle:   %s\n", merkle_root.GetHex().c_str());

    // ========================================================================
    // STEP 3: Compute Utreexo commitment (v2 forest commitment)
    // ========================================================================
    std::printf("  [3/5] Computing Utreexo commitment v2...\n");

    // Also compute and display the raw leaf hash for reference
    consensus::UtreexoHash leaf_hash_raw = consensus::HashUTXOLegacy(
        coinbase.txid, 0, PREMINE_AMOUNT_UNA, coinbase.scriptPubKey);
    std::printf("        Leaf:     ");
    for (uint8_t b : leaf_hash_raw)
        std::printf("%02x", b);
    std::printf("\n");

    // The utreexo_root in the header is the FOREST COMMITMENT, not the leaf hash
    uint256 utreexo_root = compute_utreexo_commitment(
        coinbase.txid, 0, PREMINE_AMOUNT_UNA, coinbase.scriptPubKey);
    std::printf("        Commit:   %s\n", utreexo_root.GetHex().c_str());
    std::printf("\n");
    std::printf("        CRITICAL: This commitment encodes:\n");
    std::printf("          - numLeaves:   1\n");
    std::printf("          - txid:        %s\n", coinbase.txid.GetHex().c_str());
    std::printf("          - vout:        0\n");
    std::printf("          - amount:      %" PRIu64 " una\n", PREMINE_AMOUNT_UNA);
    std::printf("          - scriptPubKey: %s\n", hex(coinbase.scriptPubKey.data(),
                                                      coinbase.scriptPubKey.size()).c_str());
    std::printf("\n");

    // ========================================================================
    // STEP 4: Build block header
    // ========================================================================
    std::printf("  [4/5] Building block header...\n");

    BlockHeader header{};
    header.version = 1;
    header.prev_block_hash = uint256::FromHexUnsafe(GENESIS_HASH_HEX);
    header.merkle_root = merkle_root;
    header.utreexo_root = utreexo_root;  // NON-ZERO!
    header.timestamp = GENESIS_TIMESTAMP + 600;  // 10 minutes after genesis
    header.difficulty = PREMINE_DIFFICULTY;
    header.nonce = 0;
    header.ZeroReserved();

    assert(header.IsReservedValid());

    std::printf("        Version:    %u\n", header.version);
    std::printf("        Prev:       %s\n", header.prev_block_hash.GetHex().c_str());
    std::printf("        Merkle:     %s\n", header.merkle_root.GetHex().c_str());
    std::printf("        Utreexo:    %s\n", header.utreexo_root.GetHex().c_str());
    std::printf("        Timestamp:  %" PRIu64 "\n", header.timestamp);
    std::printf("        Difficulty: 0x%08x\n", header.difficulty);
    std::printf("\n");

    // ========================================================================
    // STEP 5: Mine
    // ========================================================================
    std::printf("  [5/5] Mining premine block...\n");
    std::printf("        ");
    std::fflush(stdout);

    uint8_t target_be[32];
    compact_to_target(PREMINE_DIFFICULTY, target_be);

    auto start_time = std::chrono::steady_clock::now();

    std::atomic<bool> stop{false};
    FoundResult result;
    std::mutex result_mu;

    std::vector<std::thread> workers;
    uint32_t range = 0xffffffffu / threads;

    for (unsigned t = 0; t < threads; ++t) {
        uint32_t start = t * range;
        uint32_t end = (t == threads - 1) ? 0xffffffffu : (start + range);

        workers.emplace_back(mine_worker,
            start, end, header, target_be,
            std::ref(stop), std::ref(result), std::ref(result_mu));
    }

    for (auto& w : workers) w.join();

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count() / 1000.0;

    std::printf("\n\n");

    if (!result.found) {
        std::printf("  Premine block not found\n\n");
        return 1;
    }

    // ========================================================================
    // OUTPUT
    // ========================================================================
    std::printf("====================================================================\n");
    std::printf("  DINERO PREMINE BLOCK (Height 1)\n");
    std::printf("====================================================================\n");
    std::printf("\n");
    std::printf("  Block Hash:     %s\n", result.block_hash_le.c_str());
    std::printf("  Merkle Root:    %s\n", merkle_root.GetHex().c_str());
    std::printf("  Utreexo Root:   %s\n", utreexo_root.GetHex().c_str());
    std::printf("  Version:        %u\n", header.version);
    std::printf("  Prev Hash:      %s\n", GENESIS_HASH_HEX);
    std::printf("  Timestamp:      %" PRIu64 "\n", header.timestamp);
    std::printf("  Difficulty:     0x%08x\n", PREMINE_DIFFICULTY);
    std::printf("  Nonce:          %u\n", result.nonce);
    std::printf("\n");
    std::printf("  Coinbase Txid:  %s\n", coinbase.txid.GetHex().c_str());
    std::printf("  Amount:         %" PRIu64 " una (2,627,900 DIN)\n", PREMINE_AMOUNT_UNA);
    std::printf("  Address:        %s\n", PREMINE_ADDRESS);
    std::printf("\n");
    std::printf("  Header Size:    %zu bytes\n", result.header_hex.length() / 2);
    std::printf("  Elapsed:        %.2f seconds\n", elapsed);
    std::printf("\n");

    // Save to JSON
    std::ofstream out(output_file);
    out << "{\n";
    out << "  \"height\": 1,\n";
    out << "  \"block_hash\": \"" << result.block_hash_le << "\",\n";
    out << "  \"merkle_root\": \"" << merkle_root.GetHex() << "\",\n";
    out << "  \"utreexo_root\": \"" << utreexo_root.GetHex() << "\",\n";
    out << "  \"version\": " << header.version << ",\n";
    out << "  \"prev_hash\": \"" << GENESIS_HASH_HEX << "\",\n";
    out << "  \"timestamp\": " << header.timestamp << ",\n";
    out << "  \"difficulty\": \"0x" << std::hex << std::setfill('0')
        << std::setw(8) << PREMINE_DIFFICULTY << "\",\n";
    out << "  \"nonce\": " << std::dec << result.nonce << ",\n";
    out << "  \"coinbase_txid\": \"" << coinbase.txid.GetHex() << "\",\n";
    out << "  \"amount_una\": " << PREMINE_AMOUNT_UNA << ",\n";
    out << "  \"address\": \"" << PREMINE_ADDRESS << "\",\n";
    out << "  \"coinbase_hex\": \"" << hex(coinbase.raw_tx.data(), coinbase.raw_tx.size()) << "\",\n";
    out << "  \"header_hex_128\": \"" << result.header_hex << "\"\n";
    out << "}\n";
    out.close();

    std::printf("  Saved to: %s\n\n", output_file.c_str());

    return 0;
}
