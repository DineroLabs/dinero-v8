#pragma once
#include "daemon/iservice.h"
#include "wallet/wallet_manager.h"
#include "wallet/hd_wallet.h"
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <map>
#include <thread>

namespace dinero {

// Forward declarations
class ILogger;
class BlockStorage;

/**
 * WalletService - IService wrapper for WalletManager
 *
 * Wraps existing WalletManager into IService lifecycle:
 * - Init() wires dependencies from DaemonContext (logger, config, chainstate)
 * - Start() initializes wallet directory and loads active wallet if exists
 * - Stop() ensures wallet is flushed and closed cleanly
 *
 * Dependencies: Logger, Config, Chainstate
 *
 * The WalletManager handles:
 * - Multiple named wallets (create, open, rename, delete)
 * - HD address derivation (BIP84)
 * - UTXO tracking and balance calculation
 * - Transaction history
 * - Wallet encryption/locking
 * - Address labels and address book
 */
class WalletService : public IService {
public:
    WalletService();  // Defined in .cpp to allow unique_ptr<ZKWalletSync> with forward declaration
    ~WalletService() override;  // Defined in .cpp to allow unique_ptr<ZKWalletSync> with forward declaration

    std::string Name() const override { return "WalletManager"; }

    /**
     * Initialize wallet service with dependencies from context
     * Creates WalletManager instance with datadir from config
     */
    bool Init(DaemonContext& ctx) override;

    /**
     * Start wallet service
     * - Creates wallet directory if needed
     * - Loads blockchain height from chainstate
     * - Auto-opens the last active wallet if one exists, otherwise falls back to default
     */
    bool Start() override;

    /**
     * Stop wallet service
     * - Ensures current wallet is closed cleanly
     * - Flushes any pending database writes
     */
    void Stop() override;

    // A thread-affine lifetime owner. This pins the manager through Stop, but
    // does not pin a selected wallet, database transaction, seed or chain view.
    class WalletUse final {
    public:
        ~WalletUse() noexcept;
        WalletUse(const WalletUse&)=delete;
        WalletUse& operator=(const WalletUse&)=delete;
        WalletManager& Wallet() const;
    private:
        friend class WalletService;
        WalletUse(const WalletService&,std::shared_ptr<WalletService>);
        const WalletService& service_;
        std::shared_ptr<WalletService> retained_;
        const std::thread::id thread_=std::this_thread::get_id();
        WalletManager* wallet_=nullptr;
    };
    [[nodiscard]] static std::unique_ptr<WalletUse> AcquireWalletUse(
        std::shared_ptr<WalletService> service);

    // Legacy borrowed reference: callers still serialize shutdown themselves.
    WalletManager& get() { return *wallet_mgr_; }
    const WalletManager& get() const { return *wallet_mgr_; }

    bool hasActiveWallet() const { auto use=BorrowWalletUse(); return use->Wallet().hasActiveWallet(); }
    std::string getCurrentWalletName() const { auto use=BorrowWalletUse(); return use->Wallet().getCurrentWalletName(); }
    std::vector<std::string> listWallets() const { auto use=BorrowWalletUse(); return use->Wallet().listWallets(); }

    // Ensure runtime helpers that depend on an active wallet are wired after
    // wallet.createhd / wallet.open / wallet.restore, not only at daemon start.
    bool EnsureRuntimeWalletBindings();

    // Recover the active wallet from an already-loaded AssumeUTXO snapshot.
    // Locked encrypted wallets are deliberately deferred until wallet.unlock.
    bool RecoverActiveWalletFromSnapshotIfNeeded(std::string* error = nullptr);

private:
    std::unique_ptr<WalletUse> BorrowWalletUse() const;
    mutable std::mutex operation_mutex_;
    mutable std::condition_variable operation_changed_;
    mutable std::map<std::thread::id,size_t> operations_by_thread_;
    mutable size_t active_operations_=0;
    bool accepting_=false;
    bool stopping_=false;
    std::thread::id stopping_thread_;
    std::unique_ptr<WalletManager> wallet_mgr_;

    // Logger dependencies (dual pattern during migration):
    // - logger_: Legacy LoggerService (keep for compatibility during migration)
    // - logger_interface_: New ILogger dependency injection (actively used)
    std::shared_ptr<class LoggerService> logger_;
    ILogger* logger_interface_ = nullptr;

    std::shared_ptr<class ConfigService> config_;
    std::shared_ptr<class ChainstateService> chainstate_;
    BlockStorage* block_storage_ = nullptr;

    // HDWallet instance for wallet-derived helper operations
    std::unique_ptr<HDWallet> hd_wallet_;
};

} // namespace dinero
