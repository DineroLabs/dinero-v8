#pragma once
#include "wallet/wallet_services.h"
#include "wallet/hd_wallet.h"
#include "wallet/utxo_index.h"
#include <stdexcept>
#include <string>

/**
 * Wallet API: Thin, testable layer for wallet operations
 *
 * Keeps RPC handlers dumb by providing clean functions that:
 * - Handle wallet service access
 * - Validate preconditions (wallet exists, unlocked, etc.)
 * - Throw clear exceptions on errors
 * - Return simple, JSON-serializable types
 */

namespace walletapi {

// Error handling
class WalletError : public std::runtime_error {
public:
    WalletError(int code, const std::string& msg)
        : std::runtime_error(msg), code_(code) {}

    int code() const { return code_; }

private:
    int code_;
};

// Helper: require param utility
inline std::string require_string(const din::Json& params, const char* key) {
    if (!params.isMember(key) || !params[key].isString()) {
        throw WalletError(-32602, std::string("Missing or invalid '") + key + "' parameter");
    }
    return params[key].asString();
}

inline int64_t require_int64(const din::Json& params, const char* key) {
    if (!params.isMember(key) || !params[key].isInt64()) {
        throw WalletError(-32602, std::string("Missing or invalid '") + key + "' parameter");
    }
    return params[key].asInt64();
}

inline double require_double(const din::Json& params, const char* key) {
    if (!params.isMember(key) || !params[key].isDouble()) {
        throw WalletError(-32602, std::string("Missing or invalid '") + key + "' parameter");
    }
    return params[key].asDouble();
}

// ═══════════════════════════════════════════════════════════════
// Read-Only Wallet Operations (Batch 1)
// ═══════════════════════════════════════════════════════════════

/**
 * Get wallet balance from UTXO index
 * Returns balance in una
 */
inline int64_t GetBalance() {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_utxo_index()) {
        throw WalletError(-1, "UTXO index not available");
    }

    return g_wallet_services->utxo_index()->GetBalance();
}

/**
 * Generate new HD wallet address
 * Requires unlocked wallet
 */
inline std::string GetNewAddress() {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded. Call createhdwallet or restorewallet first.");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();
    if (wallet->IsEncrypted() && g_wallet_services->is_locked()) {
        throw WalletError(-13, "Wallet is locked. Use 'walletunlock' to unlock it first.");
    }

    std::string address = wallet->DeriveNextAddress();
    if (address.empty()) {
        throw WalletError(-1, "Failed to derive address from HD wallet");
    }

    return address;
}

/**
 * List all wallet addresses
 */
inline std::vector<std::string> ListAddresses() {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();
    return wallet->GetAllAddresses();
}

/**
 * List unspent transaction outputs
 */
inline std::vector<WalletUTXO> ListUnspent(uint32_t min_confirmations = 1) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();
    return wallet->ListUTXOs(min_confirmations);
}

/**
 * Get wallet information
 */
struct WalletInfo {
    std::string wallet_name;
    bool encrypted;
    bool unlocked;
    int64_t balance;
    size_t address_count;
    std::string wallet_dir;
};

inline WalletInfo GetWalletInfo() {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    WalletInfo info;
    info.wallet_name = "default";
    info.encrypted = wallet->IsEncrypted();
    info.unlocked = !g_wallet_services->is_locked();
    info.balance = GetBalance();
    info.address_count = wallet->GetAllAddresses().size();
    info.wallet_dir = g_wallet_services->wallet_datadir();

    return info;
}

// ═══════════════════════════════════════════════════════════════
// Security & Management Operations (Batch 2)
// ═══════════════════════════════════════════════════════════════

/**
 * Lock encrypted wallet
 * Makes wallet read-only until unlocked
 */
inline void LockWallet() {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (!wallet->IsEncrypted()) {
        throw WalletError(-15, "Wallet is not encrypted. Locking not required - wallet is always accessible.");
    }

    // Lock the wallet
    wallet->Lock();
    g_wallet_services->set_locked(true);
}

/**
 * Unlock encrypted wallet for specified duration
 * @param passphrase Wallet encryption passphrase
 * @param timeout_seconds How long to keep wallet unlocked (0 = indefinite)
 */
inline void UnlockWallet(const std::string& passphrase, int timeout_seconds) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (!wallet->IsEncrypted()) {
        throw WalletError(-15, "Wallet is not encrypted. Unlock not required - wallet is always accessible.");
    }

    if (passphrase.empty()) {
        throw WalletError(-32602, "Passphrase cannot be empty");
    }

    // Attempt to unlock
    bool success = wallet->Unlock(passphrase);
    if (!success) {
        throw WalletError(-14, "Incorrect passphrase");
    }

    // Update global lock state
    g_wallet_services->set_locked(false);

    // TODO: Implement timeout logic (requires timer/thread management)
    // For now, timeout parameter is accepted but not enforced
}

/**
 * Encrypt wallet with passphrase (first-time encryption)
 * @param passphrase New encryption passphrase
 */
inline void EncryptWallet(const std::string& passphrase) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (wallet->IsEncrypted()) {
        throw WalletError(-15, "Wallet is already encrypted. Use 'walletpassphrasechange' to change passphrase.");
    }

    if (passphrase.empty()) {
        throw WalletError(-32602, "Passphrase cannot be empty");
    }

    if (passphrase.length() < 8) {
        throw WalletError(-32602, "Passphrase must be at least 8 characters");
    }

    // Encrypt the wallet
    bool success = wallet->EncryptWallet(passphrase);
    if (!success) {
        throw WalletError(-1, "Failed to encrypt wallet");
    }

    // Wallet is now encrypted and locked
    g_wallet_services->set_locked(true);
}

/**
 * Change wallet encryption passphrase
 * @param old_passphrase Current passphrase
 * @param new_passphrase New passphrase
 */
inline void ChangePassphrase(const std::string& old_passphrase, const std::string& new_passphrase) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (!wallet->IsEncrypted()) {
        throw WalletError(-15, "Wallet is not encrypted. Use 'encryptwallet' first.");
    }

    if (old_passphrase.empty() || new_passphrase.empty()) {
        throw WalletError(-32602, "Passphrases cannot be empty");
    }

    if (new_passphrase.length() < 8) {
        throw WalletError(-32602, "New passphrase must be at least 8 characters");
    }

    // Change passphrase
    bool success = wallet->ChangePassword(old_passphrase, new_passphrase);
    if (!success) {
        throw WalletError(-14, "Incorrect old passphrase or encryption failed");
    }

    // Wallet remains in its current lock state
}

/**
 * Create new HD wallet
 * Returns mnemonic, fingerprint, and first address
 */
struct CreateWalletResult {
    std::string mnemonic;
    std::string fingerprint;
    std::string first_address;
    std::string wallet_path;
    size_t word_count;
};

inline CreateWalletResult CreateHDWallet(const std::string& passphrase = "") {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (g_wallet_services->has_hd_wallet()) {
        throw WalletError(-4, "Wallet already exists. Use 'restorewallet' to restore from seed.");
    }

    std::string wallet_path = g_wallet_services->wallet_datadir();
    if (wallet_path.empty()) {
        throw WalletError(-1, "Wallet directory not configured");
    }

    // Create new HD wallet (this would normally be done in main.cpp)
    // For now, throw error indicating this needs special handling
    throw WalletError(-1, "createhdwallet requires global wallet initialization - use legacy RPC temporarily");
}

/**
 * Restore HD wallet from mnemonic
 * @param mnemonic BIP39 mnemonic phrase
 * @param passphrase Optional encryption passphrase
 * Returns list of restored addresses
 */
struct RestoreWalletResult {
    std::string fingerprint;
    std::string first_address;
    std::vector<std::string> addresses;
    size_t addresses_restored;
    std::string wallet_path;
};

inline RestoreWalletResult RestoreWallet(const std::string& mnemonic, const std::string& passphrase = "") {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (mnemonic.empty()) {
        throw WalletError(-32602, "Mnemonic cannot be empty");
    }

    // Restore wallet (this would normally be done in main.cpp)
    // For now, throw error indicating this needs special handling
    throw WalletError(-1, "restorewallet requires global wallet initialization - use legacy RPC temporarily");
}

// ═══════════════════════════════════════════════════════════════
// Transactional Operations (Batch 3)
// ═══════════════════════════════════════════════════════════════

/**
 * Send coins to address
 * Creates, signs, and returns transaction hex
 * @param dest_address Destination address
 * @param amount_una Amount in una (una)
 * @param fee_rate Fee rate in una per vbyte
 * @return Transaction hex string
 */
struct SendToAddressResult {
    std::string txhex;
    std::string txid;
    uint64_t amount_una;
    uint64_t fee_una;
};

inline SendToAddressResult SendToAddress(const std::string& dest_address, uint64_t amount_una, uint64_t fee_rate = 1) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (wallet->IsEncrypted() && g_wallet_services->is_locked()) {
        throw WalletError(-13, "Wallet is locked. Use 'walletunlock' first.");
    }

    if (dest_address.empty()) {
        throw WalletError(-32602, "Destination address cannot be empty");
    }

    if (amount_una == 0) {
        throw WalletError(-32602, "Amount must be greater than zero");
    }

    // Build outputs
    std::vector<HDWallet::TxOutput> outputs;
    HDWallet::TxOutput output;
    output.address = dest_address;
    output.value = amount_una;
    outputs.push_back(output);

    // Create and sign transaction
    std::string tx_hex;
    std::string error;
    if (!wallet->CreateTransaction(outputs, fee_rate, tx_hex, error)) {
        throw WalletError(-1, "Transaction creation failed: " + error);
    }

    SendToAddressResult result;
    result.txhex = tx_hex;
    result.amount_una = amount_una;
    result.fee_una = 0; // TODO: Calculate actual fee from transaction
    // TODO: Calculate txid from tx_hex
    result.txid = "";

    return result;
}

/**
 * List wallet transactions
 * Returns list of received transactions from UTXOs
 */
struct TransactionInfo {
    std::string txid;
    std::string type;  // "receive" or "send"
    double amount_din;
    std::string address;
    uint32_t confirmations;
    bool is_coinbase;
};

inline std::vector<TransactionInfo> ListTransactions(uint32_t count = 10) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();
    auto utxos = wallet->ListUTXOs(0); // Get all UTXOs

    // Build a map of txid -> transaction details
    std::map<std::string, TransactionInfo> txMap;

    for (const auto& utxo : utxos) {
        if (txMap.find(utxo.txid) == txMap.end()) {
            TransactionInfo tx;
            tx.txid = utxo.txid;
            tx.type = "receive"; // For now, all UTXOs are receives
            tx.amount_din = 0.0;
            tx.address = utxo.address;
            tx.confirmations = utxo.confirmations;
            tx.is_coinbase = utxo.is_coinbase;
            txMap[utxo.txid] = tx;
        }

        // Sum up outputs from the same transaction
        double amount_din = static_cast<double>(utxo.value) / 1e8;
        txMap[utxo.txid].amount_din += amount_din;
    }

    // Convert map to vector
    std::vector<TransactionInfo> result;
    for (const auto& pair : txMap) {
        result.push_back(pair.second);
        if (result.size() >= count) break;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Advanced Operations (Batch 4 - Pragmatic Stubs)
// These methods are migrated to vNext but delegate to existing
// wallet implementations for complex functionality
// ═══════════════════════════════════════════════════════════════

/**
 * Backup wallet - Export mnemonic phrase
 */
inline std::string BackupWallet() {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (wallet->IsEncrypted() && g_wallet_services->is_locked()) {
        throw WalletError(-13, "Wallet is locked. Use 'walletunlock' first.");
    }

    std::string mnemonic = wallet->GetMnemonic();
    if (mnemonic.empty()) {
        throw WalletError(-1, "Failed to retrieve mnemonic");
    }

    return mnemonic;
}

/**
 * Derive address at specific index
 */
inline std::string DeriveAddress(uint32_t index) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (wallet->IsEncrypted() && g_wallet_services->is_locked()) {
        throw WalletError(-13, "Wallet is locked. Use 'walletunlock' first.");
    }

    std::string address = wallet->GetAddressAt(index);
    if (address.empty()) {
        throw WalletError(-1, "Failed to derive address at index " + std::to_string(index));
    }

    return address;
}

/**
 * Dump private key for address
 */
inline std::string DumpPrivKey(const std::string& address) {
    if (!g_wallet_services) {
        throw WalletError(-1, "Wallet services not initialized");
    }

    if (!g_wallet_services->has_hd_wallet()) {
        throw WalletError(-1, "No HD wallet loaded");
    }

    HDWallet* wallet = g_wallet_services->hd_wallet();

    if (wallet->IsEncrypted() && g_wallet_services->is_locked()) {
        throw WalletError(-13, "Wallet is locked. Use 'walletunlock' first.");
    }

    // For HD wallets, we don't export individual private keys
    // Users should use the mnemonic backup instead
    throw WalletError(-4, "dumpprivkey not implemented for HD wallets - use backupwallet to get mnemonic seed");
}

/**
 * Note: Complex methods like PSBT processing, raw transaction signing,
 * wallet rescan, and import operations are handled by their respective
 * RPC implementations which delegate to HDWallet/Transaction layer.
 * These don't need wallet_api wrappers as they're too complex for
 * a thin API layer.
 */

} // namespace walletapi
