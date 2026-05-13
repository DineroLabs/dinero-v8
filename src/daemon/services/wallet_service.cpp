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
                // Auto-create default wallet with empty passphrase (unencrypted for now)
                wallet_mgr_->create("default");

                // ⚠️ CRITICAL: Generate HD seed immediately after wallet creation
                // Without this, wallet is in illegal state (HD wallet with no seed)
                // Bitcoin Core enforces: HD wallet MUST have seed at creation time
                din::Json createhd_params;
                createhd_params[0] = "default";  // wallet name
                createhd_params[1] = 12;         // word count
                createhd_params[2] = "";         // bip39 passphrase (empty)
                createhd_params[3] = "";         // encryption password (empty for regtest/auto-creation)

                auto createhd_result = dinero::rpc::RpcCreateHDWallet(createhd_params, wallet_mgr_.get());
                if (createhd_result.isMember("error")) {
                    throw std::runtime_error("Failed to generate HD seed: " + createhd_result["error"].asString());
                }

                logger_interface_->info("[WalletService] ✅ HD seed generated for default wallet");
                logger_interface_->info("[WalletService] ✅ Auto-created and opened 'default' HD wallet with mnemonic");

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
