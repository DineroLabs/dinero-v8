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

bool MempoolService::Init(DaemonContext& ctx) {
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
    return true;
}

bool MempoolService::Start() {
    if (started_) {
        logger_interface_->warning("[MempoolService] Already started");
        return false;
    }

    logger_interface_->info("[MempoolService] Starting mempool...");

    // Mempool doesn't require explicit initialization beyond construction
    // but we could load persisted transactions here if needed

    size_t initial_size = mempool_->size();
    logger_interface_->info("[MempoolService] Mempool started successfully");
    logger_interface_->info("[MempoolService]   Initial transaction count: " +
                 std::to_string(initial_size));

    started_ = true;
    return true;
}

void MempoolService::Stop() {
    if (!started_) {
        return;
    }

    logger_interface_->info("[MempoolService] Shutting down mempool...");

    // Get final stats
    size_t final_size = mempool_->size();
    uint64_t total_fees = mempool_->getTotalFees();

    logger_interface_->info("[MempoolService] Final transaction count: " +
                 std::to_string(final_size));
    logger_interface_->info("[MempoolService] Total fees in mempool: " +
                 std::to_string(total_fees) + " una");

    // Optionally: Persist mempool transactions here
    // For now, we just clear it
    mempool_->clear();

    // Reset instance
    mempool_.reset();

    logger_interface_->info("[MempoolService] Mempool shutdown complete");
    started_ = false;
}

bool MempoolService::IsHealthy() const {
    if (!started_ || !mempool_) {
        return false;
    }

    // Basic health check: can we query mempool size?
    try {
        mempool_->size();
        return true;
    } catch (...) {
        return false;
    }
}

std::string MempoolService::GetMetrics() const {
    if (!mempool_) {
        return R"({"status":"not_initialized"})";
    }

    std::ostringstream oss;
    oss << "{"
        << R"("service":"mempool",)"
        << R"("started":)" << (started_ ? "true" : "false") << ","
        << R"("tx_count":)" << mempool_->size() << ","
        << R"("total_size":)" << mempool_->getTotalSize() << ","
        << R"("total_fees":)" << mempool_->getTotalFees()
        << "}";

    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// Fee Estimation (Phase 34)
// ═══════════════════════════════════════════════════════════════

void MempoolService::recordMempoolTransaction(const std::string& txid,
                                              uint64_t fee_rate,
                                              uint32_t current_height) {
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
    tx_relay_manager_ = tx_relay;
}

// ═══════════════════════════════════════════════════════════════
// Transaction Broadcast via P2P
// ═══════════════════════════════════════════════════════════════

void MempoolService::broadcastTxViaP2P(const uint256& txid) {
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
