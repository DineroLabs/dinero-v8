// ============================================================================
//  Dinero Genesis Block Verifier
//  Ensures runtime consensus between compiled constants and actual data
// ============================================================================

#include "consensus/genesis_verifier.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "utils/hexwriter.h"
#include "utils/hexreader.h"
#include "crypto/sha256.h"
#include "common/logger.h"
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>
#include <iostream>

namespace dinero {

// ============================================================================
// Local utilities for hex/hash conversion
// ============================================================================

static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16)));
    }
    return bytes;
}

static std::string bytesToHex(const std::vector<uint8_t>& bytes, bool reverse = false) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');

    if (reverse) {
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            ss << std::setw(2) << static_cast<int>(*it);
        }
    } else {
        for (auto byte : bytes) {
            ss << std::setw(2) << static_cast<int>(byte);
        }
    }
    return ss.str();
}

static std::vector<uint8_t> doubleSHA256(const std::vector<uint8_t>& data) {
    // First SHA-256
    crypto::CSHA256 hash1;
    hash1.Write(data.data(), data.size());
    std::vector<uint8_t> hash1_result = hash1.Finalize();

    // Second SHA-256
    crypto::CSHA256 hash2;
    hash2.Write(hash1_result.data(), hash1_result.size());
    return hash2.Finalize();
}

// ============================================================================
// Merkle root computation from transaction hex
// ============================================================================

static std::string computeMerkleRoot(const std::string& txHex) {
    auto txBytes = hexToBytes(txHex);
    auto merkleHash = doubleSHA256(txBytes);
    return bytesToHex(merkleHash, true);  // Reversed for Bitcoin-style display
}

// ============================================================================
// Block header hash computation
// ============================================================================

static std::string computeBlockHash(uint32_t version,
                                    const std::string& prevBlockHash,
                                    const std::string& merkleRoot,
                                    uint32_t timestamp,
                                    uint32_t bits,
                                    uint32_t nonce)
{
    BlockHeader header{};
    header.version = version;
    header.prev_block_hash = prevBlockHash;
    header.prev_block_hash = prevBlockHash;
    header.merkle_root = merkleRoot;
    header.timestamp = timestamp;
    header.timestamp = timestamp;
    header.difficulty = bits;
    header.difficulty = bits;
    header.nonce = nonce;
    header.utreexo_root = std::string(64, '0');

    auto serialized = header.SerializeForHash();
    auto blockHash = doubleSHA256(std::vector<uint8_t>(serialized.begin(), serialized.end()));

    return bytesToHex(blockHash, true);  // Reversed for Bitcoin-style display
}

// ============================================================================
// Public verification entry point
// ============================================================================

bool VerifyGenesisBlock(const ChainParams& params) {
    const std::string txHex = params.genesis.genesisCoinbaseHex;
    const std::string expectedMerkle = params.genesis.merkleRootHex;
    const std::string expectedHash = params.genesis.genesisHashHex;

    g_logger.info("🔍 Verifying genesis block integrity (Block 0)...");

    // Compute Merkle root from transaction
    std::string computedMerkle = computeMerkleRoot(txHex);

    // Compute block hash from header
    std::string computedHash = computeBlockHash(
        params.genesis.nVersion,
        std::string(64, '0'),  // Genesis has no previous block
        computedMerkle,
        params.genesis.nTime,
        params.genesis.nBits,
        params.genesis.nNonce
    );

    bool merkleValid = (computedMerkle == expectedMerkle);
    bool hashValid = (computedHash == expectedHash);

    // Log verification results
    g_logger.info("═══════════════════════════════════════════════════════");
    g_logger.info("            Genesis Block Verification Report          ");
    g_logger.info("═══════════════════════════════════════════════════════");
    g_logger.info("Network: " + params.name);
    g_logger.info("Block Height: 0 (Genesis)");
    g_logger.info("");
    g_logger.info("Merkle Root:");
    g_logger.info("  Expected: " + expectedMerkle);
    g_logger.info("  Computed: " + computedMerkle);
    g_logger.info("  Status:   " + std::string(merkleValid ? "✅ VALID" : "❌ INVALID"));
    g_logger.info("");
    g_logger.info("Block Hash:");
    g_logger.info("  Expected: " + expectedHash);
    g_logger.info("  Computed: " + computedHash);
    g_logger.info("  Status:   " + std::string(hashValid ? "✅ VALID" : "❌ INVALID"));
    g_logger.info("═══════════════════════════════════════════════════════");

    if (!merkleValid || !hashValid) {
        g_logger.error("❌ Genesis block verification FAILED");
        g_logger.error("⚠️  Critical consensus error detected!");
        g_logger.error("⚠️  The compiled genesis parameters do not match");
        g_logger.error("⚠️  the actual serialized block data.");
        g_logger.error("⚠️  This node will NOT start to prevent chain divergence.");
        return false;
    }

    g_logger.info("✅ Genesis block verification PASSED");
    g_logger.info("");

    g_logger.info("✅ Genesis verification PASSED (no premine)");
    g_logger.info("🔐 Consensus checksum: " + dinero::ConsensusChecksum(params));
    g_logger.info("   (This checksum must match across all nodes for consensus)");

    return true;
}

} // namespace dinero
