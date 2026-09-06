#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <nlohmann/json.hpp>

namespace dinero {
namespace solo {

/**
 * RPC client configuration
 */
struct RpcConfig {
    std::string url = "http://127.0.0.1:20998";  // Default mainnet RPC port
    std::string cookie_path;                      // Path to .cookie file (preferred)
    std::string user;                             // RPC username (fallback)
    std::string password;                         // RPC password (fallback)
    int timeout_seconds = 30;
    int submit_timeout_seconds = 120;
};

/**
 * Chain safety check result
 */
struct ChainSafetyCheck {
    bool safe = false;
    std::string network;
    std::string genesis_hash;
    std::string error;
};

enum class BlockChainStatus {
    Active,
    ConflictingActiveBlock,
    Unknown,
};

BlockChainStatus classifyBlockChainStatus(
    const std::string& candidate_hash,
    const std::optional<std::string>& active_hash);

/**
 * JSON-RPC client for DineroCoin daemon
 *
 * Supports:
 * - Cookie authentication (preferred)
 * - Username/password authentication (fallback)
 * - getblocktemplate, submitblock, getmininginfo
 */
class RpcClient {
public:
    explicit RpcClient(const RpcConfig& config);
    ~RpcClient();

    // Disable copy
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    /**
     * Test connection to daemon
     */
    bool isConnected();

    /**
     * Get block template for mining
     * @param address Payout address (required)
     * @return Template JSON or nullopt on error
     */
    std::optional<nlohmann::json> getBlockTemplate(const std::string& address);

    /**
     * Get block template with BIP22/BIP23 longpolling.
     *
     * When longpollid is non-empty AND matches the server's current tip,
     * the server blocks the response until the tip advances or an ~8s
     * timeout fires (see dinero p2p-fix 8bad44f15). Empty longpollid =
     * immediate return (same as the one-arg overload).
     *
     * @param address    Payout address (required)
     * @param longpollid Tip token from previous response, or "" to disable longpoll
     * @return Template JSON or nullopt on error
     */
    std::optional<nlohmann::json> getBlockTemplate(const std::string& address,
                                                   const std::string& longpollid);

    /**
     * Submit mined block
     * @param block_hex Serialized block in hex
     * @return true if accepted, false otherwise
     */
    bool submitBlock(const std::string& block_hex);

    /**
     * Reconcile an ambiguous submission against the active chain.
     * A different hash at the candidate height means the candidate is stale;
     * an unavailable/not-yet-reached height remains unknown.
     */
    BlockChainStatus getBlockChainStatus(const std::string& candidate_hash,
                                         uint32_t height);

    /**
     * Get mining info (difficulty, network hashrate, etc.)
     */
    std::optional<nlohmann::json> getMiningInfo();

    /**
     * Get blockchain info
     */
    std::optional<nlohmann::json> getBlockchainInfo();

    /**
     * Verify daemon chain identity using network + genesis hash
     */
    ChainSafetyCheck verifyChainSafety();

    /**
     * Get last error message
     */
    std::string getLastError() const { return last_error_; }

    /**
     * Whether the last RPC call failed due to timeout
     */
    bool lastCallTimedOut() const { return last_call_timed_out_; }

private:
    /**
     * Make JSON-RPC call
     */
    std::optional<nlohmann::json> call(const std::string& method,
                                        const nlohmann::json& params = nlohmann::json::array(),
                                        int timeout_override_seconds = -1);

    /**
     * Get block hash by height (supports namespaced and non-namespaced RPC)
     */
    std::optional<std::string> getBlockHash(uint32_t height);

    /**
     * Load credentials from cookie file
     */
    bool loadCookie();

    RpcConfig config_;
    std::string auth_header_;
    std::string last_error_;
    bool last_call_timed_out_ = false;
    bool cookie_loaded_ = false;
};

} // namespace solo
} // namespace dinero
