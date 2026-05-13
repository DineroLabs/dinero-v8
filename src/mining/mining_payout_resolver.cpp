#include "mining/mining_payout_resolver.h"
#include "mining/address_validator.h"
#include "common/logger.h"
#include <cstdlib>
#include <stdexcept>

namespace dinero {

MiningPayoutResolver::MiningPayoutResolver() = default;

void MiningPayoutResolver::setExplicit(const std::string& bech32) {
    if (!isValidBech32(bech32)) {
        throw std::invalid_argument("Invalid bech32 address: " + bech32);
    }
    explicit_address_ = bech32;
    g_logger.info("Mining payout address set explicitly: " + bech32);
}

std::optional<std::string> MiningPayoutResolver::getExplicit() const {
    return explicit_address_;
}

void MiningPayoutResolver::clearExplicit() {
    if (explicit_address_) {
        g_logger.info("Cleared explicit mining payout address");
        explicit_address_.reset();
    }
}

std::string MiningPayoutResolver::resolvePayoutAddress() {
    // Priority 1: Explicit RPC setting
    if (explicit_address_) {
        g_logger.debug("Using explicit mining payout address: " + *explicit_address_);
        return *explicit_address_;
    }
    
    // Priority 2: Current wallet's next receiving address
    if (get_wallet_next_recv_) {
        try {
            std::string wallet_addr = get_wallet_next_recv_();
            if (!wallet_addr.empty() && isValidBech32(wallet_addr)) {
                g_logger.debug("Using wallet next receiving address for mining: " + wallet_addr);
                return wallet_addr;
            }
        } catch (const std::exception& e) {
            g_logger.warning("Failed to get wallet address for mining: " + std::string(e.what()));
        }
    }
    
    // Priority 3: Environment variable
    std::string env_addr = getEnvironmentAddress();
    if (!env_addr.empty()) {
        if (isValidBech32(env_addr)) {
            g_logger.debug("Using environment mining address: " + env_addr);
            return env_addr;
        } else {
            g_logger.warning("Invalid environment mining address: " + env_addr);
        }
    }
    
    throw std::runtime_error("No mining payout address available. Set one with mining.setpayoutaddress, "
                           "open a wallet, or set DIN_MINING_ADDRESS environment variable.");
}

// Alias for compatibility with your suggested API
std::string MiningPayoutResolver::resolve() const {
    return const_cast<MiningPayoutResolver*>(this)->resolvePayoutAddress();
}

void MiningPayoutResolver::setWalletCallback(std::function<std::string()> callback) {
    get_wallet_next_recv_ = std::move(callback);
}

bool MiningPayoutResolver::hasValidSource() const {
    // Check explicit address
    if (explicit_address_) {
        return true;
    }
    
    // Check wallet callback
    if (get_wallet_next_recv_) {
        try {
            std::string addr = get_wallet_next_recv_();
            if (!addr.empty() && isValidBech32(addr)) {
                return true;
            }
        } catch (...) {
            // Wallet callback failed, continue checking other sources
        }
    }
    
    // Check environment
    std::string env_addr = getEnvironmentAddress();
    return !env_addr.empty() && isValidBech32(env_addr);
}

std::string MiningPayoutResolver::getEnvironmentAddress() const {
    const char* env = std::getenv("DIN_MINING_ADDRESS");
    return env ? std::string(env) : std::string();
}

bool MiningPayoutResolver::isValidBech32(const std::string& address) const {
    // Delegate to the project's full Dinero address validator.
    //
    // The previous implementation only checked the character set and prefix,
    // which accepts strings like "din1qqqqqqqq..." that have a valid charset
    // but fail Bech32 checksum verification. Combined with the env-var override
    // DIN_MINING_ADDRESS, this allowed typos or injected addresses to silently
    // redirect block rewards.
    //
    // IsValidDineroAddress performs full Bech32 decoding (HRP + checksum +
    // witness version + program length) for all Dinero networks (din, tdin,
    // rdin) and also validates legacy Base58Check addresses.
    return dinero::mining::IsValidDineroAddress(address);
}

} // namespace dinero
