// wallet-core/ffi/wallet_ffi.cpp
// C API implementation for Dinero wallet FFI
// Wraps C++ WalletManager class in C API

// Include C++ headers first (before C API header)
#include "wallet/bip39.h"
#include "wallet/wallet_manager.h"
#include "wallet/transaction_builder.h"
#include "wallet/intent_descriptor.h"
#include "wallet/taproot_tx_signer.h"
#include "wallet/taproot_template_builder.h"
#include "wallet/policy_descriptor.h"
#include "wallet/safety_profile.h"
#include "wallet/protected_output_builder.h"
#include "wallet/protected_spend.h"
#include "wallet/recovery_qr.h"
#include "wallet/escrow_descriptor.h"
#include "wallet/receipt_bundle.h"
#include "wallet/escrow_state_machine.h"
#include "wallet/tx_classifier.h"
#include "wallet/lp_router.h"
#include "policy/rbf_policy.h"
#include "crypto/sha256.h"
#include "primitives/hash_domains.h"
#include "consensus/coin_type.h"
#include "consensus/tx_validation.h"
#include <openssl/crypto.h>  // For OPENSSL_cleanse (secure memory wipe)

// Now include C API header (defines FFI_ prefixed structs)
#include "wallet_ffi.h"

#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <mutex>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <cassert>
#include <utility>

// Include KYC provider abstraction
#include "kyc_provider.h"

using namespace dinero::kyc;

namespace {

// Debug-only guard against same-thread reentrant locking in FFI entrypoints.
// This catches accidental lock recursion before it turns into a silent deadlock.
class WalletMutex {
public:
    void lock() {
#ifndef NDEBUG
        assert(!held_by_this_thread_ && "wallet_ffi reentrant g_wallet_mutex lock attempt");
#endif
        mutex_.lock();
#ifndef NDEBUG
        held_by_this_thread_ = true;
#endif
    }

    void unlock() {
#ifndef NDEBUG
        assert(held_by_this_thread_ && "wallet_ffi unlock without lock ownership");
        held_by_this_thread_ = false;
#endif
        mutex_.unlock();
    }

private:
    std::mutex mutex_;
#ifndef NDEBUG
    static thread_local bool held_by_this_thread_;
#endif
};

#ifndef NDEBUG
thread_local bool WalletMutex::held_by_this_thread_ = false;
#endif

}  // namespace

// Global wallet instance (thread-safe singleton)
static WalletMutex g_wallet_mutex;
static std::unique_ptr<dinero::WalletManager> g_wallet_manager;

// Global error tracking
static DineroErrorCode g_last_error = DINERO_SUCCESS;
static std::string g_wallet_datadir;
static constexpr const char* kDefaultFFIWalletName = "default";

// Securely wipe sensitive strings before clearing.
// std::string::clear() does NOT zero the underlying buffer.
static void secure_clear_string(std::string& value) {
    if (!value.empty()) {
        OPENSSL_cleanse(value.data(), value.size());
        value.clear();
    }
}

// Securely wipe sensitive byte buffers before clearing.
static void secure_clear_bytes(std::vector<uint8_t>& value) {
    if (!value.empty()) {
        OPENSSL_cleanse(value.data(), value.size());
        value.clear();
    }
}

template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F&& f) : fn_(std::forward<F>(f)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { fn_(); }

private:
    F fn_;
};

template <typename F>
ScopeExit<F> MakeScopeExit(F&& f) {
    return ScopeExit<F>(std::forward<F>(f));
}

// Helper: Allocate C string from C++ string
static char* allocate_c_string(const std::string& str) {
    if (str.empty()) {
        return nullptr;
    }
    char* c_str = static_cast<char*>(malloc(str.size() + 1));
    if (c_str) {
        std::strcpy(c_str, str.c_str());
    }
    return c_str;
}

static bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }

    auto nibble = [](char c) -> int {
        unsigned char ch = static_cast<unsigned char>(c);
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        ch = static_cast<unsigned char>(std::tolower(ch));
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        return -1;
    };

    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Helpers: callers must hold g_wallet_mutex before calling these accessors.
// Public FFI entrypoints are responsible for acquiring the lock exactly once.
static dinero::WalletManager* get_wallet_manager_nolock() {
    return g_wallet_manager.get();
}

static bool ensure_active_wallet_nolock(dinero::WalletManager* wm, bool create_if_missing) {
    if (!wm) {
        return false;
    }

    if (wm->hasActiveWallet()) {
        return true;
    }

    if (wm->exists(kDefaultFFIWalletName)) {
        wm->open(kDefaultFFIWalletName);
        return wm->hasActiveWallet();
    }

    if (!create_if_missing) {
        return false;
    }

    wm->create(kDefaultFFIWalletName);
    return wm->hasActiveWallet();
}

// ============================================================================
// Wallet Initialization
// ============================================================================

int dinero_wallet_init(const char* datadir) {
    if (!datadir) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        g_wallet_datadir = datadir;
        
        // Initialize WalletManager
        g_wallet_manager = std::make_unique<dinero::WalletManager>(datadir);
        if (g_wallet_manager->exists(kDefaultFFIWalletName)) {
            g_wallet_manager->open(kDefaultFFIWalletName);
        }
        
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_create(char** mnemonic_out) {
    if (!mnemonic_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    std::string mnemonic;
    std::vector<uint8_t> seed;
    [[maybe_unused]] auto cleanup_sensitive = MakeScopeExit([&]() {
        secure_clear_bytes(seed);
        secure_clear_string(mnemonic);
    });
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, true)) {
            return -1;
        }

        if (wm->isWalletEncrypted()) {
            // Avoid replacing seed under encrypted metadata without migration.
            return -1;
        }

        mnemonic = dinero::bip39::Generate(dinero::bip39::WordCount::Words12);
        if (mnemonic.empty()) {
            return -1;
        }

        if (!dinero::bip39::MnemonicToSeed(mnemonic, "", seed)) {
            return -1;
        }

        if (!wm->storeMasterSeed(seed, "", true)) {
            return -1;
        }

        *mnemonic_out = allocate_c_string(mnemonic);
        if (!*mnemonic_out) {
            return -1;
        }

        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_restore(const char* mnemonic, const char* passphrase) {
    if (!mnemonic) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    std::string mnemonic_str;
    std::vector<uint8_t> seed;
    [[maybe_unused]] auto cleanup_sensitive = MakeScopeExit([&]() {
        secure_clear_bytes(seed);
        secure_clear_string(mnemonic_str);
    });
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, true)) {
            return -1;
        }

        if (wm->isWalletEncrypted()) {
            // Restoring into encrypted wallet requires dedicated migration flow.
            return -1;
        }

        mnemonic_str = mnemonic;
        if (!dinero::bip39::ValidateMnemonic(mnemonic_str)) {
            return -1;
        }

        const std::string bip39_passphrase = passphrase ? passphrase : "";
        if (!dinero::bip39::MnemonicToSeed(mnemonic_str, bip39_passphrase, seed)) {
            return -1;
        }

        if (!wm->storeMasterSeed(seed, "", true)) {
            return -1;
        }

        return 0;
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// Wallet Encryption & Locking
// ============================================================================

int dinero_wallet_encrypt(const char* password) {
    if (!password) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            return -1;
        }

        wm->encryptWallet(password);
        if (!wm->isWalletEncrypted()) {
            return -1;
        }
        
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_unlock(const char* password, int32_t timeout_seconds) {
    if (!password) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            return -1;
        }
        wm->unlockWallet(password, timeout_seconds);
        
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_lock() {
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            return -1;
        }

        wm->lockWallet();
        
        return 0;
    } catch (...) {
        return -1;
    }
}

bool dinero_wallet_is_encrypted() {
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            return false;
        }
        return wm->isWalletEncrypted();
    } catch (...) {
        return false;
    }
}

bool dinero_wallet_is_locked() {
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            return true;
        }
        return wm->isWalletLocked();
    } catch (...) {
        return true;
    }
}

// ============================================================================
// Address Operations
// ============================================================================

FFI_WalletBalance dinero_wallet_get_balance() {
    FFI_WalletBalance balance;
    balance.total = 0.0;
    balance.confirmed = 0.0;
    balance.unconfirmed = 0.0;
    balance.immature = 0.0;
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            return balance;
        }

        const auto wallet_balance = wm->getBalance();
        balance.total = wallet_balance.total;
        balance.confirmed = wallet_balance.confirmed;
        balance.unconfirmed = wallet_balance.unconfirmed;
        balance.immature = wallet_balance.immature;
    } catch (...) {
        // Return zero balance on error
    }
    
    return balance;
}

int dinero_wallet_get_new_address(const char* label, char** address_out) {
    if (!address_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            return -1;
        }

        std::string address = wm->getNewAddress(label ? label : "");
        if (address.empty()) {
            return -1;
        }

        *address_out = allocate_c_string(address);
        return *address_out ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_get_change_address(char** address_out) {
    if (!address_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            return -1;
        }

        std::string address = wm->getNewChangeAddress();
        if (address.empty()) {
            return -1;
        }
        *address_out = allocate_c_string(address);
        return *address_out ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_get_mining_address(char** address_out) {
    if (!address_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            return -1;
        }

        std::string address = wm->getMiningAddress();
        if (address.empty()) {
            address = wm->getNewAddress("mining");
            if (!address.empty()) {
                wm->setMiningAddress(address, "", "");
            }
        }
        if (address.empty()) {
            return -1;
        }
        *address_out = allocate_c_string(address);
        return *address_out ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_set_label(const char* address, const char* label) {
    if (!address || !label) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            return -1;
        }
        
        wm->setAddressLabel(address, label);
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_get_label(const char* address, char** label_out) {
    if (!address || !label_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            return -1;
        }
        
        auto label = wm->getAddressLabel(address);
        if (label.has_value()) {
            *label_out = allocate_c_string(label.value());
        } else {
            *label_out = nullptr;
        }
        
        return 0;
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// Transaction Operations
// ============================================================================

int dinero_wallet_send_transaction(
    const char* to,
    double amount,
    double fee_rate,
    const char* note,
    char** txid_out
) {
    if (!to || !txid_out || amount <= 0) {
        return -1;
    }

    (void)note;
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            return -1;
        }

        auto* utxo_index = wm->getUTXOIndex();
        if (!utxo_index) {
            return -1;
        }

        dinero::TransactionBuilder builder(utxo_index);
        dinero::TransactionBuilder::BuildOptions options;
        options.fee_rate = fee_rate > 0.0 ? fee_rate : 1.0;

        const uint64_t amount_una = static_cast<uint64_t>(amount * 1e8);
        std::vector<dinero::TransactionBuilder::Recipient> recipients;
        recipients.push_back({std::string(to), static_cast<int64_t>(amount_una)});

        std::map<std::string, std::string> private_keys;
        for (const auto& utxo : wm->listUnspentUTXOs(0, 9999999)) {
            if (utxo.derivation_path.empty()) {
                continue;
            }
            std::string key_hex = wm->getPrivateKeyForPath(utxo.derivation_path);
            if (!key_hex.empty()) {
                private_keys[utxo.derivation_path] = std::move(key_hex);
            }
        }

        if (private_keys.empty()) {
            return -1;
        }

        auto result = builder.BuildTransaction(recipients, private_keys, options);
        if (!result.success) {
            return -1;
        }

        const auto txid = dinero::TxId::Compute(result.transaction).AsUint256().GetHex();
        *txid_out = allocate_c_string(txid);
        return *txid_out ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_list_utxos(
    int32_t min_confirmations,
    FFI_WalletUTXO** utxos_out,
    int32_t* count_out
) {
    if (!utxos_out || !count_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            *utxos_out = nullptr;
            *count_out = 0;
            return -1;
        }

        auto utxos = wm->listUnspentUTXOs(min_confirmations);
        
        if (utxos.empty()) {
            *utxos_out = nullptr;
            *count_out = 0;
            return 0;
        }
        
        // Allocate UTXO array (use FFI struct)
        FFI_WalletUTXO* utxo_array = static_cast<FFI_WalletUTXO*>(
            malloc(sizeof(FFI_WalletUTXO) * utxos.size())
        );
        
        if (!utxo_array) {
            return -1;
        }
        
        // Fill UTXO array from WalletManager rows
        for (size_t i = 0; i < utxos.size(); i++) {
            const auto& utxo = utxos[i];
            utxo_array[i].txid = allocate_c_string(utxo.txid);
            utxo_array[i].vout = static_cast<int32_t>(utxo.vout);
            utxo_array[i].address = allocate_c_string(utxo.address);
            utxo_array[i].amount = utxo.amount_din;
            utxo_array[i].confirmations = utxo.confirmations;
            utxo_array[i].spendable = utxo.spendable;
            utxo_array[i].coinbase = utxo.is_coinbase;
        }
        
        *utxos_out = utxo_array;
        *count_out = static_cast<int32_t>(utxos.size());
        
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_list_addresses(
    FFI_WalletAddress** addresses_out,
    int32_t* count_out
) {
    if (!addresses_out || !count_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            *addresses_out = nullptr;
            *count_out = 0;
            return -1;
        }

        auto addresses = wm->listAddresses(true);
        if (addresses.empty()) {
            *addresses_out = nullptr;
            *count_out = 0;
            return 0;
        }
        
        // Allocate address array
        FFI_WalletAddress* addr_array = static_cast<FFI_WalletAddress*>(
            malloc(sizeof(FFI_WalletAddress) * addresses.size())
        );
        
        if (!addr_array) {
            return -1;
        }
        
        // Fill address array
        for (size_t i = 0; i < addresses.size(); i++) {
            const auto& addr = addresses[i];
            const uint32_t purpose = (addr.type == "p2tr") ? 86u : 84u;
            const std::string path =
                "m/" + std::to_string(purpose) + "'/" +
                std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/" +
                std::to_string(addr.account) + "'/" +
                std::to_string(addr.change) + "/" +
                std::to_string(addr.index);

            addr_array[i].address = allocate_c_string(addr.address);
            addr_array[i].path = allocate_c_string(path);
            addr_array[i].index = static_cast<int32_t>(addr.index);
            addr_array[i].balance = wm->getAddressBalance(addr.address).total;
        }
        
        *addresses_out = addr_array;
        *count_out = static_cast<int32_t>(addresses.size());
        
        return 0;
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// Intent & SigHash V1 (B3)
// ============================================================================

int dinero_wallet_send_with_intent(
    const char* to,
    double amount,
    double fee_rate,
    const FFI_IntentDescriptor* intent,
    char** txid_out
) {
    if (!to || !txid_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            return -1;
        }

        if (wm->isWalletLocked()) {
            g_last_error = DINERO_ERROR_WALLET_LOCKED;
            return -1;
        }

        // Compute ext_commitment from intent (or use default zeros)
        std::array<uint8_t, 32> ext_commitment = dinero::DEFAULT_EXT_COMMITMENT;

        if (intent) {
            dinero::IntentDescriptor desc;
            std::copy(intent->recipient_hash, intent->recipient_hash + 32,
                      desc.recipient_hash.begin());
            desc.amount = static_cast<uint64_t>(intent->amount * 1e8);
            desc.max_fee = static_cast<uint64_t>(intent->max_fee * 1e8);
            desc.expiry_height = intent->expiry_height;
            desc.purpose_tag = std::string(intent->purpose_tag);
            ext_commitment = desc.ComputeExtCommitment();
        }

        // Build transaction using TransactionBuilder, then sign with V1

        auto* utxo_index = wm->getUTXOIndex();
        if (!utxo_index) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        dinero::TransactionBuilder builder(utxo_index);
        dinero::TransactionBuilder::BuildOptions options;
        options.fee_rate = fee_rate > 0.0 ? fee_rate : 1.0;

        uint64_t amount_una = static_cast<uint64_t>(amount * 1e8);
        std::vector<dinero::TransactionBuilder::Recipient> recipients;
        recipients.push_back({std::string(to), static_cast<int64_t>(amount_una)});

        // Preview (builds unsigned tx with coin selection)
        auto result = builder.PreviewTransaction(recipients, options);
        if (!result.success) {
            g_last_error = DINERO_ERROR_INSUFFICIENT_FUNDS;
            return -1;
        }

        // Sign each input with V1 sighash
        for (size_t i = 0; i < result.selected_utxos.size(); ++i) {
            const auto& path = result.selected_utxos[i].path;
            std::string key_hex = wm->getPrivateKeyForPath(path);
            if (key_hex.empty()) {
                g_last_error = DINERO_ERROR_GENERIC;
                return -1;
            }

            std::vector<uint8_t> key_bytes;
            if (!hex_to_bytes(key_hex, key_bytes) || key_bytes.size() != 32) {
                g_last_error = DINERO_ERROR_GENERIC;
                return -1;
            }

            if (!dinero::TaprootTxSigner::SignInputV1(
                    result.transaction, i, result.selected_utxos,
                    key_bytes, ext_commitment)) {
                g_last_error = DINERO_ERROR_GENERIC;
                return -1;
            }
        }

        auto computed_txid = dinero::TxId::Compute(result.transaction);
        *txid_out = allocate_c_string(computed_txid.AsUint256().GetHex());
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_compute_ext_commitment(
    const FFI_IntentDescriptor* intent,
    uint8_t ext_commitment_out[32]
) {
    if (!intent || !ext_commitment_out) {
        return -1;
    }

    try {
        dinero::IntentDescriptor desc;
        std::copy(intent->recipient_hash, intent->recipient_hash + 32,
                  desc.recipient_hash.begin());
        desc.amount = static_cast<uint64_t>(intent->amount * 1e8);
        desc.max_fee = static_cast<uint64_t>(intent->max_fee * 1e8);
        desc.expiry_height = intent->expiry_height;
        desc.purpose_tag = std::string(intent->purpose_tag);

        auto commitment = desc.ComputeExtCommitment();
        std::copy(commitment.begin(), commitment.end(), ext_commitment_out);

        return 0;
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// RBF & Timelock Operations (B1a+B1b)
// ============================================================================

int dinero_wallet_bump_fee(
    const char* original_txid,
    double new_fee_rate,
    char** new_txid_out
) {
    if (!original_txid || !new_txid_out || new_fee_rate <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            return -1;
        }

        if (wm->isWalletLocked()) {
            g_last_error = DINERO_ERROR_WALLET_LOCKED;
            return -1;
        }

        // Look up original transaction from wallet history
        auto txs = wm->getTransactionHistory(10000, 0);
        bool found = false;
        for (const auto& tx : txs) {
            if (tx.txid == original_txid) {
                found = true;
                break;
            }
        }

        if (!found) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        // Validate RBF signaling on original transaction
        // BIP125: At least one input must have nSequence < 0xfffffffe
        // The original transaction was built with enable_rbf=true (default),
        // so nSequence = 0xfffffffe which signals RBF.

        // Rebuild transaction with higher fee rate using the same UTXOs
        // In production, this would:
        // 1. Deserialize the original tx from the wallet's tx store
        // 2. Verify isRBFSignaled() via rbf_policy.h
        // 3. Rebuild with new_fee_rate, keeping same inputs/outputs
        // 4. Validate checkHigherFee() and checkPaysForBandwidth()
        // 5. Sign with wallet keys
        // 6. Return new txid

        // TODO: Wire through full RBF replacement pipeline once tx store is available
        // For now, create a new transaction with higher fee to the same destinations
        g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_send_timelocked(
    const char* to,
    double amount,
    double fee_rate,
    uint32_t absolute_locktime,
    uint32_t relative_locktime_blocks,
    char** txid_out
) {
    if (!to || !txid_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false)) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            return -1;
        }

        if (wm->isWalletLocked()) {
            g_last_error = DINERO_ERROR_WALLET_LOCKED;
            return -1;
        }

        // Get UTXOIndex from wallet manager
        auto* utxo_index = wm->getUTXOIndex();
        if (!utxo_index) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        // Build transaction with timelock options
        dinero::TransactionBuilder builder(utxo_index);
        dinero::TransactionBuilder::BuildOptions options;
        options.fee_rate = fee_rate > 0.0 ? fee_rate : 1.0;
        options.enable_rbf = true;  // Timelocked txs should still be RBF-able

        if (absolute_locktime > 0) {
            options.absolute_locktime = absolute_locktime;
        }

        // Apply relative locktime to all inputs
        if (relative_locktime_blocks > 0) {
            // We don't know input count yet, so we set it for indices 0-99
            // BuildUnsignedTransaction will only use indices that exist
            for (uint32_t i = 0; i < 100; ++i) {
                options.relative_locktime_blocks[i] = relative_locktime_blocks;
            }
        }

        // Convert amount to una
        uint64_t amount_una = static_cast<uint64_t>(amount * 1e8);

        std::vector<dinero::TransactionBuilder::Recipient> recipients;
        recipients.push_back({std::string(to), static_cast<int64_t>(amount_una)});

        // Build private keys map (path -> hex key) for signing
        std::map<std::string, std::string> private_keys;
        for (const auto& utxo : wm->listUnspentUTXOs(0, 9999999)) {
            if (utxo.derivation_path.empty()) {
                continue;
            }
            std::string key_hex = wm->getPrivateKeyForPath(utxo.derivation_path);
            if (!key_hex.empty()) {
                private_keys[utxo.derivation_path] = std::move(key_hex);
            }
        }

        if (private_keys.empty()) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        auto result = builder.BuildTransaction(recipients, private_keys, options);
        if (!result.success) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        auto computed_txid = dinero::TxId::Compute(result.transaction);
        *txid_out = allocate_c_string(computed_txid.AsUint256().GetHex());

        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_is_timelock_mature(
    const char* txid,
    uint32_t vout,
    uint32_t current_height,
    bool* is_mature_out
) {
    if (!txid || !is_mature_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            return -1;
        }

        auto* utxo_index = wm->getUTXOIndex();
        if (!utxo_index) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        // Look up the UTXO to get its confirmation height and sequence
        auto utxos = utxo_index->GetUnspentUTXOs();
        bool found = false;

        for (const auto& utxo : utxos) {
            if (utxo.txid.AsUint256().GetHex() == std::string(txid) &&
                static_cast<uint32_t>(utxo.vout) == vout) {
                found = true;

                // If height is 0, the tx is unconfirmed - not mature
                if (utxo.height == 0) {
                    *is_mature_out = false;
                    g_last_error = DINERO_SUCCESS;
                    return 0;
                }

                // BIP68 maturity: current_height - confirmation_height >= relative_lock
                // The relative lock value is encoded in nSequence of the *spending* tx input,
                // not in the UTXO itself. For this FFI function, we check if the UTXO
                // has enough confirmations to satisfy a typical relative lock.
                // The actual sequence lock check happens at consensus via checkSequenceLocks().
                uint32_t confirmations = current_height - static_cast<uint32_t>(utxo.height) + 1;

                // For coinbase UTXOs, must have 100 confirmations
                if (utxo.is_coinbase && confirmations < 100) {
                    *is_mature_out = false;
                } else {
                    // UTXO is confirmed and available for spending
                    // The spending tx's nSequence will be validated at consensus
                    *is_mature_out = true;
                }

                g_last_error = DINERO_SUCCESS;
                return 0;
            }
        }

        if (!found) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

// ============================================================================
// Policy Output Operations (B2)
// ============================================================================

int dinero_wallet_create_policy_output(
    uint8_t template_type,
    const uint8_t* params,
    size_t params_len,
    double amount,
    double fee_rate,
    char** address_out,
    uint8_t policy_id_out[32]
) {
    if (!params || params_len == 0 || !address_out || !policy_id_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    if (template_type < 1 || template_type > 2) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::TemplateTreeResult tree_result;
        std::vector<uint8_t> params_vec(params, params + params_len);

        if (template_type == 1) {
            // PROTECTED
            auto p = dinero::ProtectedTemplateParams::Deserialize(params_vec);
            tree_result = dinero::TaprootTemplateBuilder::BuildProtected(p);
        } else {
            // ESCROW
            auto p = dinero::EscrowTemplateParams::Deserialize(params_vec);
            tree_result = dinero::TaprootTemplateBuilder::BuildEscrow(p);
        }

        // Copy policy_id to output
        std::memcpy(policy_id_out, tree_result.policy_id.data(), 32);

        // TODO: Store policy in wallet_policies DB table
        // TODO: Build a transaction sending `amount` to tree_result.scriptPubKey
        // TODO: Store utxo_policy mapping after broadcast

        // For now, return the bech32m address for the output key
        // The address encoding will be added when bech32m encoder is wired
        // Return hex of output key as placeholder
        std::string addr_hex;
        addr_hex.reserve(64);
        static const char hex_chars[] = "0123456789abcdef";
        for (uint8_t b : tree_result.output_key) {
            addr_hex.push_back(hex_chars[b >> 4]);
            addr_hex.push_back(hex_chars[b & 0x0f]);
        }
        *address_out = allocate_c_string(addr_hex);

        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (const std::exception& e) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_get_policy_for_utxo(
    const char* txid,
    uint32_t vout,
    FFI_PolicyInfo* info_out
) {
    if (!txid || !info_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        // TODO: Query utxo_policy table by txid+vout
        // TODO: Join with wallet_policies to get template info
        // For now, return NOT_FOUND - policy DB queries will be wired in B4
        g_last_error = DINERO_ERROR_NOT_FOUND;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

// ============================================================================
// Safety Profiles & Protected Spending (B4)
// ============================================================================

// In-memory safety profile manager (TODO: persist to DB)
static dinero::SafetyProfileManager g_safety_mgr;

int dinero_wallet_create_safety_profile(
    uint32_t panic_window_blocks,
    uint32_t recovery_delay_blocks,
    const char* name
) {
    if (!name) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    dinero::SafetyProfile profile;
    profile.panic_window_blocks = panic_window_blocks;
    profile.recovery_delay_blocks = recovery_delay_blocks;
    profile.profile_name = name;
    profile.is_active = false;

    if (!g_safety_mgr.CreateProfile(profile)) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    g_last_error = DINERO_SUCCESS;
    return 0;
}

int dinero_wallet_activate_safety_profile(const char* name) {
    if (!name) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    if (!g_safety_mgr.ActivateProfile(std::string(name))) {
        g_last_error = DINERO_ERROR_NOT_FOUND;
        return -1;
    }

    g_last_error = DINERO_SUCCESS;
    return 0;
}

int dinero_wallet_send_protected(
    const char* to,
    double amount,
    double fee_rate,
    char** txid_out,
    uint8_t policy_id_out[32]
) {
    if (!to || !txid_out || !policy_id_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        auto active = g_safety_mgr.GetActiveProfile();
        if (!active.has_value()) {
            g_last_error = DINERO_ERROR_NOT_FOUND;
            return -1;
        }

        // TODO: Get wallet, derive next key index, call ProtectedOutputBuilder::Build()
        // TODO: Build transaction to the protected output scriptPubKey
        // TODO: Sign and broadcast
        // TODO: Store policy + utxo_policy in DB

        g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_panic_cancel(
    const char* txid,
    uint32_t vout,
    const char* safe_address,
    char** cancel_txid_out
) {
    if (!txid || !safe_address || !cancel_txid_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        // TODO: Look up UTXO + policy from DB
        // TODO: Reconstruct template tree from policy params
        // TODO: Get panic private key from wallet
        // TODO: Build spend tx with nSequence >= panic_window_blocks
        // TODO: Call ProtectedSpend::SpendFromPanicLeaf()
        // TODO: Broadcast

        g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_recovery_claim(
    const char* txid,
    uint32_t vout,
    const char* recovery_address,
    char** recovery_txid_out
) {
    if (!txid || !recovery_address || !recovery_txid_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        // TODO: Look up UTXO + policy from DB
        // TODO: Reconstruct template tree
        // TODO: Get recovery private key from wallet
        // TODO: Build spend tx with nSequence >= recovery_delay_blocks
        // TODO: Call ProtectedSpend::SpendFromRecoveryLeaf()
        // TODO: Broadcast

        g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_export_recovery_qr(
    const char* passphrase,
    char** qr_data_out
) {
    (void)passphrase;
    (void)qr_data_out;
    // Permanent hardening: mnemonic is no longer cached in process memory.
    // Recovery QR export requires caller-provided mnemonic in a dedicated API.
    g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
    return -1;
}

int dinero_wallet_export_recovery_qr_from_mnemonic_bytes(
    const uint8_t* mnemonic_bytes,
    size_t mnemonic_len,
    const uint8_t* passphrase_bytes,
    size_t passphrase_len,
    const uint8_t* profile_data,
    size_t profile_data_len,
    char** qr_data_out
) {
    if (!mnemonic_bytes || mnemonic_len == 0 || !qr_data_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    if (passphrase_len > 0 && !passphrase_bytes) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    if (profile_data_len > 0 && !profile_data) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::string mnemonic;
    std::string passphrase;
    std::vector<uint8_t> profile;
    [[maybe_unused]] auto cleanup_sensitive = MakeScopeExit([&]() {
        secure_clear_bytes(profile);
        secure_clear_string(passphrase);
        secure_clear_string(mnemonic);
    });

    try {
        mnemonic.assign(reinterpret_cast<const char*>(mnemonic_bytes), mnemonic_len);
        if (!dinero::bip39::ValidateMnemonic(mnemonic)) {
            g_last_error = DINERO_ERROR_INVALID_MNEMONIC;
            return -1;
        }

        if (passphrase_len > 0) {
            passphrase.assign(reinterpret_cast<const char*>(passphrase_bytes), passphrase_len);
        }

        if (profile_data_len > 0) {
            profile.assign(profile_data, profile_data + profile_data_len);
        }

        const std::string qr_payload = dinero::RecoveryQR::Encode(mnemonic, profile, passphrase);
        *qr_data_out = allocate_c_string(qr_payload);
        if (!*qr_data_out) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_import_recovery_qr(
    const char* qr_data,
    const char* passphrase
) {
    if (!qr_data) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::string mnemonic;
    std::vector<uint8_t> profile_data;
    [[maybe_unused]] auto cleanup_sensitive = MakeScopeExit([&]() {
        secure_clear_bytes(profile_data);
        secure_clear_string(mnemonic);
    });

    try {
        std::string pw = passphrase ? passphrase : "";

        if (!dinero::RecoveryQR::Decode(qr_data, pw, mnemonic, profile_data)) {
            g_last_error = DINERO_ERROR_AUTHENTICATION;
            return -1;
        }

        // TODO: Restore wallet from mnemonic
        // TODO: If profile_data is non-empty, restore SafetyProfile

        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

// ============================================================================
// Escrow Operations (B5)
// ============================================================================

int dinero_wallet_create_escrow_output(
    const uint8_t buyer_pubkey[32],
    const uint8_t seller_pubkey[32],
    const uint8_t* attestor_pubkeys,
    uint8_t n_attestors,
    uint8_t threshold,
    uint32_t timeout_blocks,
    double amount,
    double fee_rate,
    char** address_out,
    uint8_t policy_id_out[32]
) {
    if (!buyer_pubkey || !seller_pubkey || !address_out || !policy_id_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    if (n_attestors > 0 && !attestor_pubkeys) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::EscrowTemplateParams params;
        params.buyer_pubkey.assign(buyer_pubkey, buyer_pubkey + 32);
        params.seller_pubkey.assign(seller_pubkey, seller_pubkey + 32);
        params.attestor_threshold = threshold;
        params.timeout_blocks = timeout_blocks;

        for (uint8_t i = 0; i < n_attestors; i++) {
            params.attestor_pubkeys.emplace_back(
                attestor_pubkeys + i * 32, attestor_pubkeys + (i + 1) * 32);
        }

        auto tree = dinero::TaprootTemplateBuilder::BuildEscrow(params);
        std::memcpy(policy_id_out, tree.policy_id.data(), 32);

        // Return hex of output key as placeholder address
        std::string addr_hex;
        addr_hex.reserve(64);
        static const char hex_chars[] = "0123456789abcdef";
        for (uint8_t b : tree.output_key) {
            addr_hex.push_back(hex_chars[b >> 4]);
            addr_hex.push_back(hex_chars[b & 0x0f]);
        }
        *address_out = allocate_c_string(addr_hex);

        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_sign_escrow_release(
    const char* txid,
    uint32_t vout,
    const uint8_t* receipt_data,
    size_t receipt_len,
    char** release_txid_out
) {
    if (!txid || !receipt_data || !release_txid_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        // TODO: Look up UTXO + policy from DB
        // TODO: Reconstruct escrow template tree
        // TODO: Deserialize receipt bundle, verify signatures
        // TODO: Build release tx via release leaf script-path
        // TODO: Sign with seller key + use attestor sigs in witness
        // TODO: Broadcast

        g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_sign_escrow_refund(
    const char* txid,
    uint32_t vout,
    char** refund_txid_out
) {
    if (!txid || !refund_txid_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        // TODO: Look up UTXO + policy from DB
        // TODO: Reconstruct escrow template tree
        // TODO: Build refund tx via timeout leaf (buyer key + CSV)
        // TODO: Sign with buyer key, set nSequence >= timeout_blocks
        // TODO: Broadcast

        g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_verify_receipt_bundle(
    const uint8_t* receipt_data,
    size_t receipt_len,
    const char* outcome,
    bool* valid_out
) {
    if (!receipt_data || !outcome || !valid_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    try {
        std::vector<uint8_t> data(receipt_data, receipt_data + receipt_len);
        auto bundle = dinero::ReceiptBundle::Deserialize(data);

        // Compute outcome hash = SHA256(outcome_string)
        std::array<uint8_t, 32> outcome_hash;
        dinero::crypto::CSHA256()
            .Write(reinterpret_cast<const uint8_t*>(outcome), std::strlen(outcome))
            .Finalize(outcome_hash.data());

        *valid_out = bundle.VerifyReceipts(outcome_hash);
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

// ============================================================================
// TX Classification & Extended Details (B7)
// ============================================================================

int dinero_wallet_get_tx_detail_extended(
    const char* txid,
    uint32_t vout,
    uint32_t current_height,
    FFI_ExtendedTxDetail* detail_out
) {
    if (!txid || !detail_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            return -1;
        }

        auto* utxo_index = wm->getUTXOIndex();
        if (!utxo_index) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        // Find the UTXO
        auto utxos = utxo_index->GetUnspentUTXOs();
        bool found = false;

        for (const auto& utxo : utxos) {
            if (utxo.txid.AsUint256().GetHex() == std::string(txid) &&
                static_cast<uint32_t>(utxo.vout) == vout) {
                found = true;

                // Convert WalletUTXO to CanonicalWalletUTXO for classifier
                dinero::CanonicalWalletUTXO canonical;
                canonical.txid = utxo.txid.AsUint256();
                canonical.vout = utxo.vout;
                canonical.value = utxo.value;
                canonical.spk = utxo.spk;
                canonical.height = static_cast<uint32_t>(utxo.height);
                canonical.is_coinbase = utxo.is_coinbase;
                canonical.path = utxo.path;

                auto detail = dinero::TxClassifier::GetExtendedDetail(canonical, current_height);

                // Fill FFI struct
                std::memset(detail_out, 0, sizeof(FFI_ExtendedTxDetail));
                detail_out->classification = static_cast<uint8_t>(detail.classification);

                if (detail.policy_id.has_value()) {
                    std::memcpy(detail_out->policy_id, detail.policy_id->data(), 32);
                }

                std::strncpy(detail_out->template_name,
                    detail.template_name.c_str(),
                    sizeof(detail_out->template_name) - 1);

                std::strncpy(detail_out->policy_description,
                    detail.policy_description.c_str(),
                    sizeof(detail_out->policy_description) - 1);

                detail_out->panic_remaining = detail.panic_window_remaining.has_value()
                    ? static_cast<int32_t>(detail.panic_window_remaining.value()) : -1;

                detail_out->recovery_remaining = detail.recovery_delay_remaining.has_value()
                    ? static_cast<int32_t>(detail.recovery_delay_remaining.value()) : -1;

                if (detail.escrow_state.has_value()) {
                    std::strncpy(detail_out->escrow_state,
                        detail.escrow_state->c_str(),
                        sizeof(detail_out->escrow_state) - 1);
                }

                g_last_error = DINERO_SUCCESS;
                return 0;
            }
        }

        if (!found) {
            g_last_error = DINERO_ERROR_NOT_FOUND;
            return -1;
        }

        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

// ============================================================================
// B6a: LP Router — Provider Quote Aggregation
// ============================================================================

static dinero::LPRouter g_lp_router;

static dinero::FiatCurrency parseFiatCurrency(const char* code) {
    if (!code) return dinero::FiatCurrency::USD;
    std::string s(code);
    if (s == "EUR") return dinero::FiatCurrency::EUR;
    if (s == "GBP") return dinero::FiatCurrency::GBP;
    if (s == "MXN") return dinero::FiatCurrency::MXN;
    if (s == "BRL") return dinero::FiatCurrency::BRL;
    if (s == "ARS") return dinero::FiatCurrency::ARS;
    return dinero::FiatCurrency::USD;
}

static void fillFFIQuote(FFI_ProviderQuote* out, const dinero::ProviderQuote& q) {
    std::memset(out, 0, sizeof(FFI_ProviderQuote));
    std::strncpy(out->provider_id, q.provider_id.c_str(), sizeof(out->provider_id) - 1);
    std::strncpy(out->provider_name, q.provider_name.c_str(), sizeof(out->provider_name) - 1);
    out->rate = q.rate;
    out->min_amount_una = q.min_amount_una;
    out->max_amount_una = q.max_amount_una;
    out->fee_percent = q.fee_percent;
    out->fee_fixed_fiat = q.fee_fixed_fiat;
    out->estimated_seconds = q.estimated_seconds;
    std::strncpy(out->payment_method, q.payment_method.c_str(), sizeof(out->payment_method) - 1);
    out->requires_kyc = q.requires_kyc;
}

int dinero_wallet_get_router_quotes(
    uint8_t direction,
    const char* currency,
    uint64_t amount_una,
    const char* country_code,
    FFI_ProviderQuote** quotes_out,
    int32_t* count_out
) {
    if (!currency || !quotes_out || !count_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::QuoteRequest req;
        req.direction = (direction == 0)
            ? dinero::QuoteDirection::BUY
            : dinero::QuoteDirection::SELL;
        req.currency = parseFiatCurrency(currency);
        req.amount_una = amount_una;
        req.fiat_amount = 0.0;
        req.country_code = country_code ? country_code : "";

        auto quotes = g_lp_router.GetQuotes(req);

        if (quotes.empty()) {
            *quotes_out = nullptr;
            *count_out = 0;
            g_last_error = DINERO_SUCCESS;
            return 0;
        }

        auto* arr = static_cast<FFI_ProviderQuote*>(
            std::malloc(sizeof(FFI_ProviderQuote) * quotes.size()));
        if (!arr) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }

        for (size_t i = 0; i < quotes.size(); ++i) {
            fillFFIQuote(&arr[i], quotes[i]);
        }

        *quotes_out = arr;
        *count_out = static_cast<int32_t>(quotes.size());
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_select_provider(
    uint8_t direction,
    const char* currency,
    uint64_t amount_una,
    const char* country_code,
    FFI_ProviderQuote* quote_out
) {
    if (!currency || !quote_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);

    try {
        dinero::QuoteRequest req;
        req.direction = (direction == 0)
            ? dinero::QuoteDirection::BUY
            : dinero::QuoteDirection::SELL;
        req.currency = parseFiatCurrency(currency);
        req.amount_una = amount_una;
        req.fiat_amount = 0.0;
        req.country_code = country_code ? country_code : "";

        auto best = g_lp_router.SelectBestProvider(req);
        if (!best.has_value()) {
            g_last_error = DINERO_ERROR_NOT_FOUND;
            return -1;
        }

        fillFFIQuote(quote_out, best.value());
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

void dinero_wallet_free_quotes(FFI_ProviderQuote* ptr, int32_t count) {
    (void)count;
    std::free(ptr);
}

// ============================================================================
// Backup & Recovery
// ============================================================================

int dinero_wallet_backup_file(const char* filepath, char** hash_out) {
    if (!filepath || !hash_out) {
        return -1;
    }

    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            return -1;
        }
        
        // TODO: Implement backup file creation
        // For now, return placeholder hash
        *hash_out = allocate_c_string("0000000000000000000000000000000000000000000000000000000000000000");
        
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_get_mnemonic(char** mnemonic_out) {
    (void)mnemonic_out;
    // Permanent hardening: no mnemonic readback API from process memory.
    g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
    return -1;
}

// ============================================================================
// Network Operations (TODO: Implement RPC client)
// ============================================================================

int dinero_wallet_connect_rpc(const char* rpc_url) {
    // TODO: Implement RPC client connection
    // For now, just store URL for future use
    (void)rpc_url;
    return 0;
}

int dinero_wallet_sync_balance() {
    // TODO: Implement balance sync from RPC
    // For now, balance is updated via UTXO tracking
    return 0;
}

int dinero_wallet_broadcast_tx(const char* tx_hex, char** txid_out) {
    // TODO: Implement transaction broadcasting via RPC
    // For now, return placeholder
    if (!tx_hex || !txid_out) {
        return -1;
    }
    *txid_out = allocate_c_string("0000000000000000000000000000000000000000000000000000000000000000");
    return 0;
}

// ============================================================================
// Payment UX Features
// ============================================================================

int dinero_wallet_export_transactions(const char* format, const char* dest) {
    // Use batched export with default batch size
    return dinero_wallet_export_transactions_batched(format, dest, 0, nullptr);
}

int dinero_wallet_export_transactions_batched(
    const char* format,
    const char* dest,
    int32_t batch_size,
    void (*callback)(const char* txid, double progress)
) {
    if (!format || !dest) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }

    try {
        std::vector<dinero::WalletManager::TransactionInfo> txs;
        {
            std::lock_guard<WalletMutex> lock(g_wallet_mutex);
            dinero::WalletManager* wm = get_wallet_manager_nolock();
            if (!wm) {
                g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
                return -1;
            }

            // Snapshot history under lock, then release lock before file I/O
            // and user callback invocation to avoid re-entrant deadlocks.
            txs = wm->getTransactionHistory(10000, 0); // Get up to 10k transactions
        }
        
        if (txs.empty()) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }
        
        // Auto-detect batch size if not specified
        if (batch_size <= 0) {
            batch_size = std::min(1000, static_cast<int32_t>(txs.size()));
        }
        
        std::ofstream out(dest);
        if (!out.is_open()) {
            g_last_error = DINERO_ERROR_FILE_IO;
            return -1;
        }
        
        std::string fmt(format);
        size_t total = txs.size();
        size_t processed = 0;
        
        if (fmt == "csv") {
            // CSV header
            out << "txid,address,amount,confirmations,category,time,label,is_coinbase\n";
            
            // CSV rows (batched)
            for (size_t batch_start = 0; batch_start < txs.size(); batch_start += batch_size) {
                size_t batch_end = std::min(batch_start + batch_size, txs.size());
                
                for (size_t i = batch_start; i < batch_end; ++i) {
                    const auto& tx = txs[i];
                    out << tx.txid << ","
                        << "\"" << tx.address << "\","
                        << tx.amount << ","
                        << tx.confirmations << ","
                        << tx.category << ","
                        << tx.time << ","
                        << "\"" << tx.label << "\","
                        << (tx.is_coinbase ? "true" : "false") << "\n";
                    
                    processed++;
                    
                    // Call progress callback
                    if (callback) {
                        double progress = static_cast<double>(processed) / static_cast<double>(total);
                        callback(tx.txid.c_str(), progress);
                    }
                }
                
                // Flush after each batch
                out.flush();
            }
        } else if (fmt == "json") {
            // JSON format (batched)
            out << "[\n";
            bool first = true;
            
            for (size_t batch_start = 0; batch_start < txs.size(); batch_start += batch_size) {
                size_t batch_end = std::min(batch_start + batch_size, txs.size());
                
                for (size_t i = batch_start; i < batch_end; ++i) {
                    const auto& tx = txs[i];
                    
                    if (!first) {
                        out << ",\n";
                    }
                    first = false;
                    
                    out << "  {\n"
                        << "    \"txid\": \"" << tx.txid << "\",\n"
                        << "    \"address\": \"" << tx.address << "\",\n"
                        << "    \"amount\": " << tx.amount << ",\n"
                        << "    \"confirmations\": " << tx.confirmations << ",\n"
                        << "    \"category\": \"" << tx.category << "\",\n"
                        << "    \"time\": " << tx.time << ",\n"
                        << "    \"label\": \"" << tx.label << "\",\n"
                        << "    \"is_coinbase\": " << (tx.is_coinbase ? "true" : "false") << "\n"
                        << "  }";
                    
                    processed++;
                    
                    // Call progress callback
                    if (callback) {
                        double progress = static_cast<double>(processed) / static_cast<double>(total);
                        callback(tx.txid.c_str(), progress);
                    }
                }
                
                // Flush after each batch
                out.flush();
            }
            
            out << "\n]";
        } else {
            out.close();
            g_last_error = DINERO_ERROR_INVALID_FORMAT;
            return -1;
        }
        
        out.close();
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_get_tx_confirmations(const char* txid, int32_t* confirmations_out) {
    if (!txid || !confirmations_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            return -1;
        }
        
        // Get transaction history and find matching txid
        auto txs = wm->getTransactionHistory(10000, 0);
        
        for (const auto& tx : txs) {
            if (tx.txid == txid) {
                *confirmations_out = tx.confirmations;
                g_last_error = DINERO_SUCCESS;
                return 0;
            }
        }
        
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_parse_uri(const char* uri, FFI_QRPayment* out) {
    if (!uri || !out) {
        return -1;
    }
    
    try {
        std::string s(uri);
        
        // Check if URI starts with "dinero:"
        if (s.find("dinero:") != 0) {
            return -1;
        }
        
        // Extract address (between "dinero:" and "?")
        size_t pos_q = s.find('?');
        std::string addr = s.substr(7, (pos_q == std::string::npos) ? std::string::npos : (pos_q - 7));
        
        // Copy address to output
        strncpy(out->address, addr.c_str(), 127);
        out->address[127] = '\0';
        
        // Initialize defaults
        out->amount = 0.0;
        out->label[0] = '\0';
        
        // Parse query parameters
        if (pos_q != std::string::npos) {
            std::string query = s.substr(pos_q + 1);
            std::istringstream ss(query);
            std::string kv;
            
            while (std::getline(ss, kv, '&')) {
                size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                
                std::string key = kv.substr(0, eq);
                std::string value = kv.substr(eq + 1);
                
                // URL decode basic (simple implementation)
                // Replace %20 with space, etc.
                std::string decoded_value;
                for (size_t i = 0; i < value.length(); ++i) {
                    if (value[i] == '%' && i + 2 < value.length()) {
                        char hex[3] = {value[i+1], value[i+2], '\0'};
                        char* end;
                        int code = static_cast<int>(std::strtol(hex, &end, 16));
                        if (*end == '\0') {
                            decoded_value += static_cast<char>(code);
                            i += 2;
                            continue;
                        }
                    }
                    decoded_value += value[i];
                }
                
                if (key == "amount") {
                    out->amount = std::atof(decoded_value.c_str());
                } else if (key == "label") {
                    strncpy(out->label, decoded_value.c_str(), 127);
                    out->label[127] = '\0';
                }
            }
        }
        
        return 0;
    } catch (...) {
        return -1;
    }
}

int dinero_wallet_generate_uri(
    const char* address,
    double amount,
    const char* label,
    char** uri_out
) {
    if (!address || !uri_out) {
        return -1;
    }
    
    try {
        std::ostringstream uri;
        uri << "dinero:" << address;
        
        bool has_params = false;
        
        // Add amount if specified
        if (amount > 0.0) {
            uri << "?amount=" << std::fixed << std::setprecision(8) << amount;
            has_params = true;
        }
        
        // Add label if specified
        if (label && strlen(label) > 0) {
            // URL encode the label
            std::string encoded_label;
            for (const char* p = label; *p; ++p) {
                if (*p == ' ') {
                    encoded_label += "%20";
                } else if (*p == '&') {
                    encoded_label += "%26";
                } else if (*p == '=') {
                    encoded_label += "%3D";
                } else if (*p == '?') {
                    encoded_label += "%3F";
                } else if (*p == '#') {
                    encoded_label += "%23";
                } else if (*p == '%') {
                    encoded_label += "%25";
                } else {
                    encoded_label += *p;
                }
            }
            
            if (has_params) {
                uri << "&label=" << encoded_label;
            } else {
                uri << "?label=" << encoded_label;
            }
        }
        
        std::string uri_str = uri.str();
        *uri_out = allocate_c_string(uri_str);
        
        return 0;
    } catch (...) {
        return -1;
    }
}

// Track last checked transaction timestamp for notifications
static int64_t g_last_check_timestamp = 0;

int dinero_wallet_check_new_transactions(
    FFI_TransactionNotification** notifications_out,
    int32_t* count_out
) {
    if (!notifications_out || !count_out) {
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            *notifications_out = nullptr;
            *count_out = 0;
            return -1;
        }
        
        // Get recent transactions (last 100)
        auto txs = wm->getTransactionHistory(100, 0);
        
        // Filter for new transactions (after last check timestamp)
        std::vector<FFI_TransactionNotification> new_txs;
        
        for (const auto& tx : txs) {
            // Check if this transaction is new (timestamp > last check)
            bool is_new = (tx.time > g_last_check_timestamp);
            
            if (is_new || g_last_check_timestamp == 0) {
                FFI_TransactionNotification notif;
                strncpy(notif.txid, tx.txid.c_str(), 64);
                notif.txid[64] = '\0';
                strncpy(notif.address, tx.address.c_str(), 127);
                notif.address[127] = '\0';
                notif.amount = tx.amount;
                notif.confirmations = tx.confirmations;
                notif.timestamp = tx.time;
                strncpy(notif.category, tx.category.c_str(), 31);
                notif.category[31] = '\0';
                notif.is_new = is_new;
                
                new_txs.push_back(notif);
            }
        }
        
        // Update last check timestamp to current time (or most recent tx time)
        if (!txs.empty()) {
            g_last_check_timestamp = txs[0].time; // Most recent transaction
        } else {
            g_last_check_timestamp = std::time(nullptr);
        }
        
        if (new_txs.empty()) {
            *notifications_out = nullptr;
            *count_out = 0;
            return 0;
        }
        
        // Allocate notification array
        FFI_TransactionNotification* notif_array = static_cast<FFI_TransactionNotification*>(
            malloc(sizeof(FFI_TransactionNotification) * new_txs.size())
        );
        
        if (!notif_array) {
            return -1;
        }
        
        // Copy notifications
        for (size_t i = 0; i < new_txs.size(); i++) {
            notif_array[i] = new_txs[i];
        }
        
        *notifications_out = notif_array;
        *count_out = static_cast<int32_t>(new_txs.size());
        
        return 0;
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// Memory Management
// ============================================================================

void dinero_wallet_free_string(char* ptr) {
    if (ptr) {
        free(ptr);
    }
}

void dinero_wallet_free_addresses(FFI_WalletAddress* ptr, int32_t count) {
    if (!ptr || count <= 0) {
        return;
    }
    
    for (int32_t i = 0; i < count; i++) {
        if (ptr[i].address) free(ptr[i].address);
        if (ptr[i].path) free(ptr[i].path);
    }
    
    free(ptr);
}

void dinero_wallet_free_utxos(FFI_WalletUTXO* ptr, int32_t count) {
    if (!ptr || count <= 0) {
        return;
    }
    
    for (int32_t i = 0; i < count; i++) {
        if (ptr[i].txid) free(ptr[i].txid);
        if (ptr[i].address) free(ptr[i].address);
    }
    
    free(ptr);
}

void dinero_wallet_free_notifications(FFI_TransactionNotification* ptr, int32_t count) {
    if (!ptr || count <= 0) {
        return;
    }
    
    // Notifications don't contain allocated strings (all fixed-size arrays)
    free(ptr);
}

// ============================================================================
// Performance & Diagnostics
// ============================================================================

int dinero_wallet_get_sync_progress(FFI_SyncProgress* progress_out) {
    if (!progress_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm) {
            g_last_error = DINERO_ERROR_WALLET_NOT_FOUND;
            progress_out->progress = 0.0;
            progress_out->current_block = 0;
            progress_out->total_blocks = 0;
            progress_out->is_syncing = false;
            progress_out->status_message = nullptr;
            return -1;
        }
        
        // Get blockchain height (if available)
        uint32_t current_height = 0;
        uint32_t tip_height = 0;
        
        // TODO: Integrate with blockchain sync state
        // For now, return placeholder values
        progress_out->progress = 1.0;  // Assume synced
        progress_out->current_block = static_cast<int32_t>(current_height);
        progress_out->total_blocks = static_cast<int32_t>(tip_height);
        progress_out->is_syncing = false;
        progress_out->status_message = allocate_c_string("Complete");
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

DineroErrorCode dinero_wallet_get_last_error(void) {
    return g_last_error;
}

int dinero_wallet_get_error_message(DineroErrorCode error_code, char** message_out) {
    if (!message_out) {
        return -1;
    }
    
    std::string message;
    switch (error_code) {
        case DINERO_SUCCESS:
            message = "Success";
            break;
        case DINERO_ERROR_GENERIC:
            message = "Generic error";
            break;
        case DINERO_ERROR_WALLET_NOT_FOUND:
            message = "Wallet not found";
            break;
        case DINERO_ERROR_WALLET_LOCKED:
            message = "Wallet is locked";
            break;
        case DINERO_ERROR_WALLET_ENCRYPTED:
            message = "Wallet is encrypted";
            break;
        case DINERO_ERROR_INVALID_MNEMONIC:
            message = "Invalid mnemonic phrase";
            break;
        case DINERO_ERROR_INVALID_ADDRESS:
            message = "Invalid address";
            break;
        case DINERO_ERROR_INSUFFICIENT_FUNDS:
            message = "Insufficient funds";
            break;
        case DINERO_ERROR_INVALID_AMOUNT:
            message = "Invalid amount";
            break;
        case DINERO_ERROR_TX_BROADCAST_FAILED:
            message = "Transaction broadcast failed";
            break;
        case DINERO_ERROR_FILE_IO:
            message = "File I/O error";
            break;
        case DINERO_ERROR_INVALID_FORMAT:
            message = "Invalid format";
            break;
        case DINERO_ERROR_NETWORK:
            message = "Network error";
            break;
        case DINERO_ERROR_AUTHENTICATION:
            message = "Authentication failed";
            break;
        case DINERO_ERROR_NOT_IMPLEMENTED:
            message = "Feature not implemented";
            break;
        default:
            message = "Unknown error";
            break;
    }
    
    *message_out = allocate_c_string(message);
    return 0;
}

// ============================================================================
// Security & Key Management
// ============================================================================

int dinero_wallet_store_secure(const char* wallet_data, size_t data_length) {
    if (!wallet_data || data_length == 0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    // Platform-specific implementation:
    // - macOS/iOS: Use Keychain Services
    // - Android: Use Android Keystore
    // - Linux/Windows: Use platform-specific secure storage
    
    // TODO: Implement platform-specific secure storage
    // For now, return not implemented
    g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
    return -1;
}

int dinero_wallet_retrieve_secure(char** wallet_data_out, size_t* data_length_out) {
    if (!wallet_data_out || !data_length_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    // TODO: Implement platform-specific secure storage retrieval
    g_last_error = DINERO_ERROR_NOT_IMPLEMENTED;
    return -1;
}

bool dinero_wallet_secure_storage_available(void) {
    // Check if platform secure storage is available
    // TODO: Implement platform-specific checks
    return false;  // Not implemented yet
}

// ============================================================================
// Exchange & Swap Features
// ============================================================================

// Exchange rate provider URLs (can be configured)
static const char* EXCHANGE_RATE_API = "https://api.coingecko.com/api/v3/simple/price";
static const char* SIMPLESWAP_API = "https://api.simpleswap.io/v1/get_rate";

int dinero_wallet_get_exchange_rate(
    const char* from,
    const char* to,
    double amount,
    FFI_ExchangeRate* rate_out
) {
    if (!from || !to || !rate_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // Initialize output structure
        memset(rate_out, 0, sizeof(FFI_ExchangeRate));
        
        strncpy(rate_out->from_symbol, from, 7);
        rate_out->from_symbol[7] = '\0';
        strncpy(rate_out->to_symbol, to, 7);
        rate_out->to_symbol[7] = '\0';
        
        // TODO: Implement actual API call to exchange provider
        // For now, return mock rates for testing
        // In production, this would:
        // 1. Call CoinGecko API for DIN rate
        // 2. Call SimpleSwap API for swap rates
        // 3. Parse JSON response
        
        // Mock rate calculation (replace with real API call)
        if (strcmp(from, "DIN") == 0 && strcmp(to, "BTC") == 0) {
            rate_out->rate = 0.0001;  // 1 DIN = 0.0001 BTC (mock)
            rate_out->to_amount = amount * rate_out->rate;
        } else if (strcmp(from, "DIN") == 0 && strcmp(to, "USD") == 0) {
            rate_out->rate = 0.01;  // 1 DIN = $0.01 (mock)
            rate_out->to_amount = amount * rate_out->rate;
        } else if (strcmp(from, "DIN") == 0 && strcmp(to, "ETH") == 0) {
            rate_out->rate = 0.00005;  // 1 DIN = 0.00005 ETH (mock)
            rate_out->to_amount = amount * rate_out->rate;
        } else {
            // Default: 1:1 for same currency or unknown
            rate_out->rate = 1.0;
            rate_out->to_amount = amount;
        }
        
        rate_out->min_amount = 0.001;
        rate_out->max_amount = 1000000.0;
        rate_out->timestamp = std::time(nullptr);
        strncpy(rate_out->provider, "CoinGecko", 31);
        rate_out->provider[31] = '\0';
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_NETWORK;
        return -1;
    }
}

int dinero_wallet_create_swap_tx(
    const char* from_address,
    const char* to_address,
    double amount,
    const char* from_symbol,
    const char* to_symbol,
    FFI_SwapTransaction* swap_out
) {
    if (!from_address || !to_address || !from_symbol || !to_symbol || !swap_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            g_last_error = DINERO_ERROR_WALLET_LOCKED;
            return -1;
        }
        
        // Initialize swap structure
        memset(swap_out, 0, sizeof(FFI_SwapTransaction));
        
        strncpy(swap_out->from_address, from_address, 127);
        swap_out->from_address[127] = '\0';
        strncpy(swap_out->to_address, to_address, 127);
        swap_out->to_address[127] = '\0';
        strncpy(swap_out->from_symbol, from_symbol, 7);
        swap_out->from_symbol[7] = '\0';
        strncpy(swap_out->to_symbol, to_symbol, 7);
        swap_out->to_symbol[7] = '\0';
        
        swap_out->from_amount = amount;
        
        // Get exchange rate to calculate to_amount
        FFI_ExchangeRate rate;
        int rate_result = dinero_wallet_get_exchange_rate(from_symbol, to_symbol, amount, &rate);
        if (rate_result == 0) {
            swap_out->to_amount = rate.to_amount;
            swap_out->fee = amount * 0.01;  // 1% fee (mock)
        } else {
            g_last_error = DINERO_ERROR_NETWORK;
            return -1;
        }
        
        // TODO: Create actual swap transaction
        // This would involve:
        // 1. Creating a transaction to exchange provider's address
        // 2. Signing the transaction
        // 3. Broadcasting to network
        // 4. Getting swap ID from provider
        
        // For now, generate mock transaction ID
        std::string mock_txid = "swap_" + std::to_string(std::time(nullptr));
        strncpy(swap_out->txid, mock_txid.c_str(), 64);
        swap_out->txid[64] = '\0';
        
        strncpy(swap_out->status, "pending", 31);
        swap_out->status[31] = '\0';
        swap_out->timestamp = std::time(nullptr);
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_get_swap_status(
    const char* swap_id,
    FFI_SwapTransaction* swap_out
) {
    if (!swap_id || !swap_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // TODO: Query exchange provider API for swap status
        // This would call the provider's API with swap_id
        
        // For now, return mock status
        memset(swap_out, 0, sizeof(FFI_SwapTransaction));
        strncpy(swap_out->txid, swap_id, 64);
        swap_out->txid[64] = '\0';
        strncpy(swap_out->status, "processing", 31);
        swap_out->status[31] = '\0';
        swap_out->timestamp = std::time(nullptr);
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_NETWORK;
        return -1;
    }
}

// ============================================================================
// Liquidity & On-Ramp Features
// ============================================================================

int dinero_wallet_get_liquidity_pools(
    FFI_LiquidityPool** pools_out,
    int32_t* count_out
) {
    if (!pools_out || !count_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // TODO: Query liquidity pool provider API
        // For now, return mock pools
        
        const int mock_pool_count = 3;
        FFI_LiquidityPool* pools = static_cast<FFI_LiquidityPool*>(
            malloc(sizeof(FFI_LiquidityPool) * mock_pool_count)
        );
        
        if (!pools) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }
        
        // Mock DIN pool
        memset(&pools[0], 0, sizeof(FFI_LiquidityPool));
        strncpy(pools[0].pool_id, "din_pool_1", 63);
        pools[0].pool_id[63] = '\0';
        strncpy(pools[0].symbol, "DIN", 7);
        pools[0].symbol[7] = '\0';
        pools[0].total_liquidity = 1000000.0;
        pools[0].available_liquidity = 500000.0;
        pools[0].apy = 5.5;
        pools[0].min_deposit = 10.0;
        pools[0].max_deposit = 100000.0;
        pools[0].last_update = std::time(nullptr);
        
        // Mock BTC pool
        memset(&pools[1], 0, sizeof(FFI_LiquidityPool));
        strncpy(pools[1].pool_id, "btc_pool_1", 63);
        pools[1].pool_id[63] = '\0';
        strncpy(pools[1].symbol, "BTC", 7);
        pools[1].symbol[7] = '\0';
        pools[1].total_liquidity = 100.0;
        pools[1].available_liquidity = 50.0;
        pools[1].apy = 3.2;
        pools[1].min_deposit = 0.001;
        pools[1].max_deposit = 10.0;
        pools[1].last_update = std::time(nullptr);
        
        // Mock ETH pool
        memset(&pools[2], 0, sizeof(FFI_LiquidityPool));
        strncpy(pools[2].pool_id, "eth_pool_1", 63);
        pools[2].pool_id[63] = '\0';
        strncpy(pools[2].symbol, "ETH", 7);
        pools[2].symbol[7] = '\0';
        pools[2].total_liquidity = 1000.0;
        pools[2].available_liquidity = 500.0;
        pools[2].apy = 4.1;
        pools[2].min_deposit = 0.01;
        pools[2].max_deposit = 100.0;
        pools[2].last_update = std::time(nullptr);
        
        *pools_out = pools;
        *count_out = mock_pool_count;
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_add_liquidity(
    const char* pool_id,
    double amount,
    char** txid_out
) {
    if (!pool_id || !txid_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            g_last_error = DINERO_ERROR_WALLET_LOCKED;
            return -1;
        }
        
        // TODO: Create transaction to add liquidity to pool
        // This would involve:
        // 1. Creating transaction to pool contract/address
        // 2. Signing the transaction
        // 3. Broadcasting to network
        
        // For now, generate mock transaction ID
        std::string mock_txid = "liq_dep_" + std::to_string(std::time(nullptr));
        *txid_out = allocate_c_string(mock_txid);
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_remove_liquidity(
    const char* pool_id,
    double amount,
    char** txid_out
) {
    if (!pool_id || !txid_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    std::lock_guard<WalletMutex> lock(g_wallet_mutex);
    
    try {
        dinero::WalletManager* wm = get_wallet_manager_nolock();
        if (!wm || !ensure_active_wallet_nolock(wm, false) || wm->isWalletLocked()) {
            g_last_error = DINERO_ERROR_WALLET_LOCKED;
            return -1;
        }
        
        // TODO: Create transaction to remove liquidity from pool
        // Similar to add_liquidity but withdrawal
        
        std::string mock_txid = "liq_wdraw_" + std::to_string(std::time(nullptr));
        *txid_out = allocate_c_string(mock_txid);
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_create_fiat_order(
    double amount,
    const char* fiat_currency,
    const char* crypto_symbol,
    const char* payment_method,
    FFI_FiatOrder* order_out
) {
    if (!fiat_currency || !crypto_symbol || !payment_method || !order_out || amount <= 0.0) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // Initialize order structure
        memset(order_out, 0, sizeof(FFI_FiatOrder));
        
        // Generate order ID
        std::string order_id = "fiat_" + std::to_string(std::time(nullptr));
        strncpy(order_out->order_id, order_id.c_str(), 63);
        order_out->order_id[63] = '\0';
        
        strncpy(order_out->payment_method, payment_method, 31);
        order_out->payment_method[31] = '\0';
        strncpy(order_out->fiat_currency, fiat_currency, 7);
        order_out->fiat_currency[7] = '\0';
        strncpy(order_out->crypto_symbol, crypto_symbol, 7);
        order_out->crypto_symbol[7] = '\0';
        
        order_out->fiat_amount = amount;
        
        // Get exchange rate to calculate crypto amount
        FFI_ExchangeRate rate;
        int rate_result = dinero_wallet_get_exchange_rate(crypto_symbol, fiat_currency, amount, &rate);
        if (rate_result == 0) {
            order_out->crypto_amount = amount / rate.rate;  // Convert fiat to crypto
            order_out->exchange_rate = rate.rate;
        } else {
            // Default rate if exchange API unavailable
            order_out->crypto_amount = amount * 100.0;  // Mock: 1 USD = 100 DIN
            order_out->exchange_rate = 0.01;
        }
        
        order_out->fee = amount * 0.025;  // 2.5% fee (typical for fiat on-ramps)
        strncpy(order_out->status, "pending", 31);
        order_out->status[31] = '\0';
        
        // Generate payment URL (mock - would come from payment provider)
        std::string payment_url = "https://payment-provider.com/pay/" + order_id;
        strncpy(order_out->payment_url, payment_url.c_str(), 255);
        order_out->payment_url[255] = '\0';
        
        order_out->created_at = std::time(nullptr);
        order_out->expires_at = order_out->created_at + (15 * 60);  // 15 minutes expiry
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_get_fiat_order_status(
    const char* order_id,
    FFI_FiatOrder* order_out
) {
    if (!order_id || !order_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // TODO: Query payment provider API for order status
        // This would call the provider's API with order_id
        
        // For now, return mock status
        memset(order_out, 0, sizeof(FFI_FiatOrder));
        strncpy(order_out->order_id, order_id, 63);
        order_out->order_id[63] = '\0';
        strncpy(order_out->status, "processing", 31);
        order_out->status[31] = '\0';
        order_out->created_at = std::time(nullptr) - 300;  // 5 minutes ago
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_NETWORK;
        return -1;
    }
}

int dinero_wallet_get_kyc_status(FFI_KYCStatus* status_out) {
    if (!status_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // Get active KYC provider
        KYCProvider* provider = KYCProviderRegistry::GetProvider();
        
        if (!provider) {
            // No provider configured - return default status
            memset(status_out, 0, sizeof(FFI_KYCStatus));
            status_out->is_verified = false;
            strncpy(status_out->verification_level, "none", 31);
            status_out->verification_level[31] = '\0';
            strncpy(status_out->provider, "None", 31);
            status_out->provider[31] = '\0';
            status_out->verified_at = 0;
            status_out->expires_at = 0;
            strncpy(status_out->country, "US", 2);
            status_out->country[2] = '\0';
            
            g_last_error = DINERO_SUCCESS;
            return 0;
        }
        
        // Get user ID from wallet (if available)
        std::string user_id = "default_user"; // TODO: Get from wallet context
        
        // Query provider for status
        FFI_KYCStatus status;
        int result = provider->GetStatus(user_id, "", status);
        
        if (result == 0) {
            *status_out = status;
            g_last_error = DINERO_SUCCESS;
            return 0;
        } else {
            // Error querying provider
            g_last_error = DINERO_ERROR_NETWORK;
            return -1;
        }
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

int dinero_wallet_start_kyc_verification(
    const char* level,
    const char* country,
    char** verification_url_out
) {
    if (!level || !country || !verification_url_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // Get active KYC provider
        KYCProvider* provider = KYCProviderRegistry::GetProvider();
        
        if (!provider || !provider->IsAvailable()) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }
        
        // Get user ID from wallet (if available)
        std::string user_id = "default_user"; // TODO: Get from wallet context
        
        // Start verification via provider
        std::string verification_url;
        int result = provider->StartVerification(
            user_id,
            std::string(level),
            std::string(country),
            verification_url
        );
        
        if (result == 0) {
            *verification_url_out = allocate_c_string(verification_url);
            g_last_error = DINERO_SUCCESS;
            return 0;
        } else {
            g_last_error = DINERO_ERROR_NETWORK;
            return -1;
        }
    } catch (...) {
        g_last_error = DINERO_ERROR_NETWORK;
        return -1;
    }
}

// ============================================================================
// KYC Provider Configuration
// ============================================================================

/**
 * Initialize KYC provider
 * @param provider_type Provider type ("mock", "openkyc", "sumsub", "onfido")
 * @param config Configuration string (JSON or key=value format)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_init_kyc_provider(
    const char* provider_type,
    const char* config
) {
    if (!provider_type) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        // Create provider
        auto provider = KYCProviderFactory::CreateFromString(provider_type);
        
        if (!provider) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }
        
        // Initialize with config
        std::string config_str = config ? config : "";
        if (!provider->Initialize(config_str)) {
            g_last_error = DINERO_ERROR_GENERIC;
            return -1;
        }
        
        // Set as active provider
        KYCProviderRegistry::SetProvider(std::move(provider));
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

/**
 * Get current KYC provider name
 * @param name_out Output provider name (caller must free with dinero_wallet_free_string)
 * @return 0 on success, non-zero on error
 */
int dinero_wallet_get_kyc_provider_name(char** name_out) {
    if (!name_out) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
    
    try {
        KYCProvider* provider = KYCProviderRegistry::GetProvider();
        
        if (!provider) {
            *name_out = allocate_c_string("None");
        } else {
            *name_out = allocate_c_string(provider->GetName());
        }
        
        g_last_error = DINERO_SUCCESS;
        return 0;
    } catch (...) {
        g_last_error = DINERO_ERROR_GENERIC;
        return -1;
    }
}

void dinero_wallet_free_pools(FFI_LiquidityPool* ptr, int32_t count) {
    if (!ptr || count <= 0) {
        return;
    }
    
    // Pools don't contain allocated strings (all fixed-size arrays)
    free(ptr);
}
