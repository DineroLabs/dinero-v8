#pragma once

#include <string>
#include <cstdint>

namespace dinero {

// Forward declarations
struct ConsensusParams;
class uint256;

/**
 * Chain Identity Check (CIC) - Computes a unique identifier for the blockchain
 * configuration to prevent accidental chain mixing.
 * 
 * The CIC includes all parameters that define the chain identity:
 * - Genesis block hash
 * - Network HRP (Human Readable Part) 
 * - Consensus parameters (bits, rewards, etc.)
 * - Network configuration
 */
class ChainIdentityCheck {
public:
    /**
     * Build CIC from consensus parameters and genesis hash
     * @param params Consensus parameters
     * @param genesis_hash Genesis block hash
     * @param network_name Network name (regtest/testnet/mainnet)
     * @return CIC hex string
     */
    static std::string BuildCic(
        const std::string& hrp,
        uint32_t genesis_bits,
        uint32_t cpufriendly_bits, 
        uint64_t cpufriendly_reward,
        uint32_t halving_interval,
        const std::string& genesis_hash,
        const std::string& network_name
    );
    
    /**
     * Validate CIC against stored value in database
     * @param computed_cic The computed CIC for current configuration
     * @param datadir Database directory path
     * @param network_name Network name for DB path
     * @param dev_autoreset If true, reset DB on mismatch (dev mode only)
     * @return true if CIC matches or was reset, false if mismatch
     */
    static bool ValidateOrStoreCic(
        const std::string& computed_cic,
        const std::string& datadir,
        const std::string& network_name,
        bool dev_autoreset = false
    );

private:
    static std::string ComputeSha256Hex(const std::string& data);
    static std::string GetCicDbPath(const std::string& datadir, const std::string& network_name);
};

} // namespace dinero
