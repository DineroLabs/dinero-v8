#pragma once
#include <cstdint>
#include <optional>
#include <functional>
#include <filesystem>
#include "daemon_context.h"
#include "iservice.h"
#include "daemon/notify_drain_worker.h"
#include "daemon/stop_gate.h"
#include "daemon/core_reopen.h"
#include <vector>
#include <memory>
#include <mutex>

// Forward declarations
namespace dinero {
namespace daemon { class DatadirGuard; }
namespace pool {
class PoolManager;
}
namespace grpc_server {
#ifndef DISABLE_GRPC
class GrpcServer;
#endif
class SocketWalletServer;  // Phase 5: Socket-based wallet service (always enabled)
}
namespace ipc {
class WatchRegistrationServer;  // Phase 9.3: Watch registration server
}
class ChainDB;
class TxOrphanPool;
class RuntimeDeliveryWorker;
class RuntimeBlockNotifications;
struct RuntimeDeliveryStartupTestAccess;
}

namespace dinero {

/**
 * DaemonApp - Main application lifecycle manager
 *
 * Responsibilities:
 * - Construct all services in dependency order
 * - Initialize services (wire dependencies)
 * - Start services (open resources, spawn threads)
 * - Stop services in reverse order (graceful shutdown)
 *
 * Example usage:
 *   DaemonApp app;
 *   if (!app.Init()) return 1;
 *   if (!app.Start()) return 2;
 *   // ... run event loop ...
 *   app.Stop();
 */
class DaemonApp {
public:
    DaemonApp();
    ~DaemonApp();

    /**
     * Initialize all services
     * Creates service instances and calls Init() on each
     * @param argc Command line argument count (optional)
     * @param argv Command line arguments (optional)
     * @return true if all services initialized successfully
     */
    bool Init(int argc = 0, char** argv = nullptr);

    // Native startup pins the same resolved root as its held datadir lock.
    // Keep the guard alive until Stop(); config/CLI aliases must not redirect
    // services or the storage-upgrade cohort to another root after locking.
    void SetLockedDatadir(const daemon::DatadirGuard& guard);

    // Native daemons upgrade a legacy store to the separated shielded-state
    // layout before opening it (daemon/orchard_storage_upgrade.h). Off by
    // default for explicit migrator fixtures; dinerod and NodeCore opt in only
    // while retaining their real operator-root datadir guard.
    void SetAutomaticStorageUpgrade(bool enabled) { automatic_storage_upgrade_ = enabled; }
    // Bootstrap run for a brand-new datadir: Init returns at once if the store
    // already exists; otherwise it creates genesis and persists the records
    // the storage upgrade needs. The caller then Start()s/Stop()s this
    // instance and starts the real one, which upgrades the store at genesis.
    void SetFreshStoreBootstrapOnly(bool enabled) { fresh_store_bootstrap_only_ = enabled; }
    // True when this bootstrap run created the store (it must Start() and Stop()).
    bool FreshStoreCreated() const { return fresh_store_bootstrap_only_ && fresh_store_created_; }
    // After Start(): persist the genesis records; true when the store can now be upgraded.
    bool PersistFreshStoreGenesisRecords();

    // Tests only: called after each durable upgrade step; replaces measured free space.
    void SetStorageUpgradeTestHooks(std::function<void(const char*)> after_phase,
                                    std::optional<std::uintmax_t> available_bytes) {
        storage_upgrade_after_phase_ = std::move(after_phase);
        storage_upgrade_available_bytes_ = available_bytes;
    }

    /**
     * Start all services
     * Calls Start() on each service in order
     * @return true if all services started successfully
     */
    bool Start();

    /**
     * Stop all services
     * Calls Stop() on each service in REVERSE order
     */
    void Stop();

    // No owner conversion is authorized by these lifecycle methods. Failure
    // before commit resumes the SAME services; after commit RPC stays paused
    // until ResumeAfterInternalReopen succeeds on a fresh, started App.
    // An SDK host may pause under its publication mutex BEFORE moving the
    // old App out. Retain its shared pause until abort republishes that App;
    // do not release it in the unavailable publication gap on capture failure.
    std::unique_ptr<CoreReopenReceipt> CloseForInternalReopen(const daemon::DatadirGuard&,
        std::shared_ptr<ReopenRpcGate::Pause> pause={});
    CoreReopenReceipt::Readmission ResumeAfterInternalReopen(const daemon::DatadirGuard&,CoreReopenReceipt&);

    /**
     * Get daemon context (for testing/inspection)
     */
    DaemonContext& GetContext() { return ctx_; }

private:
    std::recursive_mutex close_reopen_mutex_;
    bool reopen_mempool_sealed_=false; // Set only by a genuine current capture.
    // Retained identity does not retain this App or any old database reader.
    const std::shared_ptr<const uint8_t> reopen_identity_=std::make_shared<const uint8_t>(0);
    std::optional<std::filesystem::path> locked_datadir_;
    bool automatic_storage_upgrade_ = false;
    bool fresh_store_bootstrap_only_ = false;
    bool fresh_store_created_ = false;
    std::function<void(const char*)> storage_upgrade_after_phase_;
    std::optional<std::uintmax_t> storage_upgrade_available_bytes_;
    friend struct RuntimeDeliveryStartupTestAccess;
    void ClosePoolRuntime() noexcept;
    DaemonContext ctx_;
    std::vector<std::shared_ptr<IService>> services_;
    bool started_ = false;
    // Stop() is reachable from multiple threads (node-thread shutdown path,
    // fatal escalation, destructor); the gate makes teardown run exactly once
    // while concurrent callers wait for it to finish (see stop_gate.h).
    StopGate stop_gate_;

    // The app and chainstate share the real database lifetime. Detached historical
    // preparation keeps a copy; init-only close/reopen still precedes workers.
    std::shared_ptr<ChainDB> chain_db_;

    // Per-service JSON loggers (Step B: Separate log files)
    std::unique_ptr<class JsonLogger> wallet_logger_;

    // #377: dedicated CSN ordered-validation worker (unpins the P2P dispatch
    // thread). Stopped early in Stop() so no drain runs during teardown.
    std::shared_ptr<daemon::NotifyDrainWorker> csn_drain_worker_;
    std::unique_ptr<class JsonLogger> p2p_logger_;
    std::unique_ptr<class JsonLogger> mining_logger_;
    std::unique_ptr<class JsonLogger> mempool_logger_;

    // Unified log aggregator (Optional: Real-time log streaming)
    std::unique_ptr<class LoggerRouter> logger_router_;

    // Phase 3: gRPC server (infrastructure, not a service)
    // Provides wallet operations to lightningd via gRPC
    // Dev mode only: Disabled in release builds (DINERO_RELEASE=ON)
    #ifndef DISABLE_GRPC
    std::unique_ptr<grpc_server::GrpcServer> grpc_server_;
    #endif

    // Phase 5: Socket wallet server (infrastructure, not a service)
    // Provides wallet operations to lightningd via sockets
    // Always enabled: Works in both dev and release builds
    std::unique_ptr<grpc_server::SocketWalletServer> socket_wallet_server_;

    // Phase 9.3: Watch registration server (bidirectional oracle communication)
    // Allows lightningd to register transaction watches with dinerod
    // Always enabled: Part of oracle infrastructure
    std::unique_ptr<class dinero::ipc::WatchRegistrationServer> watch_registration_server_;

    // Transaction orphan pool (lifetime owned by DaemonApp)
    std::unique_ptr<TxOrphanPool> orphan_pool_owned_;

    // Started after core recovery and joined before any dependency teardown.
    // This worker observes durable prefixes; it is not all-consumer readiness.
    std::unique_ptr<RuntimeDeliveryWorker> runtime_delivery_worker_;
    // Owns only coalesced wake mailboxes. Detached from chainstate before
    // worker shutdown; this ownership is not a delivery acknowledgment.
    std::shared_ptr<RuntimeBlockNotifications> runtime_notifications_;

    // Pool accounting runtime (optional)
    std::shared_ptr<pool::PoolManager> pool_manager_runtime_;
};

} // namespace dinero
