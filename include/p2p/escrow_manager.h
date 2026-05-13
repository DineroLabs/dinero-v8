#pragma once

#include <string>
#include <optional>
#include <map>
#include <mutex>
#include <cstdint>
#include <vector>

namespace dinero {
namespace p2p {

/**
 * EscrowStatus - Current state of an escrow transaction
 */
enum class EscrowStatus {
    PENDING,      // Waiting for confirmations
    LOCKED,       // Funds locked, offer active
    RELEASED,     // Funds released to buyer
    REFUNDED,     // Funds returned to seller
    EXPIRED       // Escrow expired, auto-refund pending
};

/**
 * EscrowInfo - Complete information about an escrow transaction
 */
struct EscrowInfo {
    std::string escrow_id;          // Unique escrow identifier
    std::string seller_address;     // Original seller address
    std::string buyer_address;      // Buyer address (set when accepted)
    std::string escrow_address;     // Time-locked escrow address
    std::string lock_txid;          // Transaction ID of lock
    std::string release_txid;       // Transaction ID of release (if released)
    double amount;                  // DIN amount locked
    uint64_t lock_time;             // Unix timestamp when funds unlock
    uint64_t created_at;            // Unix timestamp of creation
    uint64_t expires_at;            // Unix timestamp of expiry
    int confirmations;              // Number of confirmations
    EscrowStatus status;            // Current status
    std::string offer_id;           // Associated P2P offer ID
};

/**
 * EscrowManager - Manages time-locked escrow transactions for P2P trades
 *
 * Features:
 * - Creates time-locked escrow transactions
 * - Releases funds to buyer after trade completion
 * - Refunds seller if trade cancelled or expired
 * - Tracks all active escrows
 * - Auto-cleanup of expired escrows
 *
 * Security:
 * - Uses CHECKLOCKTIMEVERIFY (CLTV) for on-chain timelocks
 * - Funds provably locked on blockchain
 * - No custodial risk - fully decentralized
 * - Automatic expiry protection
 */
class EscrowManager {
public:
    static EscrowManager& instance();

    /**
     * Create a new escrow transaction
     *
     * @param seller_address Address of seller
     * @param amount DIN amount to lock
     * @param duration_seconds How long to lock (e.g., 86400 = 24 hours)
     * @param offer_id Associated P2P offer ID
     * @return EscrowInfo with escrow details, or nullopt on failure
     */
    std::optional<EscrowInfo> createEscrow(
        const std::string& seller_address,
        double amount,
        uint64_t duration_seconds,
        const std::string& offer_id
    );

    /**
     * Release escrow funds to buyer
     *
     * @param escrow_id Unique escrow identifier
     * @param buyer_address Destination address
     * @return Transaction ID of release, or nullopt on failure
     */
    std::optional<std::string> releaseEscrow(
        const std::string& escrow_id,
        const std::string& buyer_address
    );

    /**
     * Refund escrow funds to seller (cancel/expire)
     *
     * @param escrow_id Unique escrow identifier
     * @return Transaction ID of refund, or nullopt on failure
     */
    std::optional<std::string> refundEscrow(
        const std::string& escrow_id
    );

    /**
     * Get escrow information
     *
     * @param escrow_id Unique escrow identifier
     * @return EscrowInfo, or nullopt if not found
     */
    std::optional<EscrowInfo> getEscrow(const std::string& escrow_id);

    /**
     * List all active escrows (for a specific address or all)
     *
     * @param address Optional filter by seller address
     * @return Vector of active escrows
     */
    std::vector<EscrowInfo> listEscrows(const std::string& address = "");

    /**
     * Verify escrow is valid and locked
     *
     * @param escrow_id Unique escrow identifier
     * @return true if escrow is valid and locked
     */
    bool verifyEscrow(const std::string& escrow_id);

    /**
     * Process expired escrows (auto-refund)
     * Should be called periodically by daemon
     */
    void processExpiredEscrows();

    /**
     * Get total value locked in escrows
     *
     * @return Total DIN locked across all escrows
     */
    double getTotalLocked();

private:
    EscrowManager() = default;
    ~EscrowManager() = default;

    // Generate unique escrow ID
    std::string generateEscrowId();

    // Generate time-locked escrow address
    std::string generateEscrowAddress(uint64_t lock_time);

    // Create time-locked transaction
    std::string createTimeLockTransaction(
        const std::string& from_address,
        const std::string& escrow_address,
        double amount,
        uint64_t lock_time
    );

    // Check if escrow has expired
    bool isExpired(const EscrowInfo& escrow);

    // Update escrow status based on confirmations
    void updateEscrowStatus(EscrowInfo& escrow);

    std::map<std::string, EscrowInfo> escrows_;  // escrow_id → info

    // Use function-local static for mutex to avoid static initialization order issues
    static std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }

    static constexpr int MIN_CONFIRMATIONS = 3;  // Minimum confirmations for locked status
    static constexpr uint64_t DEFAULT_LOCK_TIME = 86400;  // 24 hours default
    static constexpr uint64_t MAX_LOCK_TIME = 604800;  // 7 days maximum
};

} // namespace p2p
} // namespace dinero
