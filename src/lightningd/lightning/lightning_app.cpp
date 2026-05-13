// ═══════════════════════════════════════════════════════════════════════════
// LightningApp Implementation - Phase 8.6: ChannelManagerCore Integration
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/lightning_app.h"
#include "lightning/chain_oracle.h"
#include "lightning/wallet_oracle.h"
#include "lightning/funding_service.h"
#include "lightning/htlc_sweep_oracle.h"
#include "lightning/justice_oracle.h"
#include "lightning/time_oracle.h"
#include "common/logger.h"

namespace dinero {
namespace lightning {

LightningApp::LightningApp(const LightningConfig& config)
    : m_config(config)
    , m_running(false)
{
    g_logger.info("[LightningApp] Initialized");
    g_logger.info("[LightningApp] DB path: " + m_config.db_path);
    g_logger.info("[LightningApp] Socket: " + m_config.socket_path);
}

LightningApp::~LightningApp() {
    if (m_running) {
        shutdown();
    }
}

bool LightningApp::start() {
    g_logger.info("[LightningApp] ═══════════════════════════════════════════════");
    g_logger.info("[LightningApp] Phase 8.6: Canonical Startup Sequence");
    g_logger.info("[LightningApp] ═══════════════════════════════════════════════");

    // =========================================================================
    // Step 1: Open Lightning DB
    // =========================================================================
    g_logger.info("[LightningApp] Step 1/4: Opening Lightning DB...");

    try {
        m_db = std::shared_ptr<SQLiteLightningDB>(new SQLiteLightningDB(m_config.db_path));

        if (!m_db->isOpen()) {
            g_logger.error("[LightningApp] Failed to open database");
            return false;
        }

        g_logger.info("[LightningApp] ✅ Database opened: " + m_config.db_path);
    } catch (const std::exception& e) {
        g_logger.error("[LightningApp] Database exception: " + std::string(e.what()));
        return false;
    }

    // =========================================================================
    // Step 2: Construct ChannelManagerCore
    // =========================================================================
    g_logger.info("[LightningApp] Step 2/4: Constructing ChannelManagerCore...");

    // Phase 8.6: Use mock oracles for now
    // Production oracles require DaemonContext, which lightningd cannot have
    // Future Phase 8.7: Implement gRPC-based oracles or IPC-based oracles

    auto chain_oracle = std::make_shared<::lightning::MockChainOracle>();
    auto wallet_oracle = std::make_shared<::lightning::MockWalletOracle>();
    auto funding_service = std::make_shared<::lightning::MockFundingService>();
    auto sweep_oracle = std::make_shared<::lightning::MockHTLCSweepOracle>();
    auto justice_oracle = std::make_shared<::lightning::MockJusticeOracle>();
    auto time_oracle = std::make_shared<::lightning::MockTimeOracle>();

    g_logger.info("[LightningApp] Using mock oracles (Phase 8.6 - production oracles in 8.7+)");

    // Placeholder node pubkey for testing (33-byte compressed pubkey in hex)
    std::string test_node_pubkey = "02" + std::string(64, '0');

    try {
        m_core = std::make_unique<ChannelManagerCore>(
            chain_oracle,
            wallet_oracle,
            funding_service,
            sweep_oracle,
            justice_oracle,
            m_db,
            test_node_pubkey,
            time_oracle.get()
        );

        g_logger.info("[LightningApp] ✅ ChannelManagerCore instantiated");
    } catch (const std::exception& e) {
        g_logger.error("[LightningApp] Core construction failed: " + std::string(e.what()));
        return false;
    }

    // =========================================================================
    // Step 3: Restore State from DB
    // =========================================================================
    g_logger.info("[LightningApp] Step 3/4: Restoring state from DB...");

    auto recovery_result = m_core->restoreFromDB();

    g_logger.info("[LightningApp] Recovery complete:");
    g_logger.info("[LightningApp]   Channels loaded: " + std::to_string(recovery_result.channels_loaded));
    g_logger.info("[LightningApp]   Channels corrupted: " + std::to_string(recovery_result.channels_corrupted));

    if (!recovery_result.errors.empty()) {
        g_logger.warning("[LightningApp] Recovery errors detected:");
        for (const auto& error : recovery_result.errors) {
            g_logger.warning("[LightningApp]   - " + error);
        }
    }

    if (recovery_result.channels_corrupted > 0) {
        g_logger.warning("[LightningApp] Some channels are corrupted (recovery_blocked)");
        g_logger.warning("[LightningApp] Continuing with valid channels only");
    }

    g_logger.info("[LightningApp] ✅ State restored from disk");

    // =========================================================================
    // Step 4: Enter Quiescent State
    // =========================================================================
    g_logger.info("[LightningApp] Step 4/4: Entering quiescent state...");

    // Phase 8.5 Critical Invariant:
    // After restoreFromDB() → NO actions occur until events arrive
    //
    // This means:
    // - NO sweeps are broadcast
    // - NO justice is executed
    // - NO HTLC transitions occur
    // - Lightning does NOTHING until events arrive
    //
    // This is INTENTIONAL and REQUIRED for determinism.

    m_running = true;

    g_logger.info("[LightningApp] ✅ Quiescent state reached");
    g_logger.info("[LightningApp] Waiting for events (block connections, tx confirmations)...");
    g_logger.info("[LightningApp] ═══════════════════════════════════════════════");

    return true;
}

void LightningApp::shutdown() {
    if (!m_running) {
        return;
    }

    g_logger.info("[LightningApp] Shutting down...");

    // Flush DB to ensure all state is persisted
    if (m_db) {
        m_db->flush();
        g_logger.info("[LightningApp] Database flushed");
    }

    m_running = false;
    g_logger.info("[LightningApp] Shutdown complete");
}

// ═════════════════════════════════════════════════════════════════════════════
// ILightningEventSink Implementation (Phase 8.6: Event Wiring)
// ═════════════════════════════════════════════════════════════════════════════

void LightningApp::onBlockConnected(const BlockConnectedEvent& event) {
    g_logger.info("[LightningApp] Event: BLOCK_CONNECTED height=" +
                  std::to_string(event.height) +
                  " hash=" + event.block_hash);

    // Phase 8.6: Wire event → core
    // One event → one atomic call to core
    auto result = m_core->onNewBlock(event.height, event.block_hash);

    if (result.isErr()) {
        g_logger.error("[LightningApp] Block processing failed: " + result.err());
    } else {
        g_logger.debug("[LightningApp] Block processed successfully");
    }
}

void LightningApp::onBlockDisconnected(const BlockDisconnectedEvent& event) {
    g_logger.info("[LightningApp] Event: BLOCK_DISCONNECTED height=" +
                  std::to_string(event.height) +
                  " hash=" + event.block_hash);

    // Phase 8.6: Reorg handling not yet implemented in core
    // For now, log only
    g_logger.warning("[LightningApp] Reorg handling not yet implemented (Phase 8.7+)");
}

void LightningApp::onTransactionConfirmed(const TransactionConfirmedEvent& event) {
    g_logger.info("[LightningApp] Event: TX_CONFIRMED txid=" + event.txid +
                  " height=" + std::to_string(event.height));

    // Phase 9: Wire event → core
    // This is called when watchtower detects a watched commitment on-chain
    m_core->onTransactionConfirmed(event.txid, event.height);

    g_logger.debug("[LightningApp] Transaction confirmation processed");
}

} // namespace lightning
} // namespace dinero
