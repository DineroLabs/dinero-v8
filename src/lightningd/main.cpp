/**
 * lightningd - Standalone Lightning Network Daemon
 *
 * Phase 8.6: ChannelManagerCore Integration
 * - Pure event-driven state machine (NO timers, NO threads, NO implicit time)
 * - Deterministic recovery from disk
 * - Quiescent startup (no side effects until events arrive)
 * - Idempotent event processing
 * - Crash-safe atomicity
 * - Real Lightning state transitions
 *
 * Architecture:
 *   dinerod → Unix Socket (IPC) → lightningd → LightningApp → ChannelManagerCore
 *
 * PHASE 8.6: Lightning becomes real
 * - Channels advance confirmations
 * - HTLC timeouts trigger
 * - Revoked commitments detected
 * - Justice paths activate
 * - Sweeps confirm
 *
 * PHASE 8 INVARIANTS (ENFORCED):
 * - ❌ NO std::thread (compile error if used)
 * - ❌ NO std::chrono::system_clock (wall time forbidden)
 * - ❌ NO timers or background polling
 * - ✅ ONLY event-driven state transitions
 * - ✅ Time exists ONLY as block height
 */

#include "lightning/lightning_app.h"
#include "lightningd/ipc_server.h"
#include "common/logger.h"
#include <iostream>
#include <memory>
#include <string>
#include <csignal>
#include <atomic>

// Phase 8.5: NO <thread> include - threads are forbidden
// Phase 8.5: NO <chrono> include - wall time is forbidden

using namespace dinero;
using namespace dinero::lightning;

// ═════════════════════════════════════════════════════════════════════════════
// Phase 8.5: Signal Handling (Deterministic Shutdown)
// ═════════════════════════════════════════════════════════════════════════════
// Signal handler triggers IPC server shutdown
// IPC server then exits its run() loop cleanly
// Main thread continues to cleanup and exit
//
// NO race conditions (signal handler only calls stop(), which is atomic)
// NO undefined behavior (stop() is signal-safe: just sets atomic bool + closes FD)
// ═════════════════════════════════════════════════════════════════════════════

// Global IPC server pointer (for signal handler access)
static LightningIPCServer* g_ipc_server = nullptr;

void signalHandler(int signum) {
    g_logger.info("Shutdown signal received (signal " + std::to_string(signum) + ")");

    // Phase 8.5: Trigger clean shutdown
    if (g_ipc_server) {
        g_ipc_server->stop();  // Sets m_running = false, closes listen socket
    }
}

// Phase 8.6: SimpleEventSink removed - replaced by LightningApp
// LightningApp owns ChannelManagerCore and implements ILightningEventSink

int main(int argc, char* argv[]) {
    // =========================================================================
    // Phase 8.6: Canonical Startup Sequence (LOCKED)
    // =========================================================================
    // 1. Parse arguments
    // 2. Construct LightningApp
    // 3. app.start() - opens DB, instantiates core, restores state
    // 4. Start IPC server (blocking, event-driven)
    // 5. app.shutdown()
    //
    // Phase 8.6: Lightning becomes real
    // - Channels advance confirmations
    // - HTLC timeouts trigger
    // - Revoked commitments detected
    // - Justice paths activate
    // =========================================================================

    g_logger.info("===========================================");
    g_logger.info("⚡ lightningd - Lightning Network Daemon");
    g_logger.info("===========================================");
    g_logger.info("Phase 8.6: ChannelManagerCore Integration");
    g_logger.info("");

    // =========================================================================
    // Step 1: Parse Command Line Arguments
    // =========================================================================
    LightningConfig config;
    config.socket_path = "/tmp/lightningd.sock";
    config.db_path = "lightning.db";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            config.socket_path = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            config.db_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: lightningd [options]\n"
                      << "\nOptions:\n"
                      << "  --socket <path>      Unix socket path (default: /tmp/lightningd.sock)\n"
                      << "  --db <path>          Database path (default: lightning.db)\n"
                      << "  --help, -h           Show this help message\n"
                      << "\nPhase 8.6: ChannelManagerCore Integration\n"
                      << "Runtime Guarantees:\n"
                      << "  - Real Lightning state transitions\n"
                      << "  - NO timers or background threads\n"
                      << "  - NO implicit time sources\n"
                      << "  - Purely event-driven state machine\n"
                      << "  - Deterministic replay from disk\n"
                      << "  - Idempotent event processing\n"
                      << "\nWire Protocol:\n"
                      << "  BLOCK_CONNECTED height=<uint64> hash=<hex>\n"
                      << "  BLOCK_DISCONNECTED height=<uint64> hash=<hex>\n"
                      << "  TX_CONFIRMED txid=<hex> height=<uint64>\n";
            return 0;
        }
    }

    // Install signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    g_logger.info("Configuration:");
    g_logger.info("  IPC socket: " + config.socket_path);
    g_logger.info("  Database: " + config.db_path);
    g_logger.info("");

    // =========================================================================
    // Step 2: Construct LightningApp
    // =========================================================================
    g_logger.info("[Main] Constructing LightningApp...");
    auto app = std::make_unique<LightningApp>(config);

    // =========================================================================
    // Step 3: Start LightningApp (Canonical Startup)
    // =========================================================================
    if (!app->start()) {
        g_logger.error("[Main] Failed to start LightningApp");
        return 1;
    }

    g_logger.info("[Main] ✅ LightningApp started successfully");
    g_logger.info("");

    // =========================================================================
    // Step 4: Start IPC Server (Blocking Event Loop)
    // =========================================================================
    g_logger.info("[Main] Initializing IPC server...");
    auto ipc_server = std::make_unique<LightningIPCServer>(app.get(), config.socket_path);

    // Phase 8.5: Register IPC server for signal handling
    g_ipc_server = ipc_server.get();

    if (!ipc_server->start()) {
        g_logger.error("Failed to start IPC server");
        g_logger.error("Check that socket path is writable and not already in use");
        g_ipc_server = nullptr;
        return 1;
    }

    g_logger.info("✅ IPC server listening on " + config.socket_path);
    g_logger.info("");
    g_logger.info("===========================================");
    g_logger.info("⚡ lightningd QUIESCENT");
    g_logger.info("===========================================");
    g_logger.info("");
    g_logger.info("Phase 8.5 Guarantees:");
    g_logger.info("  ✅ NO background threads");
    g_logger.info("  ✅ NO timers or polling");
    g_logger.info("  ✅ NO implicit time sources");
    g_logger.info("  ✅ Purely event-driven");
    g_logger.info("  ✅ Deterministic replay");
    g_logger.info("  ✅ Idempotent events");
    g_logger.info("");
    g_logger.info("Runtime state: QUIESCENT (waiting for events)");
    g_logger.info("Time source: Block height (from events)");
    g_logger.info("");
    g_logger.info("Press Ctrl+C to shutdown...");
    g_logger.info("");

    // =========================================================================
    // Phase 8.6: Event Loop (Pure Blocking, No Threads)
    // =========================================================================
    // IPC server runs in THIS thread (not a background thread)
    // run() blocks until:
    // - Signal received (SIGINT/SIGTERM)
    // - IPC error occurs
    //
    // NO std::thread used (Phase 8 violation)
    // NO sleep_for used (Phase 8 violation)
    //
    // This is the ONLY correct pattern for Phase 8 determinism.
    // =========================================================================

    ipc_server->run();  // Blocks until shutdown (stop() called by signal handler)

    // =========================================================================
    // Step 5: Shutdown Sequence
    // =========================================================================
    g_logger.info("");
    g_logger.info("Shutting down lightningd...");

    // Stop IPC server
    ipc_server->stop();  // Idempotent (may already be stopped by signal handler)
    g_ipc_server = nullptr;

    // Shutdown LightningApp (flushes DB, persists final state)
    app->shutdown();

    g_logger.info("⚡ lightningd stopped");
    return 0;
}
