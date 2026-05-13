#pragma once

#include "lightningd/lightning_context.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lightningd {

/**
 * LightningDaemonApp - Lifecycle manager for standalone Lightning daemon
 *
 * Manages initialization, startup, and shutdown of all Lightning components.
 * Similar to dinero::DaemonApp but focused solely on Lightning Network.
 *
 * Lifecycle:
 * 1. Init(argc, argv) - Parse config, create components
 * 2. Start() - Start gRPC clients, connect to dinerod, start Lightning
 * 3. WaitForShutdown() - Block until SIGINT/SIGTERM
 * 4. Stop() - Graceful shutdown in reverse order
 *
 * Architecture:
 * - No blockchain consensus (delegated to dinerod)
 * - No mempool (delegated to dinerod via gRPC)
 * - No mining
 * - Focused on: channels, HTLCs, routing, watchtower
 *
 * Communication:
 * - dinerod ← gRPC → lightningd (blockchain/mempool queries)
 * - peers ← Lightning P2P (port 9735) → lightningd (BOLT protocol)
 */
class LightningDaemonApp {
public:
    LightningDaemonApp();
    ~LightningDaemonApp();

    /**
     * Initialize Lightning daemon
     *
     * Parses command-line arguments, loads configuration,
     * creates gRPC clients, and initializes Lightning components.
     *
     * @param argc  Argument count
     * @param argv  Argument vector
     * @return true if initialization succeeded, false otherwise
     */
    bool Init(int argc, char** argv);

    /**
     * Start Lightning daemon
     *
     * Connects to dinerod via gRPC, starts event subscriptions,
     * and begins Lightning P2P listening.
     *
     * @return true if startup succeeded, false otherwise
     */
    bool Start();

    /**
     * Stop Lightning daemon
     *
     * Gracefully shuts down all components in reverse order.
     * Closes channels if needed, stops peer connections,
     * disconnects from dinerod.
     */
    void Stop();

    /**
     * Wait for shutdown signal
     *
     * Blocks the main thread until SIGINT or SIGTERM is received.
     * Call this after Start() to keep the daemon running.
     */
    void WaitForShutdown();

    /**
     * Signal shutdown
     *
     * Called by signal handler to initiate graceful shutdown.
     * Wakes up WaitForShutdown().
     */
    void SignalShutdown();

    /**
     * Get Lightning context
     *
     * @return Reference to the Lightning context
     */
    LightningContext& GetContext() { return ctx_; }

    /**
     * Check if daemon is running
     *
     * @return true if daemon is running, false otherwise
     */
    bool IsRunning() const { return running_; }

private:
    /**
     * Parse command-line arguments
     *
     * Extracts configuration options from argc/argv.
     *
     * @param argc  Argument count
     * @param argv  Argument vector
     * @return true if parsing succeeded, false otherwise
     */
    bool ParseArguments(int argc, char** argv);

    /**
     * Initialize gRPC clients
     *
     * Creates BlockchainClient, MempoolClient, and EventSubscriber
     * and connects them to dinerod.
     *
     * @return true if clients connected successfully, false otherwise
     */
    bool InitializeGrpcClients();

    /**
     * Initialize Lightning components
     *
     * Creates ChannelManager, HTLCManager, PaymentRouter, etc.
     *
     * @return true if components initialized successfully, false otherwise
     */
    bool InitializeLightningComponents();

    /**
     * Start gRPC event subscriptions
     *
     * Subscribes to block events and transaction notifications
     * from dinerod.
     *
     * @return true if subscription succeeded, false otherwise
     */
    bool StartEventSubscriptions();

    /**
     * Start Lightning P2P server
     *
     * Begins listening on port 9735 for Lightning peer connections.
     *
     * @return true if server started successfully, false otherwise
     */
    bool StartLightningP2P();

    // Lightning context (dependency injection container)
    LightningContext ctx_;

    // Daemon state
    std::atomic<bool> running_;
    std::atomic<bool> shutdown_requested_;

    // Shutdown synchronization
    std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;

    // Configuration values (parsed from args)
    std::string config_file_;
    std::string data_dir_;
    std::string dinerod_grpc_address_;
    int lightning_port_;
    bool testnet_;
    bool regtest_;
};

} // namespace lightningd
