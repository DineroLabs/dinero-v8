#pragma once
#include "daemon/iservice.h"
#include "wallet/wallet_manager.h"
#include "wallet/hd_wallet.h"
#include <memory>
#include <string>
#include <vector>

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

    /**
     * Get reference to wrapped WalletManager
     * Use this to access wallet functionality
     */
    WalletManager& get() { return *wallet_mgr_; }
    const WalletManager& get() const { return *wallet_mgr_; }

    // Forward commonly used methods for convenience
    bool hasActiveWallet() const { return wallet_mgr_->hasActiveWallet(); }
    std::string getCurrentWalletName() const { return wallet_mgr_->getCurrentWalletName(); }
    std::vector<std::string> listWallets() const { return wallet_mgr_->listWallets(); }

    // Ensure runtime helpers that depend on an active wallet are wired after
    // wallet.createhd / wallet.open / wallet.restore, not only at daemon start.
    bool EnsureRuntimeWalletBindings();

private:
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
