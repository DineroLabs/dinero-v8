#pragma once
#include <memory>

// Forward declarations
class HDWallet;
namespace dinero {
    class UTXOIndex;
    class WalletManager;
}

/**
 * WalletServices: Central façade for wallet-related global state
 *
 * Replaces ad-hoc global variables scattered in main() with a single
 * service handle that provides clean access to wallet components.
 *
 * Initialized once at daemon startup, accessible throughout RPC layer.
 */
struct WalletServicesConfig {
    std::shared_ptr<std::shared_ptr<HDWallet>>* hd_wallet_ptr = nullptr;
    dinero::UTXOIndex* utxo_index = nullptr;
    std::shared_ptr<bool>* wallet_locked_ptr = nullptr;
    std::shared_ptr<std::string>* wallet_datadir_ptr = nullptr;
    std::unique_ptr<dinero::WalletManager>* wallet_manager_ptr = nullptr;
};

class WalletServices {
public:
    explicit WalletServices(WalletServicesConfig cfg) : cfg_(cfg) {}

    // HD Wallet access
    HDWallet* hd_wallet() const {
        if (!cfg_.hd_wallet_ptr || !*cfg_.hd_wallet_ptr || !(**cfg_.hd_wallet_ptr)) return nullptr;
        return (**cfg_.hd_wallet_ptr).get();
    }

    bool has_hd_wallet() const {
        return hd_wallet() != nullptr;
    }

    // UTXO Index access
    dinero::UTXOIndex* utxo_index() const {
        return cfg_.utxo_index;
    }

    bool has_utxo_index() const {
        return cfg_.utxo_index != nullptr;
    }

    // Wallet lock state
    bool is_locked() const {
        return (cfg_.wallet_locked_ptr && *cfg_.wallet_locked_ptr) ? **cfg_.wallet_locked_ptr : false;
    }

    void set_locked(bool locked) const {
        if (cfg_.wallet_locked_ptr && *cfg_.wallet_locked_ptr) {
            **cfg_.wallet_locked_ptr = locked;
        }
    }

    // Wallet data directory
    std::string wallet_datadir() const {
        return (cfg_.wallet_datadir_ptr && *cfg_.wallet_datadir_ptr) ? **cfg_.wallet_datadir_ptr : "";
    }

    // Wallet Manager access
    dinero::WalletManager* wallet_manager() const {
        return (cfg_.wallet_manager_ptr && *cfg_.wallet_manager_ptr) ? (*cfg_.wallet_manager_ptr).get() : nullptr;
    }

    bool has_wallet_manager() const {
        return wallet_manager() != nullptr;
    }

    // Wallet lifecycle management (for createhdwallet/restorewallet)
    void set_hd_wallet(std::shared_ptr<HDWallet> wallet) const {
        if (cfg_.hd_wallet_ptr && *cfg_.hd_wallet_ptr) {
            **cfg_.hd_wallet_ptr = wallet;
        }
    }

    // Get underlying shared_ptr for wallet manager wiring
    std::shared_ptr<HDWallet>* hd_wallet_shared_ptr() const {
        return (cfg_.hd_wallet_ptr && *cfg_.hd_wallet_ptr) ? cfg_.hd_wallet_ptr->get() : nullptr;
    }

private:
    WalletServicesConfig cfg_;
};

// Single global accessor (initialized in main)
extern WalletServices* g_wallet_services;
