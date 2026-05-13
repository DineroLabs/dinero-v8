#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace dinero {

// Forward declarations
struct Transaction;
struct TxInput;
struct TxOutput;

namespace policy {

/**
 * Transaction validation result
 */
enum class ValidationResult {
    VALID,
    INVALID_SIZE,
    INVALID_WEIGHT,
    INVALID_SIGOPS,
    INVALID_FEE,
    INVALID_SCRIPT,
    INVALID_DUST,
    INVALID_RBF,
    ALREADY_IN_MEMPOOL,
    CONFLICTING_TX
};

/**
 * Mempool policy configuration
 */
struct MempoolConfig {
    // Size limits
    uint64_t max_mempool_size = 300 * 1024 * 1024; // 300MB
    uint32_t max_tx_size = 100 * 1024; // 100KB
    uint32_t max_tx_weight = 400 * 1024; // 400K weight units
    
    // Fee policy
    uint64_t min_relay_fee_rate = 1000; // una per KB
    uint64_t dust_threshold = 546; // una
    
    // Script limits
    uint32_t max_sigops = 20000;
    uint32_t max_script_size = 10000;
    
    // RBF policy (Replace-By-Fee)
    // Default: OFF - opt-in only for user trust and payment finality
    // Enable via config: mempool.enable_rbf=true
    bool enable_rbf = false;
    double rbf_fee_increment = 1.25; // 25% increase required

    // CPFP policy (Child-Pays-For-Parent)
    // Default: ON - non-controversial fee bumping mechanism
    bool enable_cpfp = true;
    uint32_t max_ancestor_count = 25;
    uint32_t max_descendant_count = 25;
    uint64_t max_ancestor_size = 101 * 1024; // 101KB
    uint64_t max_descendant_size = 101 * 1024; // 101KB
};

/**
 * Transaction standardness and policy checker
 */
class MempoolPolicy {
public:
    explicit MempoolPolicy(const MempoolConfig& config = MempoolConfig{});
    
    /**
     * Check if transaction meets standardness rules
     */
    ValidationResult checkStandardness(const Transaction& tx) const;
    
    /**
     * Check transaction size and weight limits
     */
    ValidationResult checkSizeAndWeight(const Transaction& tx) const;
    
    /**
     * Check signature operation limits
     */
    ValidationResult checkSigOps(const Transaction& tx) const;
    
    /**
     * Check fee requirements
     */
    ValidationResult checkFees(const Transaction& tx, uint64_t fee) const;
    
    /**
     * Check for dust outputs
     */
    ValidationResult checkDust(const Transaction& tx) const;
    
    // RBF validation removed - use RBFPolicy for BIP125-compliant RBF checking
    // ValidationResult checkRBF(const Transaction& new_tx, const Transaction& existing_tx) const;
    
    /**
     * Check CPFP (Child-Pays-For-Parent) limits
     */
    ValidationResult checkCPFP(const Transaction& tx, 
                               uint32_t ancestor_count,
                               uint32_t descendant_count,
                               uint64_t ancestor_size,
                               uint64_t descendant_size) const;
    
    /**
     * Comprehensive policy validation
     */
    ValidationResult validateTransaction(const Transaction& tx, uint64_t fee) const;
    
    /**
     * Get human-readable error message
     */
    std::string getErrorMessage(ValidationResult result) const;
    
    // Configuration access
    const MempoolConfig& getConfig() const { return config_; }
    void updateConfig(const MempoolConfig& config) { config_ = config; }

private:
    MempoolConfig config_;
    
    // Helper methods
    uint32_t calculateTxWeight(const Transaction& tx) const;
    uint32_t calculateSigOps(const Transaction& tx) const;
    bool isStandardScript(const std::vector<uint8_t>& script) const;
    bool isDustOutput(const TxOutput& output) const;
    bool hasRBFSignal(const Transaction& tx) const;
};

/**
 * Mempool eviction policy for when mempool is full
 */
class EvictionPolicy {
public:
    struct EvictionCandidate {
        std::string txid;
        uint64_t fee_rate;
        uint64_t size;
        uint64_t time_added;
        uint32_t descendant_count;
        bool has_unconfirmed_parents;
    };
    
    /**
     * Select transactions for eviction when mempool is full
     */
    std::vector<std::string> selectForEviction(
        const std::vector<EvictionCandidate>& candidates,
        uint64_t bytes_to_free
    ) const;
    
private:
    // Eviction scoring (lower score = more likely to be evicted)
    double calculateEvictionScore(const EvictionCandidate& candidate) const;
};

} // namespace policy
} // namespace dinero
