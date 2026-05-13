#include "contracts/daemon_mediator.h"
#include "contracts/escrow_contract.h"
#include "contracts/contract_registry.h"
#include "dinero/core/crypto/dinero_crypto_minimal.h"
#include "daemon/crypto_utils.h"
#include "common/logger.h"
#include <fstream>
#include <random>
#include <cstring>
#include <sys/stat.h>

namespace dinero {
namespace contracts {

// Static members
std::array<uint8_t, 32> DaemonMediator::mediator_privkey_;
std::array<uint8_t, 33> DaemonMediator::mediator_pubkey_;
bool DaemonMediator::initialized_ = false;

bool DaemonMediator::initialize(const std::filesystem::path& datadir) {
    std::string key_path = (datadir / "mediator_key.dat").string();

    // Try to load existing key
    if (std::filesystem::exists(key_path)) {
        std::ifstream file(key_path, std::ios::binary);
        if (!file) {
            dinero::g_logger.error("[DaemonMediator] Failed to open key file");
            return false;
        }

        // Read private key (32 bytes)
        file.read(reinterpret_cast<char*>(mediator_privkey_.data()), 32);
        file.close();

        // Derive public key from private key using dinero_crypto_minimal
        if (!CF_GetCompressedPubkey(mediator_privkey_.data(), mediator_pubkey_.data())) {
            dinero::g_logger.error("[DaemonMediator] Failed to derive public key");
            return false;
        }

        initialized_ = true;

        dinero::g_logger.info("[DaemonMediator] Loaded existing mediator key");
        dinero::g_logger.info("[DaemonMediator] Pubkey: " + bytes_to_hex(mediator_pubkey_.data(), 33));
        return true;
    }

    // Generate new keypair
    if (!CF_GeneratePrivKey(mediator_privkey_.data())) {
        dinero::g_logger.error("[DaemonMediator] Failed to generate private key");
        return false;
    }

    // Derive public key
    if (!CF_GetCompressedPubkey(mediator_privkey_.data(), mediator_pubkey_.data())) {
        dinero::g_logger.error("[DaemonMediator] Failed to derive public key");
        return false;
    }

    // Save private key to file
    std::ofstream file(key_path, std::ios::binary);
    if (!file) {
        dinero::g_logger.error("[DaemonMediator] Failed to create key file");
        return false;
    }

    file.write(reinterpret_cast<const char*>(mediator_privkey_.data()), 32);
    file.close();

    // Set restrictive permissions (owner read/write only) - POSIX systems
#ifndef _WIN32
    chmod(key_path.c_str(), S_IRUSR | S_IWUSR);
#endif

    initialized_ = true;

    dinero::g_logger.info("[DaemonMediator] Generated new mediator keypair");
    dinero::g_logger.info("[DaemonMediator] Pubkey: " + bytes_to_hex(mediator_pubkey_.data(), 33));
    dinero::g_logger.info("[DaemonMediator] Key saved to: " + key_path);

    return true;
}

std::optional<std::string> DaemonMediator::getMediatorPubKey() {
    if (!initialized_) {
        return std::nullopt;
    }
    return bytes_to_hex(mediator_pubkey_.data(), 33);
}

std::optional<std::string> DaemonMediator::getMediatorPrivKey() {
    if (!initialized_) {
        return std::nullopt;
    }
    return bytes_to_hex(mediator_privkey_.data(), 32);
}

bool DaemonMediator::shouldSign(
    const std::string& contract_id,
    uint32_t current_height,
    bool favor_seller
) {
    if (!initialized_) {
        dinero::g_logger.warning("[DaemonMediator] Not initialized");
        return false;
    }

    // Load contract from registry
    auto& registry = ContractRegistry::instance();
    auto contract = registry.getContract(contract_id);

    if (!contract) {
        dinero::g_logger.warning("[DaemonMediator] Contract not found: " + contract_id);
        return false;
    }

    // Only auto-sign for auto-mediated contracts
    if (contract->type != EscrowType::TwoOfThreeAuto) {
        dinero::g_logger.info("[DaemonMediator] Contract " + contract_id +
                              " is not auto-mediated type");
        return false;
    }

    // Check if contract uses daemon mediator
    std::string our_pubkey = bytes_to_hex(mediator_pubkey_.data(), 33);
    if (contract->keys.mediator_pubkey != our_pubkey) {
        dinero::g_logger.warning("[DaemonMediator] Contract uses different mediator");
        return false;
    }

    uint32_t blocks_elapsed = current_height - contract->created_height;

    if (favor_seller) {
        // Seller window: After seller_window_blocks, daemon signs for seller
        bool should_sign_seller = blocks_elapsed >= contract->seller_window_blocks;

        if (should_sign_seller) {
            dinero::g_logger.info("[DaemonMediator] Seller window reached for " + contract_id +
                                  " (" + std::to_string(blocks_elapsed) + " blocks)");
        }

        return should_sign_seller;
    } else {
        // Buyer refund: After refund_time, daemon signs for buyer
        bool should_sign_buyer = current_height >= contract->refund_time;

        if (should_sign_buyer) {
            dinero::g_logger.info("[DaemonMediator] Refund time reached for " + contract_id +
                                  " (height " + std::to_string(current_height) + ")");
        }

        return should_sign_buyer;
    }
}

std::optional<std::string> DaemonMediator::signInput(
    const std::string& tx_hash,
    uint32_t input_index
) {
    if (!initialized_) {
        dinero::g_logger.error("[DaemonMediator] Cannot sign - not initialized");
        return std::nullopt;
    }

    // Convert tx_hash hex to bytes
    std::vector<uint8_t> hash_bytes = hex_to_bytes(tx_hash);
    if (hash_bytes.size() != 32) {
        dinero::g_logger.error("[DaemonMediator] Invalid tx hash length");
        return std::nullopt;
    }

    // Sign the hash using dinero_crypto_minimal
    unsigned char signature[72];  // Max DER signature size
    size_t sig_len = 72;

    if (!CF_SignDER(mediator_privkey_.data(), hash_bytes.data(), signature, sig_len, 72)) {
        dinero::g_logger.error("[DaemonMediator] Signing failed");
        return std::nullopt;
    }

    dinero::g_logger.info("[DaemonMediator] Signed input " + std::to_string(input_index) +
                          " of tx " + tx_hash.substr(0, 16) + "...");

    return bytes_to_hex(signature, sig_len);
}

// Helper: Convert bytes to hex string
std::string DaemonMediator::bytes_to_hex(const uint8_t* bytes, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i];
    }
    return oss.str();
}

// Helper: Convert hex string to bytes
std::vector<uint8_t> DaemonMediator::hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

} // namespace contracts
} // namespace dinero
