#include "daemon/services/mempool_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/p2p_service.h"  // For tx broadcast
#include "daemon/daemon_context.h"
#include "daemon/config.h"
#include "common/ilogger.h"  // For ILogger interface dependency injection
#include <sstream>
#include <iostream>
#include <ctime>

namespace dinero {

MempoolService::PoolUse::PoolUse(const MempoolService& service,
                                std::shared_ptr<MempoolService> retained)
    : service_(service),retained_(std::move(retained)) {
    std::lock_guard<std::mutex> lock(service_.operation_mutex_);
    const auto current=service_.operations_by_thread_.find(thread_);
    // A callback inside an already-owned operation may finish during drain.
    // A new independent operation must not enter a stopping service.
    if ((!service_.accepting_ && current==service_.operations_by_thread_.end()) || !service_.mempool_)
        throw PoolUnavailable();
    ++service_.operations_by_thread_[thread_];
    ++service_.active_operations_;
    pool_=service_.mempool_.get();
}
MempoolService::PoolUse::~PoolUse() noexcept {
    if (thread_!=std::this_thread::get_id()) std::terminate();
    std::lock_guard<std::mutex> lock(service_.operation_mutex_);
    auto current=service_.operations_by_thread_.find(thread_);
    if (current==service_.operations_by_thread_.end() || !current->second || !service_.active_operations_)
        std::terminate();
    if (!--current->second) service_.operations_by_thread_.erase(current);
    --service_.active_operations_;
    service_.operation_changed_.notify_all();
}
Mempool& MempoolService::PoolUse::Pool() const {
    if (thread_!=std::this_thread::get_id()) throw std::logic_error("Mempool operation is thread-affine");
    return *pool_;
}
std::unique_ptr<MempoolService::PoolUse> MempoolService::AcquirePoolUse(
    std::shared_ptr<MempoolService> service) {
    if (!service) throw PoolUnavailable();
    const auto* ptr=service.get();
    return std::unique_ptr<PoolUse>(new PoolUse(*ptr,std::move(service)));
}
std::unique_ptr<MempoolService::PoolUse> MempoolService::BorrowPoolUse() const {
    // Synchronous member call: its caller keeps this object alive until return.
    return std::unique_ptr<PoolUse>(new PoolUse(*this,{}));
}

bool MempoolService::Init(DaemonContext& ctx) {
    { std::lock_guard<std::mutex> lock(operation_mutex_); if (mempool_ || accepting_ || stopping_) return false; }
    // Store dependencies
    if (ctx.logger) {
        logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    }
    // Use dedicated mempool logger if available, fallback to shared logger
    logger_interface_ = ctx.mempool_logger ? ctx.mempool_logger : ctx.logger_interface;

    if (ctx.config) {
        config_ = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    }
    if (ctx.chainstate) {
        chainstate_ = std::dynamic_pointer_cast<ChainstateService>(ctx.chainstate);
    }
    if (ctx.p2p) {
        p2p_service_ = std::dynamic_pointer_cast<P2PService>(ctx.p2p);
    }

    if (!logger_interface_ || !config_ || !chainstate_) {
        if (!logger_interface_) {
            std::cerr << "[MempoolService] Logger interface dependency missing" << std::endl;
        } else {
            std::cerr << "[MempoolService] Missing required dependencies" << std::endl;
        }
        return false;
    }

    // Create Mempool instance with ChainDB
    try {
        // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
        ChainDB* chain_db = chainstate_ ? chainstate_->GetChainDB() : nullptr;
        if (!chain_db) {
            logger_interface_->error("[MempoolService] ChainDB not available from ChainstateService");
            return false;
        }

        mempool_ = std::make_unique<Mempool>(
            chain_db,
            chainstate_->GetConsensusUTXOSet(),
            GetConfig().utreexo_stateless);
        mempool_->setLogger(logger_interface_);  // Inject logger for dependency injection
        std::weak_ptr<ChainstateService> weak_chainstate = chainstate_;
        mempool_->setChainstateReadGuardFactory([weak_chainstate]()
            -> std::unique_ptr<Mempool::ChainstateReadGuard> {
            const auto chainstate = weak_chainstate.lock();
            if (!chainstate) return nullptr;
            return ChainstateService::AcquireMempoolChainstateRead(chainstate);
        });
        mempool_->setPreBaseCoinResolver(
            [weak_chainstate](const OutPoint& outpoint)
                -> std::optional<consensus::UTXOEntry> {
                const auto chainstate = weak_chainstate.lock();
                return chainstate
                    ? chainstate->ResolveLivePreBaseCoin(outpoint)
                    : std::nullopt;
            });
        mempool_->setPreBaseCoinStatusResolver(
            [weak_chainstate](const OutPoint& outpoint) -> StatusOr<consensus::UTXOEntry> {
                const auto chainstate = weak_chainstate.lock();
                return chainstate ? chainstate->ResolveLivePreBaseCoinChecked(outpoint)
                                  : StatusOr<consensus::UTXOEntry>(Status::Internal);
            });
        mempool_->setPreBaseCoinPredicate(
            [weak_chainstate](const OutPoint& outpoint) {
                const auto chainstate = weak_chainstate.lock();
                return chainstate &&
                       chainstate->ResolvePreBaseCoinForUndo(outpoint).has_value();
            });
        logger_interface_->info(
            std::string("[MempoolService] Mempool instance created with active consensus UTXO view") +
            (GetConfig().utreexo_stateless ? " and stateless ChainDB fallback" : ""));

        // Apply RBF/CPFP policy from config
        // Default: RBF off (preserves payment finality), CPFP on (safe fee bumping)
        if (config_) {
            bool rbf_enabled = config_->GetBool("mempool.enable_rbf", false);
            mempool_->setRBFEnabled(rbf_enabled);
            logger_interface_->info("[MempoolService] RBF policy: " +
                std::string(rbf_enabled ? "ENABLED (opt-in)" : "DISABLED (default)"));
            logger_interface_->info("[MempoolService] CPFP policy: ENABLED (always)");
        }

        // Phase 34: Initialize fee estimator
        fee_estimator_ = std::make_shared<policy::FeeEstimator>(10);  // Require 10 samples minimum
        logger_interface_->info("[MempoolService] Fee estimator initialized");

        // Wire transaction broadcast callback via P2PService
        if (p2p_service_) {
            mempool_->setTxBroadcastCallback(
                [this](const uint256& txid) {
                    this->broadcastTxViaP2P(txid);
                }
            );
            logger_interface_->info("[MempoolService] Transaction broadcast callback wired to P2P");
        } else {
            logger_interface_->warning("[MempoolService] P2P service not available - tx relay disabled");
        }

    } catch (const std::exception& e) {
        logger_interface_->error("[MempoolService] Failed to create Mempool: " +
                      std::string(e.what()));
        return false;
    }

    logger_interface_->info("[MempoolService] Initialized successfully");
    { std::lock_guard<std::mutex> lock(operation_mutex_); accepting_=true; }
    return true;
}

bool MempoolService::Start() {
    std::unique_ptr<PoolUse> use;
    try { use=BorrowPoolUse(); } catch (const PoolUnavailable&) { return false; }
    { std::lock_guard<std::mutex> lock(operation_mutex_); if (started_ || stopping_) return false; }

    logger_interface_->info("[MempoolService] Starting mempool...");

    // Mempool doesn't require explicit initialization beyond construction
    // but we could load persisted transactions here if needed

    size_t initial_size = use->Pool().size();
    logger_interface_->info("[MempoolService] Mempool started successfully");
    logger_interface_->info("[MempoolService]   Initial transaction count: " +
                 std::to_string(initial_size));

    { std::lock_guard<std::mutex> lock(operation_mutex_); started_ = true; }
    return true;
}

void MempoolService::Stop() {
    {
        std::unique_lock<std::mutex> lock(operation_mutex_);
        if (operations_by_thread_.count(std::this_thread::get_id()))
            throw std::logic_error("Cannot stop mempool inside an active operation");
        if (stopping_) {
            if (stopping_thread_==std::this_thread::get_id())
                throw std::logic_error("Recursive mempool shutdown is not available");
            operation_changed_.wait(lock,[&]{return !stopping_;});
            return;
        }
        if (!mempool_) return;
        accepting_=false;
        stopping_=true;
        stopping_thread_=std::this_thread::get_id();
        operation_changed_.notify_all();
        operation_changed_.wait(lock,[&]{return active_operations_==0;});
    }
    // No operation can enter; no selected-chain/pool lock is held while waiting
    // above. Logging failure must not strand the service in its stopping state.
    try {
        if (logger_interface_) {
            logger_interface_->info("[MempoolService] Shutting down mempool...");
            logger_interface_->info("[MempoolService] Final transaction count: " + std::to_string(mempool_->size()));
            logger_interface_->info("[MempoolService] Total fees in mempool: " + std::to_string(mempool_->getTotalFees()) + " una");
        }
    } catch (...) { /* Diagnostics cannot prevent owned shutdown. */ }
    // clear()/destruction must not invoke a fallible external logger after drain.
    mempool_->setLogger(nullptr);
    mempool_->clear();
    std::unique_ptr<Mempool> retired;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        retired=std::move(mempool_);
        started_=false;
    }
    // Destroy callbacks and dependency owners outside the service gate.
    retired.reset();
    // Existing daemon order has stopped ingress/P2P before this point.
    tx_relay_manager_.reset();
    p2p_service_.reset();
    chainstate_.reset();
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        stopping_=false;
        stopping_thread_={};
        operation_changed_.notify_all();
    }
}

bool MempoolService::IsHealthy() const {
    try {
        auto use=BorrowPoolUse();
        { std::lock_guard<std::mutex> lock(operation_mutex_); if (!started_ || stopping_) return false; }
        use->Pool().size();
        return true;
    } catch (...) { return false; }
}

std::string MempoolService::GetMetrics() const {
    std::unique_ptr<PoolUse> use;
    try { use=BorrowPoolUse(); } catch (const PoolUnavailable&) { return R"({"status":"not_initialized"})"; }
    bool started;
    { std::lock_guard<std::mutex> lock(operation_mutex_); started=started_; }
    std::ostringstream oss;
    oss << "{" << R"("service":"mempool",)" << R"("started":)" << (started ? "true" : "false") << ","
        << R"("tx_count":)" << use->Pool().size() << "," << R"("total_size":)" << use->Pool().getTotalSize() << ","
        << R"("total_fees":)" << use->Pool().getTotalFees() << "}";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// Fee Estimation (Phase 34)
// ═══════════════════════════════════════════════════════════════

void MempoolService::recordMempoolTransaction(const std::string& txid,
                                              uint64_t fee_rate,
                                              uint32_t current_height) {
    auto use=BorrowPoolUse();
    if (fee_estimator_) {
        fee_estimator_->addMempoolTransaction(txid, fee_rate, current_height);
        if (logger_interface_) {
            logger_interface_->debug("[MempoolService] Fee estimator: recorded tx " +
                txid.substr(0, 8) + "... at " + std::to_string(fee_rate) + " sat/kB");
        }
    }
}

void MempoolService::recordConfirmedTransaction(const std::string& txid,
                                                uint32_t confirm_height) {
    auto use=BorrowPoolUse();
    if (fee_estimator_) {
        uint64_t confirm_time = static_cast<uint64_t>(std::time(nullptr));
        fee_estimator_->addConfirmedTransaction(txid, confirm_height, confirm_time);
        if (logger_interface_) {
            logger_interface_->debug("[MempoolService] Fee estimator: confirmed tx " +
                txid.substr(0, 8) + "... at height " + std::to_string(confirm_height));
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase G.3: Transaction Relay Integration
// ═══════════════════════════════════════════════════════════════

void MempoolService::setTxRelayManager(std::shared_ptr<class TxRelayManager> tx_relay) {
    auto use=BorrowPoolUse();
    { std::lock_guard<std::mutex> lock(operation_mutex_); tx_relay_manager_.swap(tx_relay); }
}

// ═══════════════════════════════════════════════════════════════
// Transaction Broadcast via P2P
// ═══════════════════════════════════════════════════════════════

void MempoolService::broadcastTxViaP2P(const uint256& txid) {
    auto use=BorrowPoolUse();
    if (!p2p_service_) {
        if (logger_interface_) {
            logger_interface_->warning("[MempoolService] Cannot broadcast tx: P2P service not available");
        }
        return;
    }

    // Create inv message with raw binary txid (MSG_TX = 1)
    // This triggers the standard inv -> getdata -> tx flow
    ::P2PMessage inv_msg = ::P2PMessage::create_inv_binary(txid.data, 32, 1);

    p2p_service_->BroadcastMessage(inv_msg);

    if (logger_interface_) {
        logger_interface_->info("[TX-RELAY] Broadcasting INV for tx " + txid.GetHex());
    }
}

} // namespace dinero
