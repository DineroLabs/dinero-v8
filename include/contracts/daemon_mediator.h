#pragma once

#include <string>
#include <filesystem>
#include <optional>
#include <array>
#include <vector>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace contracts {

/**
 * DaemonMediator - Automated mediator for escrow contracts
 *
 * The daemon generates and stores a mediator keypair that can be used
 * for automatic dispute resolution based on time-based logic.
 *
 * Resolution Logic:
 * - Before seller_window: No automatic signing
 * - After seller_window: Daemon signs for seller (assumes delivery)
 * - After refund_time: Daemon signs for buyer (timeout refund)
 */
class DaemonMediator {
public:
    /**
     * Initialize mediator (load existing key or generate new one)
     * @param datadir Path to data directory
     * @return true if successful
     */
    static bool initialize(const std::filesystem::path& datadir);

    /**
     * Get mediator public key (hex)
     * @return Public key or nullopt if not initialized
     */
    static std::optional<std::string> getMediatorPubKey();

    /**
     * Get mediator private key (hex) - for signing
     * @return Private key or nullopt if not initialized
     */
    static std::optional<std::string> getMediatorPrivKey();

    /**
     * Check if mediator should sign for a given contract
     * @param contract_id Contract to check
     * @param current_height Current blockchain height
     * @param favor_seller true to check seller window, false for buyer refund
     * @return true if mediator should sign
     */
    static bool shouldSign(
        const std::string& contract_id,
        uint32_t current_height,
        bool favor_seller
    );

    /**
     * Sign a transaction input with mediator key
     * @param tx_hash Transaction hash to sign (hex, 32 bytes)
     * @param input_index Input index
     * @return Signature (DER encoded) or nullopt on failure
     */
    static std::optional<std::string> signInput(
        const std::string& tx_hash,
        uint32_t input_index
    );

private:
    // Helper functions
    static std::string bytes_to_hex(const uint8_t* bytes, size_t len);
    static std::vector<uint8_t> hex_to_bytes(const std::string& hex);

    static std::array<uint8_t, 32> mediator_privkey_;
    static std::array<uint8_t, 33> mediator_pubkey_;
    static bool initialized_;
};

} // namespace contracts
} // namespace dinero
