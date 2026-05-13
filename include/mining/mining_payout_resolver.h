#pragma once

#include <string>
#include <optional>
#include <functional>

namespace dinero {

/**
 * @brief Resolves mining payout addresses with intelligent fallbacks
 * 
 * Priority order:
 * 1. Explicit RPC setting (sticky): mining.setpayoutaddress
 * 2. Current wallet's next external address (HD derivation)
 * 3. Environment variable: DIN_MINING_ADDRESS
 * 
 * This guarantees "it just works" out of the box while allowing power users
 * to point mining to cold wallets or external addresses.
 */
class MiningPayoutResolver {
public:
    MiningPayoutResolver();
    ~MiningPayoutResolver() = default;

    /**
     * @brief Set explicit payout address (highest priority)
     * @param bech32 Valid bech32 address (din1...)
     */
    void setExplicit(const std::string& bech32);
    
    /**
     * @brief Get current explicit payout address
     * @return Optional explicit address if set
     */
    std::optional<std::string> getExplicit() const;
    
    /**
     * @brief Clear explicit payout address (fall back to wallet/env)
     */
    void clearExplicit();

    /**
     * @brief Resolve payout address using priority order
     * @return Valid bech32 address for mining payouts
     * @throws std::runtime_error if no address available
     */
    std::string resolvePayoutAddress();
    
    /**
     * @brief Alias for resolvePayoutAddress (const version)
     * @return Valid bech32 address for mining payouts
     * @throws std::runtime_error if no address available
     */
    std::string resolve() const;

    /**
     * @brief Set wallet callback for next receiving address
     * @param callback Function that returns next bech32 address from current wallet
     */
    void setWalletCallback(std::function<std::string()> callback);

    /**
     * @brief Check if resolver has any valid address source
     * @return true if resolvePayoutAddress() would succeed
     */
    bool hasValidSource() const;

private:
    std::optional<std::string> explicit_address_;
    std::function<std::string()> get_wallet_next_recv_;
    
    // Helper methods
    std::string getEnvironmentAddress() const;
    bool isValidBech32(const std::string& address) const;
};

} // namespace dinero
