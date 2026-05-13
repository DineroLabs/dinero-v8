#pragma once
#include "daemon/iservice.h"
#include "daemon/mempool.h"
#include "daemon/interfaces/tx_ingress.h"  // Step 5: ITxIngress, IBlockTemplateSource
#include "daemon/interfaces/origin.h"      // Step 5: TxOrigin
#include "policy/fee_estimator.h"
#include <memory>
#include <string>

namespace dinero {

// Forward declarations
class ILogger;

/**
 * MempoolService - IService wrapper for Mempool
 *
 * Wraps the existing Mempool class into the IService lifecycle.
 * Manages unconfirmed transactions and provides transaction selection
 * for block templates.
 *
 * Step 5: Implements ITxIngress and IBlockTemplateSource interfaces.
 * External code should access via these interfaces, not the service directly.
 *
 * Dependencies: Logger, Config, Chainstate
 *
 * Initialization order:
 * - Init() creates Mempool instance with blockchain reference
 * - Start() initializes mempool and loads any persisted transactions
 * - Stop() performs clean shutdown and optionally persists mempool
 */
class MempoolService : public IService, public ITxIngress, public IBlockTemplateSource {
public:
    MempoolService() = default;
    ~MempoolService() override = default;

    std::string Name() const override { return "Mempool"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Service health
    bool IsHealthy() const override;
    std::string GetMetrics() const override;

    // Access to wrapped mempool
    // NOTE: Only valid after Init() has been called
    Mempool& mempool() {
        if (!mempool_) {
            throw std::runtime_error("MempoolService::mempool() called before Init()");
        }
        return *mempool_;
    }
    const Mempool& mempool() const {
        if (!mempool_) {
            throw std::runtime_error("MempoolService::mempool() called before Init()");
        }
        return *mempool_;
    }

    // Check if mempool is initialized (safe to call before Init())
    bool isInitialized() const { return mempool_ != nullptr; }

    // ========================================================================
    // ITxIngress INTERFACE IMPLEMENTATION (Step 5)
    // ========================================================================
    // Submit() is the canonical entry point via the interface.
    // External code should use ITxIngress*, not MempoolService* directly.
    // ========================================================================

    /**
     * Submit transaction to mempool (ITxIngress interface)
     *
     * @param tx      Transaction to submit
     * @param origin  Origin type (RPC, P2P, WALLET, etc.)
     * @return        Structured result with rejection code and message
     */
    TxAcceptResult Submit(const Transaction& tx, TxOrigin origin) override {
        // Convert TxOrigin to string source identifier
        const char* source = TxOriginToString(origin);
        // Relay policy:
        //   INTERNAL: No relay (internal operations)
        //   P2P: No relay (P2P layer handles relay manually to exclude sender)
        //   RPC/GRPC/WALLET: Auto-relay
        bool relay = (origin != TxOrigin::INTERNAL && origin != TxOrigin::P2P);
        return mempool_->submitTransaction(tx, source, relay);
    }

    /**
     * Check if transaction is in mempool (ITxIngress interface)
     */
    bool HasTransaction(const uint256& txid) const override {
        return mempool_->hasTransaction(txid);
    }

    /**
     * Get transaction from mempool (ITxIngress interface)
     */
    std::shared_ptr<Transaction> GetTransaction(const uint256& txid) const override {
        return mempool_->getTransaction(txid);
    }

    // ========================================================================
    // IBlockTemplateSource INTERFACE IMPLEMENTATION (Step 5)
    // ========================================================================

    /**
     * Select transactions for block template (IBlockTemplateSource interface)
     */
    std::vector<Transaction> SelectTransactionsForBlock(
        size_t max_block_size = 1000000,
        uint64_t max_block_weight = 4000000,
        uint32_t next_block_height = 0
    ) const override {
        return mempool_->selectTransactionsForBlock(max_block_size, max_block_weight,
                                                    next_block_height);
    }

    // ========================================================================
    // LEGACY METHODS (backward compatibility during migration)
    // ========================================================================
    // These will be removed once all consumers migrate to interfaces.
    // ========================================================================

    /**
     * Submit transaction (legacy string-based source)
     * @deprecated Use ITxIngress::Submit() with TxOrigin instead
     */
    TxAcceptResult submitTransaction(const Transaction& tx, const std::string& source, bool relay = true) {
        return mempool_->submitTransaction(tx, source, relay);
    }

    // Legacy adapter - DEPRECATED, use Submit() instead
    // Returns bool only for backward compatibility during migration
    [[deprecated("Use ITxIngress::Submit() for structured error handling")]]
    bool addTransaction(const Transaction& tx, bool relay = true) {
        return mempool_->submitTransaction(tx, "legacy-service", relay).accepted();
    }

    // Legacy accessors (kept for backward compatibility)
    bool hasTransaction(const uint256& txid) const {
        return mempool_->hasTransaction(txid);
    }

    std::shared_ptr<Transaction> getTransaction(const uint256& txid) const {
        return mempool_->getTransaction(txid);
    }

    size_t size() const {
        return mempool_->size();
    }

    std::vector<Transaction> selectTransactionsForBlock(
        size_t max_block_size,
        uint64_t max_block_weight,
        uint32_t next_block_height = 0
    ) const {
        return mempool_->selectTransactionsForBlock(max_block_size, max_block_weight,
                                                    next_block_height);
    }

    // ═══════════════════════════════════════════════════════════════
    // Fee Estimation (Phase 34)
    // ═══════════════════════════════════════════════════════════════

    /**
     * @brief Get the fee estimator instance
     * @return Shared pointer to fee estimator, or nullptr if not initialized
     */
    std::shared_ptr<policy::FeeEstimator> getFeeEstimator() const {
        return fee_estimator_;
    }

    /**
     * @brief Record a transaction entering the mempool for fee estimation
     * @param txid Transaction ID
     * @param fee_rate Fee rate in una per KB
     * @param current_height Current block height
     */
    void recordMempoolTransaction(const std::string& txid, uint64_t fee_rate, uint32_t current_height);

    /**
     * @brief Record a transaction being confirmed for fee estimation
     * @param txid Transaction ID
     * @param confirm_height Confirmation block height
     */
    void recordConfirmedTransaction(const std::string& txid, uint32_t confirm_height);

    // ═══════════════════════════════════════════════════════════════
    // Phase G.3: Transaction Relay Integration
    // ═══════════════════════════════════════════════════════════════

    /**
     * @brief Set TxRelayManager for transaction announcements
     * @param tx_relay Shared pointer to TxRelayManager
     */
    void setTxRelayManager(std::shared_ptr<class TxRelayManager> tx_relay);

private:
    std::unique_ptr<Mempool> mempool_;

    // Logger dependencies (dual pattern during migration):
    // - logger_: Legacy LoggerService (keep for compatibility during migration)
    // - logger_interface_: New ILogger dependency injection (actively used)
    std::shared_ptr<class LoggerService> logger_;
    ILogger* logger_interface_ = nullptr;

    std::shared_ptr<class ConfigService> config_;
    std::shared_ptr<class ChainstateService> chainstate_;

    // Phase 34: Fee estimation
    std::shared_ptr<policy::FeeEstimator> fee_estimator_;

    // Phase G.3: Transaction relay
    std::shared_ptr<class TxRelayManager> tx_relay_manager_;

    // P2P service for transaction broadcast
    std::shared_ptr<class P2PService> p2p_service_;

    bool started_ = false;

    // Internal helper: broadcast transaction via P2P (sends inv, peers request full tx)
    void broadcastTxViaP2P(const uint256& txid);
};

} // namespace dinero
