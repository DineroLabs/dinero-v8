#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace dinero {

/**
 * @brief Single payout entry for daemon-defined pool payouts
 *
 * Represents one recipient in a weighted payout scheme.
 * The daemon constructs coinbase outputs from these entries.
 */
struct PayoutEntry {
    std::string address;    // Bech32 address (din1...)
    uint32_t weight;        // Relative weight (must be > 0)

    PayoutEntry() : weight(0) {}
    PayoutEntry(const std::string& addr, uint32_t w) : address(addr), weight(w) {}

    bool operator==(const PayoutEntry& other) const {
        return address == other.address && weight == other.weight;
    }
};

/**
 * @brief Resolved payout with calculated amount
 *
 * Result of applying weights to total reward.
 * Used by BlockAssembler to construct coinbase outputs.
 */
struct ResolvedPayout {
    std::string address;    // Bech32 address
    uint64_t amount;        // Amount in una (1 DIN = 100,000,000 una)

    ResolvedPayout() : amount(0) {}
    ResolvedPayout(const std::string& addr, uint64_t amt) : address(addr), amount(amt) {}
};

/**
 * @brief Pool payout specification
 *
 * Core principle (non-negotiable):
 * - Pools request payout parameters
 * - The daemon constructs the coinbase outputs
 * - Miners and stratum never build coinbase outputs
 *
 * This preserves:
 * - txid stability (Utreexo correctness)
 * - auditability
 * - determinism
 *
 * Supported payout types (v1):
 * - Single payout address (solo / simple pool)
 * - Weighted split (PPS / proportional pools)
 *
 * No dynamic scripting. No miner-supplied outputs.
 */
class PayoutSpec {
public:
    // ========================================================================
    // Configuration limits (guardrails)
    // ========================================================================
    static constexpr uint32_t MAX_PAYOUT_ENTRIES = 20;
    static constexpr uint32_t MAX_SCRIPT_SIZE = 100;  // bytes per output script

    // ========================================================================
    // Construction
    // ========================================================================

    PayoutSpec() = default;

    /**
     * @brief Create single-address payout (solo mining)
     * @param address Bech32 address for full reward
     */
    static PayoutSpec Single(const std::string& address);

    /**
     * @brief Create weighted split payout (pool mining)
     * @param entries Vector of (address, weight) pairs
     */
    static PayoutSpec Weighted(const std::vector<PayoutEntry>& entries);

    // ========================================================================
    // Validation
    // ========================================================================

    /**
     * @brief Validation result with error details
     */
    struct ValidationResult {
        bool valid;
        std::string error;

        ValidationResult() : valid(true) {}
        ValidationResult(bool v, const std::string& e) : valid(v), error(e) {}

        static ValidationResult Ok() { return ValidationResult(true, ""); }
        static ValidationResult Error(const std::string& msg) {
            return ValidationResult(false, msg);
        }

        explicit operator bool() const { return valid; }
    };

    /**
     * @brief Validate the payout specification
     *
     * Checks:
     * - At least one entry
     * - No more than MAX_PAYOUT_ENTRIES entries
     * - All addresses valid bech32
     * - All weights > 0
     * - No duplicate addresses
     *
     * @return ValidationResult with success or error message
     */
    ValidationResult Validate() const;

    /**
     * @brief Check if spec has valid entries
     */
    bool IsValid() const { return Validate().valid; }

    /**
     * @brief Check if this is a single-address payout
     */
    bool IsSinglePayout() const { return entries_.size() == 1; }

    // ========================================================================
    // Weight normalization and amount calculation
    // ========================================================================

    /**
     * @brief Resolve payouts for a given total reward
     *
     * Deterministic algorithm:
     * 1. Compute total_weight = sum of all weights
     * 2. For each entry: share_i = floor(total_reward * weight_i / total_weight)
     * 3. Remainder = total_reward - sum(shares)
     * 4. Assign remainder to first entry (deterministic)
     *
     * Guarantees:
     * - sum(resolved amounts) == total_reward (exact)
     * - deterministic: same inputs → same outputs
     * - no floating point (integer arithmetic only)
     *
     * @param total_reward Total coinbase value (subsidy + fees) in una
     * @return Vector of resolved payouts, or empty on error
     */
    std::vector<ResolvedPayout> Resolve(uint64_t total_reward) const;

    /**
     * @brief Get total weight sum
     */
    uint64_t TotalWeight() const;

    // ========================================================================
    // Accessors
    // ========================================================================

    const std::vector<PayoutEntry>& Entries() const { return entries_; }
    size_t Count() const { return entries_.size(); }
    bool Empty() const { return entries_.empty(); }

    // ========================================================================
    // Serialization (for RPC/logging)
    // ========================================================================

    /**
     * @brief Convert to JSON-compatible representation
     *
     * Format:
     * [
     *   { "address": "din1...", "weight": 70 },
     *   { "address": "din1...", "weight": 30 }
     * ]
     */
    std::string ToJson() const;

    /**
     * @brief Parse from JSON representation
     * @return PayoutSpec or nullopt on parse error
     */
    static std::optional<PayoutSpec> FromJson(const std::string& json);

private:
    std::vector<PayoutEntry> entries_;
};

// ============================================================================
// Address validation helper (forward declaration)
// ============================================================================

/**
 * @brief Validate a bech32 address for payout
 *
 * Checks:
 * - Valid bech32 encoding
 * - Correct HRP for network (din/tdin/rdin)
 * - Valid witness version and program
 *
 * @param address Address to validate
 * @return true if valid for payout
 */
bool IsValidPayoutAddress(const std::string& address);

} // namespace dinero
