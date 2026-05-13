#include "dinero/core/consensus/genesis_canonical.h"
#include "dinero/core/consensus/genesis_block.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dinero {

// Helper to convert bytes to hex (BE format for display)
static std::string BytesToHexBE(const std::vector<uint8_t>& v) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : v) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

CanonicalGenesis BuildCanonicalGenesis(const ChainParams& P) {
    CanonicalGenesis out{};
    
    // ⚠️ IMPORTANT: Use the EXACT same functions the daemon uses
    // Create genesis block using daemon's method (copy from dump_genesis_header)
    GenesisBlock genesis;
    genesis.version = P.genesis.nVersion;
    genesis.timestamp = P.genesis.nTime;
    genesis.nonce = P.genesis.nNonce;
    genesis.bits = P.genesis.nBits;
    genesis.block_reward = 0;  // Genesis has no spendable reward
    genesis.coinbase_message = P.genesis.coinbaseText;
    
    // Initialize vectors
    genesis.prev_hash.resize(32, 0);
    genesis.merkle_root.resize(32, 0);
    genesis.block_hash.resize(32, 0);
    genesis.raw_block.clear();
    
    // Create coinbase and calculate merkle root using daemon's method
    auto coinbase_tx = GenesisBlockGenerator::createCoinbaseTransaction(genesis);
    genesis.merkle_root = GenesisBlockGenerator::calculateMerkleRoot(coinbase_tx);
    
    // Serialize header using daemon's method
    auto header = GenesisBlockGenerator::serializeBlockHeader(genesis);
    
    // Calculate block hash using daemon's method
    auto calculated_hash = GenesisBlockGenerator::calculateBlockHash(header);
    
    // Store the 80-byte header (little-endian as daemon uses)
    if (header.size() == 80) {
        std::copy(header.begin(), header.end(), out.headerLE.begin());
    }
    
    // Convert hash to BE hex for display (same as dump_genesis_header)
    out.hashBE = BytesToHexBE(calculated_hash);
    
    // Convert merkle root to BE hex for display (same as dump_genesis_header)
    out.merkleBE = BytesToHexBE(genesis.merkle_root);
    
    return out;
}

} // namespace dinero
