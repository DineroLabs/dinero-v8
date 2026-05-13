#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// LightningApp - Phase 8.6: ChannelManagerCore Integration
// ═══════════════════════════════════════════════════════════════════════════
// lightningd runtime binding. Owns the Lightning state machine.
//
// CANONICAL OWNERSHIP MODEL:
// lightningd
//  └── LightningApp
//      ├── ILightningDB          (owned)
//      ├── ChannelManagerCore    (owned)
//      └── IPCServer             (owned, via main)
//
// ARCHITECTURAL CONTRACT:
// - NO globals
// - NO shared pointers crossing boundaries
// - Exactly one authoritative runtime instance
// - Quiescent startup (NO actions until events)
//
// Phase 8.6: This is the moment Lightning becomes real.
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/lightning_event_sink.h"
#include "lightning/channel_manager_core.h"
#include "lightning/sqlite_lightning_db.h"
#include <memory>
#include <string>

namespace dinero {
namespace lightning {

/**
 * @struct LightningConfig
 * @brief Configuration for LightningApp
 */
struct LightningConfig {
    std::string db_path = "lightning.db";
    std::string socket_path = "/tmp/lightningd.sock";
};

/**
 * @class LightningApp
 * @brief Lightning daemon application - owns the state machine
 *
 * Phase 8.6 Responsibilities:
 * - Owns ChannelManagerCore (the state machine)
 * - Owns ILightningDB (persistence)
 * - Implements ILightningEventSink (receives events from IPC)
 * - Enforces quiescent startup (restore then wait)
 * - Maintains Phase 8.5 determinism (no threads, no clocks)
 *
 * Lifecycle:
 * 1. Construct with config
 * 2. start() - opens DB, instantiates core, restores state
 * 3. run() - event loop (blocking, waits for events)
 * 4. shutdown() - clean stop
 */
class LightningApp : public ILightningEventSink {
public:
    /**
     * @brief Construct Lightning application
     * @param config Configuration (DB path, socket path, etc.)
     */
    explicit LightningApp(const LightningConfig& config);
    ~LightningApp();

    /**
     * @brief Start Lightning daemon (canonical startup sequence)
     *
     * Phase 8.6 Startup Sequence (LOCKED):
     * 1. Open Lightning DB
     * 2. Construct ChannelManagerCore
     * 3. Restore state from DB (restoreFromDB)
     * 4. Enter quiescent state
     *
     * Critical Invariant:
     * After restoreFromDB() → NO actions occur until events arrive
     *
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Shutdown Lightning daemon
     * Flushes DB, persists final state
     */
    void shutdown();

    /**
     * @brief Check if Lightning is running
     * @return true if started and not shut down
     */
    bool isRunning() const { return m_running; }

    // ═══════════════════════════════════════════════════════════════════════
    // ILightningEventSink Implementation (Phase 8.6: Event Wiring)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Process block connection event
     *
     * Phase 8.6 Semantics:
     * - One event → one atomic call to core
     * - NO batching
     * - NO implicit follow-up calls
     * - Deterministic ordering only
     *
     * Wiring:
     * BlockConnectedEvent → m_core->onNewBlock(height, hash)
     */
    void onBlockConnected(const BlockConnectedEvent& event) override;

    /**
     * @brief Process block disconnection event (reorg)
     *
     * Phase 8.6: Currently logged, not yet handled by core
     * Future: onBlockDisconnected() support in core
     */
    void onBlockDisconnected(const BlockDisconnectedEvent& event) override;

    /**
     * @brief Process transaction confirmation event
     *
     * Phase 9 Integration:
     * TransactionConfirmedEvent → m_core->onTransactionConfirmed(txid, height)
     *
     * This is called when watchtower detects a watched commitment on-chain.
     */
    void onTransactionConfirmed(const TransactionConfirmedEvent& event) override;

    // ═══════════════════════════════════════════════════════════════════════
    // Public Accessors (Read-Only)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Get channel manager core (read-only access)
     * For inspection/debugging only - do NOT mutate state
     */
    const ChannelManagerCore* getCore() const { return m_core.get(); }

    /**
     * @brief Get database (read-only access)
     * For inspection/debugging only - do NOT mutate state
     */
    const ILightningDB* getDB() const { return m_db.get(); }

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Owned Components (Phase 8.6 Canonical Ownership)
    // ═══════════════════════════════════════════════════════════════════════

    LightningConfig m_config;
    bool m_running;

    // Phase 8.6: Owned components (lifecycle managed by LightningApp)
    std::shared_ptr<ILightningDB> m_db;              // Persistence (shared with core)
    std::unique_ptr<ChannelManagerCore> m_core;      // State machine

    // TODO Phase 8.6+: Oracle implementations (currently mocks)
    // std::shared_ptr<IChainOracle> m_chain_oracle;
    // std::shared_ptr<IWalletOracle> m_wallet_oracle;
    // std::shared_ptr<IFundingService> m_funding_service;
    // std::shared_ptr<IHTLCSweepOracle> m_sweep_oracle;
    // std::shared_ptr<ITimeOracle> m_time_oracle;
};

} // namespace lightning
} // namespace dinero
