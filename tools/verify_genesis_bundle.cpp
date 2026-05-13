/**
 * @file verify_genesis_bundle.cpp
 * @brief Standalone genesis + premine artifact verification tool
 *
 * Verifies a genesis bundle JSON against Dinero consensus rules.
 * Links against dinero_consensus_core — NO database, RPC, or datadir.
 *
 * Usage: verify_genesis_bundle <bundle.json>
 *
 * Checks:
 *   [G1] Genesis coinbase txid = merkle_root
 *   [G2] Genesis utreexo_root = v2 empty forest commitment
 *   [G3] Genesis header hash = block_hash (128-byte SHA256d)
 *   [G4] Genesis PoW valid (hash <= target from nBits)
 *   [P1] Premine prev_hash = genesis block_hash
 *   [P2] Premine coinbase txid = merkle_root
 *   [P3] Premine coinbase output matches amount + scriptPubKey
 *   [P4] Premine utreexo_root = v2 single-leaf forest commitment
 *   [P5] Premine header hash = block_hash (128-byte SHA256d)
 *   [P6] Premine PoW valid (hash <= target from nBits)
 *
 * Exit codes: 0 = all pass, 1 = verification failed, 2 = input error
 */

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <json/json.h>
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "crypto/sha256.h"
#include "consensus/utreexo_accumulator.h"

using namespace dinero;

// ============================================================================
// Byte conversion helpers
// ============================================================================

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nib = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back((nib(hex[i]) << 4) | nib(hex[i + 1]));
    }
    return out;
}

static std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static const char h[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s.push_back(h[data[i] >> 4]);
        s.push_back(h[data[i] & 0x0f]);
    }
    return s;
}

// ============================================================================
// SHA-256d (double SHA-256) → uint256
//
// Two variants matching the two canonical code paths in the codebase:
//   blockhash: byte-reversed (matches BlockHeader::GetHash())
//   txid:      memcpy / no reversal (matches Transaction::GetTxid())
// ============================================================================

static uint256 compute_sha256d_blockhash(const uint8_t* data, size_t len) {
    uint8_t h1[32], h2[32];
    crypto::CSHA256().Write(data, len).Finalize(h1);
    crypto::CSHA256().Write(h1, 32).Finalize(h2);

    // SHA-256 output is big-endian; uint256 stores little-endian (data[0]=LSB)
    // Reverse bytes so operator< and GetHex() work correctly for block hashes.
    uint256 result;
    for (int i = 0; i < 32; i++)
        result.data[i] = h2[31 - i];
    return result;
}

static uint256 compute_sha256d_txid(const uint8_t* data, size_t len) {
    uint8_t h1[32], h2[32];
    crypto::CSHA256().Write(data, len).Finalize(h1);
    crypto::CSHA256().Write(h1, 32).Finalize(h2);

    // Transaction::GetTxid() uses DoubleSHA256Bytes() which returns raw bytes
    // stored directly into uint256 via memcpy — no byte reversal.
    uint256 result;
    std::memcpy(result.data, h2, 32);
    return result;
}

// ============================================================================
// CompactSize varint reader (for transaction parsing)
// ============================================================================

static uint64_t read_compact_size(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    uint8_t v = *p++;
    if (v < 253) return v;
    if (v == 253 && p + 2 <= end) {
        uint16_t r = p[0] | ((uint16_t)p[1] << 8);
        p += 2;
        return r;
    }
    if (v == 254 && p + 4 <= end) {
        uint32_t r = p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        return r;
    }
    if (v == 255 && p + 8 <= end) {
        uint64_t r = 0;
        for (int i = 0; i < 8; i++)
            r |= ((uint64_t)p[i]) << (i * 8);
        p += 8;
        return r;
    }
    return 0;
}

// ============================================================================
// Build 128-byte BlockHeader and compute hash via canonical code path
// ============================================================================

static uint256 compute_header_hash(
    uint32_t version,
    const uint256& prev_hash,
    const uint256& merkle_root,
    const uint256& utreexo_root,
    uint64_t timestamp,
    uint32_t difficulty,
    uint32_t nonce)
{
    BlockHeader hdr;
    hdr.version = version;
    hdr.prev_block_hash = prev_hash;
    hdr.merkle_root = merkle_root;
    hdr.utreexo_root = utreexo_root;
    hdr.timestamp = timestamp;
    hdr.difficulty = difficulty;
    hdr.nonce = nonce;
    hdr.ZeroReserved();
    return hdr.GetHash();
}

// ============================================================================
// PoW check: hash <= target derived from compact nBits
// ============================================================================

static bool check_pow(const uint256& hash, uint32_t nbits) {
    int exponent = nbits >> 24;
    uint32_t mantissa = nbits & 0x007fffff;
    if (nbits & 0x00800000) return false;  // negative mantissa

    // Build target in big-endian 32 bytes
    uint8_t target_be[32] = {0};
    if (exponent >= 3 && exponent <= 34) {
        int pos = 32 - exponent;  // MSB of mantissa in big-endian
        if (pos >= 0 && pos < 32)     target_be[pos]     = (mantissa >> 16) & 0xff;
        if (pos + 1 >= 0 && pos + 1 < 32) target_be[pos + 1] = (mantissa >> 8) & 0xff;
        if (pos + 2 >= 0 && pos + 2 < 32) target_be[pos + 2] = mantissa & 0xff;
    } else if (exponent < 3) {
        mantissa >>= 8 * (3 - exponent);
        target_be[31] = mantissa & 0xff;
        if (exponent >= 2) target_be[30] = (mantissa >> 8) & 0xff;
    }

    // Convert BE → uint256 LE
    uint256 target;
    for (int i = 0; i < 32; i++)
        target.data[i] = target_be[31 - i];

    return !(target < hash);  // hash <= target
}

// ============================================================================
// Parse first output from raw transaction bytes
// ============================================================================

struct ParsedOutput {
    uint64_t value = 0;
    std::vector<uint8_t> scriptPubKey;
    bool valid = false;
};

static ParsedOutput parse_first_output(const std::vector<uint8_t>& tx) {
    ParsedOutput out;
    const uint8_t* p = tx.data();
    const uint8_t* end = p + tx.size();

    // version (4 bytes)
    if (p + 4 > end) return out;
    p += 4;

    // segwit marker check
    if (p + 2 <= end && p[0] == 0x00 && p[1] == 0x01) p += 2;

    // inputs
    uint64_t n_in = read_compact_size(p, end);
    for (uint64_t i = 0; i < n_in; i++) {
        if (p + 36 > end) return out;
        p += 36;  // txid + vout
        uint64_t ss = read_compact_size(p, end);
        if (p + ss + 4 > end) return out;
        p += ss + 4;  // scriptSig + sequence
    }

    // outputs
    uint64_t n_out = read_compact_size(p, end);
    if (n_out < 1) return out;

    // first output: value (8 LE)
    if (p + 8 > end) return out;
    for (int i = 0; i < 8; i++)
        out.value |= ((uint64_t)p[i]) << (i * 8);
    p += 8;

    // scriptPubKey
    uint64_t spk_len = read_compact_size(p, end);
    if (p + spk_len > end) return out;
    out.scriptPubKey.assign(p, p + spk_len);
    out.valid = true;
    return out;
}

// ============================================================================
// Check runner
// ============================================================================

struct Checker {
    int passed = 0;
    int failed = 0;

    void check(const char* id, const char* desc, bool ok) {
        std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] "
                  << id << ": " << desc << "\n";
        if (ok) passed++; else failed++;
    }

    void detail(const char* label, const std::string& val) {
        std::cout << "         " << label << ": " << val << "\n";
    }
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: verify_genesis_bundle <bundle.json>\n"
                  << "\n"
                  << "Standalone verification of Dinero genesis + premine artifacts.\n"
                  << "No database, RPC, wallet, or datadir dependencies.\n"
                  << "\n"
                  << "See docs/consensus/CONSENSUS-SNAPSHOT-v1.md for field definitions.\n";
        return 2;
    }

    // --- Read JSON ---
    std::ifstream file(argv[1]);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open " << argv[1] << "\n";
        return 2;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, file, &root, &errs)) {
        std::cerr << "Error: invalid JSON: " << errs << "\n";
        return 2;
    }

    if (!root.isMember("genesis") || !root.isMember("premine")) {
        std::cerr << "Error: JSON must contain \"genesis\" and \"premine\" objects\n";
        return 2;
    }

    Checker chk;

    // ====================================================================
    // GENESIS BLOCK (height 0)
    // ====================================================================
    std::cout << "\n=== Genesis Block (height 0) ===\n";

    const Json::Value& g = root["genesis"];
    const uint32_t  g_ver   = g["version"].asUInt();
    const uint256   g_prev  = uint256::FromHexUnsafe(
                                g["prev_block_hash"].asString());
    const uint256   g_merk  = uint256::FromHexUnsafe(
                                g["merkle_root"].asString());
    const uint256   g_utx   = uint256::FromHexUnsafe(
                                g["utreexo_root"].asString());
    const uint64_t  g_time  = g["timestamp"].asUInt64();
    const uint32_t  g_bits  = static_cast<uint32_t>(
                                std::stoul(g["difficulty"].asString(), nullptr, 16));
    const uint32_t  g_nonce = g["nonce"].asUInt();
    const uint256   g_hash  = uint256::FromHexUnsafe(
                                g["block_hash"].asString());
    const auto      g_cb    = hex_to_bytes(g["coinbase_hex"].asString());

    // [G1] coinbase txid = merkle_root
    // Accept either canonical string form:
    //   - raw SHA256 bytes hex
    //   - uint256 display hex (GetHex())
    uint256 g_txid = compute_sha256d_txid(g_cb.data(), g_cb.size());
    std::string g_txid_raw_hex = bytes_to_hex(g_txid.data, 32);
    std::string g_txid_display_hex = g_txid.GetHex();
    std::string g_merk_hex = g["merkle_root"].asString();
    bool g1_ok = (g_txid_raw_hex == g_merk_hex) || (g_txid_display_hex == g_merk_hex);
    chk.check("G1", "coinbase txid = merkle_root", g1_ok);
    if (!g1_ok) {
        chk.detail("computed txid(raw)", g_txid_raw_hex);
        chk.detail("computed txid(display)", g_txid_display_hex);
        chk.detail("merkle_root  ", g_merk_hex);
    }

    // [G2] utreexo_root = v2 empty forest commitment
    consensus::UtreexoForest empty_forest;
    consensus::UtreexoHash   empty_commit = empty_forest.getCommitment();
    uint256 g_utx_computed;
    std::memcpy(g_utx_computed.data, empty_commit.data(), 32);
    chk.check("G2", "utreexo_root = v2 empty forest commitment",
              g_utx_computed == g_utx);
    if (g_utx_computed != g_utx) {
        chk.detail("computed", g_utx_computed.GetHex());
        chk.detail("expected", g_utx.GetHex());
    }

    // [G3] header SHA256d = block_hash
    uint256 g_hash_c = compute_header_hash(
        g_ver, g_prev, g_merk, g_utx, g_time, g_bits, g_nonce);
    chk.check("G3", "header SHA256d = block_hash", g_hash_c == g_hash);
    if (g_hash_c != g_hash) {
        chk.detail("computed", g_hash_c.GetHex());
        chk.detail("expected", g_hash.GetHex());
    }

    // [G4] PoW valid
    chk.check("G4", "PoW valid (hash <= target)", check_pow(g_hash_c, g_bits));

    // ====================================================================
    // PREMINE BLOCK (height 1)
    // ====================================================================
    std::cout << "\n=== Premine Block (height 1) ===\n";

    const Json::Value& p = root["premine"];
    const uint32_t  p_ver   = p["version"].asUInt();
    const uint256   p_prev  = uint256::FromHexUnsafe(
                                p["prev_block_hash"].asString());
    const uint256   p_merk  = uint256::FromHexUnsafe(
                                p["merkle_root"].asString());
    const uint256   p_utx   = uint256::FromHexUnsafe(
                                p["utreexo_root"].asString());
    const uint64_t  p_time  = p["timestamp"].asUInt64();
    const uint32_t  p_bits  = static_cast<uint32_t>(
                                std::stoul(p["difficulty"].asString(), nullptr, 16));
    const uint32_t  p_nonce = p["nonce"].asUInt();
    const uint256   p_hash  = uint256::FromHexUnsafe(
                                p["block_hash"].asString());
    const auto      p_cb    = hex_to_bytes(p["coinbase_hex"].asString());
    const uint64_t  p_amt   = p["amount_una"].asUInt64();
    const auto      p_spk   = hex_to_bytes(p["scriptPubKey_hex"].asString());

    // [P1] prev_hash = genesis block_hash
    chk.check("P1", "prev_hash = genesis block_hash", p_prev == g_hash);

    // [P2] coinbase txid = merkle_root
    // Accept either canonical string form:
    //   - raw SHA256 bytes hex
    //   - uint256 display hex (GetHex())
    uint256 p_txid = compute_sha256d_txid(p_cb.data(), p_cb.size());
    std::string p_txid_raw_hex = bytes_to_hex(p_txid.data, 32);
    std::string p_txid_display_hex = p_txid.GetHex();
    std::string p_merk_hex = p["merkle_root"].asString();
    bool p2_ok = (p_txid_raw_hex == p_merk_hex) || (p_txid_display_hex == p_merk_hex);
    chk.check("P2", "coinbase txid = merkle_root", p2_ok);
    if (!p2_ok) {
        chk.detail("computed txid(raw)", p_txid_raw_hex);
        chk.detail("computed txid(display)", p_txid_display_hex);
        chk.detail("merkle_root  ", p_merk_hex);
    }

    // [P3] coinbase output matches amount + scriptPubKey
    ParsedOutput out = parse_first_output(p_cb);
    bool p3 = out.valid && out.value == p_amt && out.scriptPubKey == p_spk;
    chk.check("P3", "coinbase output = amount + scriptPubKey", p3);
    if (!p3 && out.valid) {
        chk.detail("parsed value", std::to_string(out.value) +
                   " (expected " + std::to_string(p_amt) + ")");
        chk.detail("parsed spk",
                   bytes_to_hex(out.scriptPubKey.data(), out.scriptPubKey.size()));
        chk.detail("expected spk",
                   bytes_to_hex(p_spk.data(), p_spk.size()));
    }

    // [P4] utreexo_root = v2 single-leaf forest commitment
    consensus::UtreexoHash leaf = consensus::HashUTXO(
        p_txid, 0, p_amt, p_spk);
    consensus::UtreexoForest premine_forest;
    premine_forest.add(leaf);
    consensus::UtreexoHash premine_commit = premine_forest.getCommitment();
    uint256 p_utx_computed;
    std::memcpy(p_utx_computed.data, premine_commit.data(), 32);
    chk.check("P4", "utreexo_root = v2 single-leaf commitment",
              p_utx_computed == p_utx);
    if (p_utx_computed != p_utx) {
        chk.detail("computed", p_utx_computed.GetHex());
        chk.detail("expected", p_utx.GetHex());
        chk.detail("leaf hash",
                   bytes_to_hex(leaf.data(), leaf.size()));
    }

    // [P5] header SHA256d = block_hash
    uint256 p_hash_c = compute_header_hash(
        p_ver, p_prev, p_merk, p_utx, p_time, p_bits, p_nonce);
    chk.check("P5", "header SHA256d = block_hash", p_hash_c == p_hash);
    if (p_hash_c != p_hash) {
        chk.detail("computed", p_hash_c.GetHex());
        chk.detail("expected", p_hash.GetHex());
    }

    // [P6] PoW valid
    chk.check("P6", "PoW valid (hash <= target)", check_pow(p_hash_c, p_bits));

    // ====================================================================
    // Summary
    // ====================================================================
    int total = chk.passed + chk.failed;
    std::cout << "\n=== Summary ===\n"
              << "  Passed: " << chk.passed << "/" << total << "\n";

    if (chk.failed > 0) {
        std::cout << "  FAILED: " << chk.failed << " check(s)\n";
        return 1;
    }

    std::cout << "  All checks passed. Genesis bundle is valid.\n";
    return 0;
}
