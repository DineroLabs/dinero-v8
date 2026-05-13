#include "consensus/cic.h"
#include "crypto/dinero_crypto_minimal.h"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>

namespace dinero {

std::string ChainIdentityCheck::BuildCic(
    const std::string& hrp,
    uint32_t genesis_bits,
    uint32_t cpufriendly_bits,
    uint64_t cpufriendly_reward,
    uint32_t halving_interval,
    const std::string& genesis_hash,
    const std::string& network_name
) {
    // Build blob containing all chain-defining parameters
    std::ostringstream blob;
    
    // Core identity parameters
    blob << "genesis_hash:" << genesis_hash;
    blob << ";hrp:" << hrp;
    blob << ";network:" << network_name;
    
    // Consensus parameters
    blob << ";genesis_bits:" << std::hex << genesis_bits;
    blob << ";cpufriendly_bits:" << std::hex << cpufriendly_bits;
    blob << ";cpufriendly_reward:" << std::dec << cpufriendly_reward;
    blob << ";halving_interval:" << halving_interval;
    
    // Version identifier for CIC format
    blob << ";cic_version:1";
    
    std::string blob_str = blob.str();
    return ComputeSha256Hex(blob_str);
}

bool ChainIdentityCheck::ValidateOrStoreCic(
    const std::string& computed_cic,
    const std::string& datadir,
    const std::string& network_name,
    bool dev_autoreset
) {
    std::string cic_db_path = GetCicDbPath(datadir, network_name);
    
    // Ensure directory exists
    std::filesystem::create_directories(std::filesystem::path(cic_db_path).parent_path());
    
    // Check if CIC file exists
    if (!std::filesystem::exists(cic_db_path)) {
        // First run - store the CIC
        std::ofstream cic_file(cic_db_path, std::ios::trunc);
        if (cic_file.is_open()) {
            cic_file << computed_cic << std::endl;
            cic_file.close();
            return true; // Success - stored new CIC
        } else {
            return false; // Failed to write CIC file
        }
    }
    
    // Read existing CIC
    std::ifstream cic_file(cic_db_path);
    if (!cic_file.is_open()) {
        return false; // Failed to read CIC file
    }
    
    std::string stored_cic;
    std::getline(cic_file, stored_cic);
    cic_file.close();
    
    // Trim whitespace
    stored_cic.erase(0, stored_cic.find_first_not_of(" \t\n\r"));
    stored_cic.erase(stored_cic.find_last_not_of(" \t\n\r") + 1);
    
    // Compare CICs
    if (computed_cic == stored_cic) {
        return true; // Match - all good
    }
    
    // Mismatch detected
    if (dev_autoreset) {
        // Dev mode - reset the database
        std::filesystem::remove_all(std::filesystem::path(datadir) / network_name);
        std::filesystem::create_directories(std::filesystem::path(datadir) / network_name);
        
        // Store new CIC
        std::ofstream new_cic_file(cic_db_path, std::ios::trunc);
        if (new_cic_file.is_open()) {
            new_cic_file << computed_cic << std::endl;
            new_cic_file.close();
            return true; // Reset successful
        }
    }
    
    return false; // Mismatch and not reset
}

std::string ChainIdentityCheck::ComputeSha256Hex(const std::string& data) {
    uint8_t hash[32];
    sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), hash);
    
    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < 32; ++i) {
        hex_stream << std::setw(2) << static_cast<unsigned>(hash[i]);
    }
    
    return hex_stream.str();
}

std::string ChainIdentityCheck::GetCicDbPath(const std::string& datadir, const std::string& network_name) {
    return (std::filesystem::path(datadir) / network_name / ".cic").string();
}

} // namespace dinero
