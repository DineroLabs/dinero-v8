#include "daemon/services/wallet_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/daemon_context.h"
#include "daemon/rpc/wallet_gui_handlers.h"  // For RpcCreateHDWallet
#include "common/ilogger.h"           // For ILogger interface dependency injection
#include "vault/vault_runtime.h"
#include "wallet/wallet_worker.h"
#include "wallet/hd_wallet.h"         // Phase F.5: For BIP32 view key derivation
#include "consensus/coin_type.h"      // DINERO_COIN_TYPE
#include "primitives/block.h"          // For dinero::Block
#include <filesystem>
#include <stdexcept>

namespace dinero {

// Constructor and destructor must be defined in .cpp to allow unique_ptr<ZKWalletSync> with forward declaration
WalletService::WalletService() = default;
WalletService::~WalletService() = default;

bool WalletService::Init(DaemonContext& ctx) {
    // Wire dependencies from context
    logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    // Use dedicated wallet logger if available, fallback to shared logger
    logger_interface_ = ctx.wallet_logger ? ctx.wallet_logger : ctx.logger_interface;
    config_ = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    chainstate_ = std::dynamic_pointer_cast<ChainstateService>(ctx.chainstate);
    block_storage_ = ctx.block_storage.get();

    if (!logger_interface_) {
        throw std::runtime_error("[WalletService] Logger interface dependency missing");
    }
    if (!config_) {
        logger_interface_->error("[WalletService] Config dependency missing");
        return false;
    }

    // Note: Chainstate is optional during Init - may not be ready yet
    // WalletWorker will be initialized in Start() when chainstate is available

    // Get data directory from config
    std::string datadir = config_->DataDir();
    std::string wallet_schema_path = config_->GetString("wallet-schema-path", "");

    logger_interface_->info("[WalletService] Initializing with data directory: " + datadir);
    if (!wallet_schema_path.empty()) {
        logger_interface_->info("[WalletService] Using wallet schema override path: " + wallet_schema_path);
    }

    try {
        // Create WalletManager with data directory (it handles /wallets subdirectory internally)
        #ifdef FFI_WALLET_ONLY
        wallet_mgr_ = std::make_unique<WalletManager>(datadir, logger_interface_, wallet_schema_path);
        #else
        wallet_mgr_ = std::make_unique<WalletManager>(std::filesystem::path(datadir), logger_interface_, wallet_schema_path);
        #endif

        // Week 5: Bridge pattern removed - all code now uses ctx.daemon->wallet->get()
        logger_interface_->info("[WalletService] WalletManager created successfully");
        return true;

    } catch (const std::exception& e) {
        logger_interface_->error("[WalletService] Failed to create WalletManager: " + std::string(e.what()));
        return false;
    }
}

bool WalletService::Start() {
    if (!wallet_mgr_) {
        logger_interface_->error("[WalletService] Cannot start - wallet manager not initialized");
        return false;
    }

    logger_interface_->info("[WalletService] Starting wallet service...");

    try {
        // Run health check on wallet database
        logger_interface_->info("[WalletService] Running wallet database health check...");
        wallet_mgr_->runHealthCheck();

        // Load blockchain height for UTXO maturity calculations
        // Get real blockchain height from chainstate, not wallet's scan progress
        // Note: Rescan will happen AFTER wallet is opened (see below)
        uint32_t actual_blockchain_height = 0;
        uint32_t wallet_scan_height = 0;
        bool needs_catchup_scan = false;
        
        try {
            if (chainstate_) {
                actual_blockchain_height = chainstate_->getBlockHeight();
                logger_interface_->info("[WalletService] Real blockchain height from chainstate: " + std::to_string(actual_blockchain_height));
                
                // Also load wallet's scan progress from its tip table
                wallet_mgr_->loadBlockchainHeight();
                wallet_scan_height = wallet_mgr_->getCurrentBlockchainHeight();
                logger_interface_->info("[WalletService] Wallet scan progress: " + std::to_string(wallet_scan_height));
                
                if (wallet_scan_height < actual_blockchain_height) {
                    logger_interface_->warning("[WalletService] Wallet is behind blockchain ("
                        + std::to_string(wallet_scan_height) + " < " + std::to_string(actual_blockchain_height)
                        + ") - will trigger catch-up scan after wallet is opened");
                    needs_catchup_scan = true;
                } else if (wallet_scan_height > actual_blockchain_height) {
                    // Wallet is AHEAD of blockchain — chain data was wiped or reorged.
                    // Reset wallet to blockchain height and rescan from genesis.
                    logger_interface_->warning("[WalletService] Wallet is AHEAD of blockchain ("
                        + std::to_string(wallet_scan_height) + " > " + std::to_string(actual_blockchain_height)
                        + ") - chain was likely wiped/reorged. Rewinding wallet.");
                    wallet_mgr_->setBlockchainHeight(actual_blockchain_height);
                    wallet_scan_height = 0;  // rescan from genesis
                    needs_catchup_scan = true;
                }
            } else {
                wallet_mgr_->loadBlockchainHeight();
                wallet_scan_height = wallet_mgr_->getCurrentBlockchainHeight();
                logger_interface_->warning("[WalletService] Chainstate not available - using wallet height: " + std::to_string(wallet_scan_height));
            }
        } catch (const std::exception& e) {
            logger_interface_->warning("[WalletService] Could not load blockchain height: " + std::string(e.what()));
            logger_interface_->warning("[WalletService] UTXO maturity calculations may be inaccurate until chainstate syncs");
        }

        // Initialize wallet worker thread for block notifications
        // Get UTXO index from chainstate service if available
        UTXOIndex* utxo_index = nullptr;
        if (chainstate_) {
            utxo_index = chainstate_->utxoIndex();
            logger_interface_->info("[WalletService] Initializing wallet worker with UTXO index");
        } else {
            logger_interface_->warning("[WalletService] Chainstate not available - wallet worker will start without UTXO index");
        }

        // ✅ CRITICAL FIX: Pass WalletManager to enable UTXO database persistence
        WalletNotify::Initialize(utxo_index, wallet_mgr_.get());
        logger_interface_->info("[WalletService] Wallet worker thread started with database persistence");

        // ✅ CRITICAL INVARIANT: UTXOIndex MUST be set before any wallet operations
        // This enables address registration for wallet UTXO tracking
        // Without UTXOIndex, wallet addresses won't be registered for block scanning
        if (utxo_index) {
            wallet_mgr_->setUTXOIndex(utxo_index);
            logger_interface_->info("[WalletService] ✅ UTXOIndex set in WalletManager");
        } else {
            logger_interface_->error("[WalletService] ❌ CRITICAL: UTXOIndex not available!");
            logger_interface_->error("[WalletService] Wallet UTXO tracking will NOT work.");
            logger_interface_->error("[WalletService] Ensure Chainstate is initialized before WalletService.");
        }

        // ⚡ Inject LightningService pointer into WalletManager
        // This enables per-wallet Lightning initialization
        if (chainstate_) {
            auto daemon_ctx = DaemonContext::instance();
            if (daemon_ctx && daemon_ctx->lightning) {
                wallet_mgr_->setLightningService(daemon_ctx->lightning.get());
                logger_interface_->info("[WalletService] ⚡ LightningService injected into WalletManager");
            } else {
                logger_interface_->warning("[WalletService] Lightning service not available in daemon context");
            }
        }

        // Check for existing wallets
        auto wallets = wallet_mgr_->listWallets();
        if (wallets.empty()) {
            logger_interface_->info("[WalletService] No wallets found - creating default HD wallet");
            try {
                // User-facing wallets must begin from BIP39 recovery material.
                // RpcCreateHDWallet creates the DB, replaces its internal
                // bootstrap seed before any address can be exposed, binds the
                // mnemonic entropy to that active seed, and returns the phrase
                // for client presentation. Calling create() first used to make
                // the RPC fail with "Wallet already exists", leaving a raw-seed
                // wallet with no recoverable mnemonic.
                din::Json createhd_params;
                createhd_params[0] = "default";  // wallet name
                createhd_params[1] = 12;         // word count
                createhd_params[2] = "";         // bip39 passphrase (empty)
                createhd_params[3] = "";         // encryption password (empty for regtest/auto-creation)

                auto createhd_result = dinero::rpc::RpcCreateHDWallet(createhd_params, wallet_mgr_.get());
                if (createhd_result.isMember("error")) {
                    throw std::runtime_error("Failed to generate HD seed: " + createhd_result["error"].asString());
                }
                if (!createhd_result.isMember("mnemonic") ||
                    createhd_result["mnemonic"].asString().empty() ||
                    !wallet_mgr_->hasAuthoritativeBip39Mnemonic()) {
                    throw std::runtime_error(
                        "HD wallet creation did not persist authoritative BIP39 recovery material");
                }

                // Do not log the phrase. It remains retrievable from the active
                // wallet via wallet.exportseed until the user records it.
                createhd_result["mnemonic"] = "";
                logger_interface_->info("[WalletService] ✅ Auto-created and opened 'default' BIP39 HD wallet");
                logger_interface_->warning(
                    "[WalletService] RECOVERY BACKUP REQUIRED: retrieve the authoritative phrase with "
                    "wallet.exportseed, store it offline, then call wallet.acknowledgeseedbackup");

                // Load any existing addresses into UTXOIndex (now populated with first address from HD wallet)
                wallet_mgr_->LoadAddressesIntoUTXOIndex();

            } catch (const std::exception& e) {
                logger_interface_->error("[WalletService] Failed to auto-create default wallet: " + std::string(e.what()));
                logger_interface_->info("[WalletService] User can manually create wallet via RPC");
            }
        } else {
            logger_interface_->info("[WalletService] Found " + std::to_string(wallets.size()) + " wallet(s):");
            for (const auto& name : wallets) {
                logger_interface_->info("[WalletService]   - " + name);
            }

            std::string wallet_to_open = wallet_mgr_->getMostRecentlyOpenedWallet();
            if (wallet_to_open.empty() &&
                std::find(wallets.begin(), wallets.end(), "default") != wallets.end()) {
                wallet_to_open = "default";
            }

            if (!wallet_to_open.empty()) {
                try {
                    wallet_mgr_->open(wallet_to_open);
                    logger_interface_->info("[WalletService] Auto-opened wallet: " + wallet_to_open);

                } catch (const std::exception& e) {
                    logger_interface_->warning("[WalletService] Could not auto-open wallet '" +
                                               wallet_to_open + "': " + std::string(e.what()));
                }
            }
        }

        if (wallet_mgr_->hasActiveWallet()) {
            EnsureRuntimeWalletBindings();

            if (!wallet_mgr_->hasAuthoritativeBip39Mnemonic()) {
                logger_interface_->warning(
                    "[WalletService] ACTIVE WALLET HAS NO AUTHORITATIVE MNEMONIC: this wallet predates "
                    "mnemonic-backed creation or uses a raw seed. Preserve a verified encrypted wallet "
                    "backup and migrate funds to a mnemonic-backed wallet before relying on seed recovery.");
            } else if (wallet_mgr_->getSetting("bip39_backup_acknowledged") != "1") {
                logger_interface_->warning(
                    "[WalletService] RECOVERY BACKUP NOT ACKNOWLEDGED: retrieve wallet.exportseed, "
                    "store it offline, then call wallet.acknowledgeseedbackup");
            }
        }

        // Snapshot UTXO-set rescan for a wallet that becomes active AFTER an
        // AssumeUTXO snapshot was already loaded (e.g. fast-sync the node, THEN
        // open/import/restore the wallet). In that order the LoadSnapshot hook
        // ran wallet-absent, so the pre-base coins were never recorded — and the
        // block-replay catch-up below cannot recover them because the node is
        // headers-only with no block bodies under the snapshot base height.
        //
        // Gate: while an AssumeUTXO snapshot is active OR after its history has
        // been promoted. Promotion proves history but does not materialize the
        // missing pre-base block bodies, so late wallet recovery still requires
        // the snapshot UTXO section. Never run on a normal full node. The rescan
        // upsert is idempotent, so a redundant run is harmless.
        //
        // The rescan operates on the ACTIVE wallet only, so on a multi-wallet node
        // sweep EVERY wallet: open each, rescan from the snapshot, then restore the
        // originally-active wallet. This surfaces every wallet's pre-snapshot coins
        // at once instead of only the auto-opened one.
        const uint32_t snapshot_recovery_base = chainstate_
            ? chainstate_->GetSnapshotWalletRecoveryBaseHeight() : 0u;
        if (chainstate_ && snapshot_recovery_base > 0 && !wallets.empty()) {
            const uint32_t base_height = snapshot_recovery_base;
            // Capture the wallet to restore as active afterward (open() below
            // overwrites the "most recently opened" marker).
            const std::string preferred_wallet = wallet_mgr_->hasActiveWallet()
                ? wallet_mgr_->getMostRecentlyOpenedWallet() : std::string();
            for (const auto& wname : wallets) {
                try {
                    wallet_mgr_->open(wname);
                    int recorded = chainstate_->RescanWalletFromSnapshotUTXOs(*wallet_mgr_, base_height);
                    if (recorded < 0) {
                        throw std::runtime_error("snapshot UTXO source unavailable");
                    }
                    if (recorded > 0) {
                        logger_interface_->info("[WalletService] Snapshot UTXO-set rescan: wallet '"
                            + wname + "' recorded " + std::to_string(recorded)
                            + " owned coin(s) from base height " + std::to_string(base_height));
                    }

                    // The wallet may already claim to be at the current tip even
                    // though the snapshot coins were absent when it originally
                    // scanned the post-base blocks. Replaying only when the old
                    // watermark is behind would resurrect snapshot coins spent
                    // after the base. Always replay base+1..tip immediately for
                    // EACH wallet after inserting its base UTXOs.
                    if (base_height < actual_blockchain_height) {
                        auto* chain_db = chainstate_->GetChainDB();
                        if (!chain_db || !wallet_mgr_->rescanBlockchain(
                                static_cast<int>(base_height + 1),
                                /*gap_limit=*/20,
                                chain_db,
                                block_storage_)) {
                            throw std::runtime_error(
                                "post-snapshot wallet replay failed");
                        }
                    }
                } catch (const std::exception& e) {
                    logger_interface_->warning("[WalletService] Snapshot rescan failed for wallet '"
                        + wname + "': " + std::string(e.what()));
                }
            }
            // Restore the originally-active wallet and re-establish its bindings.
            if (!preferred_wallet.empty()) {
                try {
                    wallet_mgr_->open(preferred_wallet);
                    EnsureRuntimeWalletBindings();
                } catch (const std::exception&) {}
            }
            // Every wallet was synchronously replayed through the current tip
            // above. Do not enqueue a second asynchronous pass against whichever
            // wallet happened to be restored as active.
            wallet_scan_height = actual_blockchain_height;
            needs_catchup_scan = false;
        }

        // Trigger catch-up scan if wallet is behind blockchain
        // This happens AFTER wallet is opened so rescan has an active wallet
        // We manually trigger WalletNotify for each missed block
        if (needs_catchup_scan && wallet_mgr_->hasActiveWallet() && chainstate_) {
            logger_interface_->info("[WalletService] Triggering catch-up scan from height " 
                + std::to_string(wallet_scan_height + 1) + " to " + std::to_string(actual_blockchain_height) + "...");
            
            try {
                // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
                auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(chainstate_);
                auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
                if (!chain_db || !chainstate) {
                    logger_interface_->error("[WalletService] ChainDB not available for catch-up scan");
                } else {

                    // Manually trigger WalletNotify for each block the wallet missed
                    for (uint32_t height = wallet_scan_height + 1; height <= actual_blockchain_height; height++) {
                        if ((height - wallet_scan_height) % 1000 == 0) {
                            logger_interface_->info("[WalletService] Catch-up progress: " +
                                std::to_string(height) + "/" + std::to_string(actual_blockchain_height));
                        }
                        try {
                            // Phase M.0: Get block hash as uint256
                            auto hash_result = chain_db->getBlockHashByHeight(height);
                            if (hash_result.ok() && !hash_result.value().IsNull()) {
                                const uint256& block_hash = hash_result.value();

                                // Get full block data
                                auto block_result = chainstate->getBlockByHash(block_hash);
                                if (block_result.ok()) {
                                    // Trigger wallet worker to scan this block
                                    // Phase M.0: Convert uint256 to string at wallet boundary
                                    WalletNotify::OnBlockConnected(height, block_hash.GetHex(), block_result.value().vtx);
                                    logger_interface_->info("[WalletService]   Scanned block " + std::to_string(height)
                                        + " (" + std::to_string(block_result.value().vtx.size()) + " txs)");
                                } else {
                                    logger_interface_->warning("[WalletService]   Could not load block " + std::to_string(height) + " for scanning");
                                }
                            } else {
                                logger_interface_->warning("[WalletService]   Could not find hash for block " + std::to_string(height));
                            }
                        } catch (const std::exception& block_err) {
                            logger_interface_->warning("[WalletService]   Failed to scan block " + std::to_string(height) + ": " + block_err.what());
                        }
                    }

                    logger_interface_->info("[WalletService] ✅ Wallet catch-up scan completed");

                    // Priority 3 FIX: Validate wallet UTXOs against consensus
                    // This removes any phantom UTXOs that may have been created by incomplete reorgs
                    if (utxo_index && chain_db) {
                        try {
                            size_t phantoms = utxo_index->ValidateAgainstConsensus(
                                [chain_db](const TxId& txid, uint32_t vout) -> bool {
                                    auto result = chain_db->getCoin(txid.AsUint256(), vout);
                                    return result.ok();
                                });
                            if (phantoms > 0) {
                                logger_interface_->warning("[WalletService] Priority 3 FIX: Removed "
                                    + std::to_string(phantoms) + " phantom UTXOs");
                            } else {
                                logger_interface_->info("[WalletService] ✅ Wallet UTXO validation passed (no phantoms)");
                            }
                        } catch (const std::exception& val_err) {
                            logger_interface_->warning("[WalletService] UTXO validation failed: "
                                + std::string(val_err.what()));
                        }
                    }
                }
            } catch (const std::exception& rescan_err) {
                logger_interface_->error("[WalletService] Wallet catch-up scan failed: " + std::string(rescan_err.what()));
            }
        }

        logger_interface_->info("[WalletService] Wallet service started successfully");

        return true;

    } catch (const std::exception& e) {
        logger_interface_->error("[WalletService] Failed to start: " + std::string(e.what()));
        return false;
    }
}

bool WalletService::EnsureRuntimeWalletBindings() {
    if (!wallet_mgr_ || !wallet_mgr_->hasActiveWallet()) {
        return false;
    }

    wallet_mgr_->LoadAddressesIntoUTXOIndex();

    try {
        if (!wallet_mgr_->getHDWallet()) {
            auto cfg = std::dynamic_pointer_cast<ConfigService>(config_);
            std::string hd_dir = (cfg ? cfg->getDataDir() : ".") + "/hd_wallet";
            hd_wallet_ = HDWallet::Open(hd_dir, dinero::consensus::DINERO_COIN_TYPE, false);
            if (hd_wallet_) {
                wallet_mgr_->setHDWallet(hd_wallet_.get());
                logger_interface_->info("[WalletService] ✅ HDWallet wired into WalletManager");
            }
        }
    } catch (const std::exception& e) {
        logger_interface_->warning("[WalletService] Runtime wallet binding incomplete: " + std::string(e.what()));
    }

    try {
        if (dinero::vault::GetVaultRuntimeService() != nullptr) {
            const auto bound = dinero::vault::GetVaultOperator();
            if (bound.address.empty()) {
                std::string primary = wallet_mgr_->getPrimaryAddress();
                if (primary.empty()) {
                    primary = wallet_mgr_->getNewAddress("", "taproot");
                    if (!primary.empty()) {
                        logger_interface_->info(
                            "[WalletService] Derived first wallet address for Liquidity Vault auto-bind: " +
                            primary);
                    }
                }
                if (!primary.empty()) {
                    std::string err;
                    if (dinero::vault::SetVaultOperator(primary, "wallet", &err)) {
                        logger_interface_->info(
                            "[WalletService] Auto-bound Liquidity Vault to wallet primary address: " +
                            primary);
                    } else if (!err.empty()) {
                        logger_interface_->warning(
                            "[WalletService] Liquidity Vault auto-bind failed: " + err);
                    }
                } else {
                    logger_interface_->warning(
                        "[WalletService] Liquidity Vault auto-bind skipped: wallet has no primary address");
                }
            }
        }
    } catch (const std::exception& e) {
        logger_interface_->warning(
            "[WalletService] Liquidity Vault auto-bind failed: " + std::string(e.what()));
    }

    return wallet_mgr_->hasActiveWallet();
}

void WalletService::Stop() {
    if (!wallet_mgr_) {
        logger_interface_->info("[WalletService] Already stopped");
        return;
    }

    logger_interface_->info("[WalletService] Stopping wallet service...");

    try {
        // Shutdown wallet worker thread
        logger_interface_->info("[WalletService] Shutting down wallet worker thread...");
        WalletNotify::Shutdown();
        logger_interface_->info("[WalletService] Wallet worker thread stopped");

        // If a wallet is currently open, close it cleanly
        if (wallet_mgr_->hasActiveWallet()) {
            std::string current = wallet_mgr_->getCurrentWalletName();
            logger_interface_->info("[WalletService] Closing active wallet: " + current);

            // WalletManager destructor will handle cleanup
            // No explicit close() method needed
        }

        // WalletManager only borrows the HDWallet pointer; release service ownership explicitly.
        wallet_mgr_->setHDWallet(nullptr);
        hd_wallet_.reset();

        // Week 5: Bridge pattern removed - no longer clearing legacy global
        // Reset the unique_ptr (calls WalletManager destructor)
        wallet_mgr_.reset();

        logger_interface_->info("[WalletService] Wallet service stopped cleanly");

    } catch (const std::exception& e) {
        logger_interface_->error("[WalletService] Error during shutdown: " + std::string(e.what()));
        // Still reset to avoid dangling pointer
        wallet_mgr_.reset();
    }
}

} // namespace dinero
