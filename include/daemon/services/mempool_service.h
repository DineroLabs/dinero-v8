#pragma once
#include "daemon/iservice.h"
#include "daemon/interfaces/mempool_access.h"
#include "daemon/mempool.h"
#include "daemon/interfaces/tx_ingress.h"  // Step 5: ITxIngress, IBlockTemplateSource
#include "daemon/interfaces/origin.h"      // Step 5: TxOrigin
#include "policy/fee_estimator.h"
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <map>
#include <thread>

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

    // Long operations retain the service and participate in Stop's drain.
    // Acquire while selected-chain ownership is held, before pool/cache locks.
    // Stop must run outside selected-chain ownership and refuses same-thread
    // shutdown from an active operation. Init/Start remain serialized startup.
    class PoolUse final : public MempoolAccess {
    public:
        ~PoolUse() noexcept override;
        PoolUse(const PoolUse&)=delete;
        PoolUse& operator=(const PoolUse&)=delete;
        Mempool& Pool() const override;
    private:
        friend class MempoolService;
        PoolUse(const MempoolService&,std::shared_ptr<MempoolService>);
        const MempoolService& service_;
        std::shared_ptr<MempoolService> retained_;
        const std::thread::id thread_=std::this_thread::get_id();
        Mempool* pool_=nullptr;
    };
    [[nodiscard]] static std::unique_ptr<PoolUse> AcquirePoolUse(
        std::shared_ptr<MempoolService> service);

    // Access to wrapped mempool
    // Legacy borrowed reference: caller must serialize shutdown. For complete
    // operations use AcquirePoolUse; returning this reference does not pin it.
    Mempool& mempool() {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!accepting_ || !mempool_) {
            throw std::runtime_error("MempoolService::mempool() called before Init()");
        }
        return *mempool_;
    }
    const Mempool& mempool() const {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!accepting_ || !mempool_) {
            throw std::runtime_error("MempoolService::mempool() called before Init()");
        }
        return *mempool_;
    }

    // Check if mempool is initialized (safe to call before Init())
    bool isInitialized() const { std::lock_guard<std::mutex> lock(operation_mutex_); return accepting_ && mempool_ != nullptr; }

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
        auto use=BorrowPoolUse();
        return use->Pool().submitTransaction(tx, source, relay);
    }

    TxAcceptResult SubmitBody(const MempoolTransaction& body, TxOrigin origin) override {
        auto use=BorrowPoolUse();
        return use->Pool().submitBody(body, TxOriginToString(origin),
            origin!=TxOrigin::INTERNAL && origin!=TxOrigin::P2P);
    }

    TxAcceptResult SubmitProofBody(const UtreexoTransactionPayload& payload, TxOrigin origin) {
        auto use=BorrowPoolUse();
        return use->Pool().submitProofBody(payload,TxOriginToString(origin),
            origin!=TxOrigin::INTERNAL && origin!=TxOrigin::P2P);
    }

    // Policy preflight has no admission or relay effects; Submit revalidates.
    std::optional<TxAcceptResult> Test(const Transaction& tx, TxOrigin origin) override {
        std::unique_ptr<PoolUse> use;
        try { use=BorrowPoolUse(); } catch (const PoolUnavailable&) { return std::nullopt; }
        return use->Pool().submitTransactionTestOnly(tx, TxOriginToString(origin));
    }

    bool HasTransaction(const uint256& txid) const override {
        auto use=BorrowPoolUse();
        return use->Pool().hasTransaction(txid);
    }

    /**
     * Get transaction from mempool (ITxIngress interface)
     */
    std::shared_ptr<Transaction> GetTransaction(const uint256& txid) const override {
        auto use=BorrowPoolUse();
        return use->Pool().getTransaction(txid);
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
        auto use=BorrowPoolUse();
        return use->Pool().selectTransactionsForBlock(max_block_size, max_block_weight,
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
        auto use=BorrowPoolUse();
        return use->Pool().submitTransaction(tx, source, relay);
    }

    // Legacy adapter - DEPRECATED, use Submit() instead
    // Returns bool only for backward compatibility during migration
    [[deprecated("Use ITxIngress::Submit() for structured error handling")]]
    bool addTransaction(const Transaction& tx, bool relay = true) {
        auto use=BorrowPoolUse();
        return use->Pool().submitTransaction(tx, "legacy-service", relay).accepted();
    }

    // Legacy accessors (kept for backward compatibility)
    bool hasTransaction(const uint256& txid) const {
        auto use=BorrowPoolUse();
        return use->Pool().hasTransaction(txid);
    }

    std::shared_ptr<Transaction> getTransaction(const uint256& txid) const {
        auto use=BorrowPoolUse();
        return use->Pool().getTransaction(txid);
    }

    size_t size() const {
        auto use=BorrowPoolUse();
        return use->Pool().size();
    }

    std::vector<Transaction> selectTransactionsForBlock(
        size_t max_block_size,
        uint64_t max_block_weight,
        uint32_t next_block_height = 0
    ) const {
        auto use=BorrowPoolUse();
        return use->Pool().selectTransactionsForBlock(max_block_size, max_block_weight,
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
    friend class MempoolServiceOwnerTestPeer;
    class PoolUnavailable final : public std::runtime_error {
    public: PoolUnavailable() : std::runtime_error("Mempool service is unavailable") {}
    };
    std::unique_ptr<PoolUse> BorrowPoolUse() const;
    mutable std::mutex operation_mutex_;
    mutable std::condition_variable operation_changed_;
    mutable std::map<std::thread::id,size_t> operations_by_thread_;
    mutable size_t active_operations_=0;
    bool accepting_=false,stopping_=false;
    std::thread::id stopping_thread_;
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
