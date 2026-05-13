#pragma once
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include "primitives/uint256.h"

namespace din {

struct Address {
    std::string bech32;
    uint32_t index;
    bool is_change;

    Address() : index(0), is_change(false) {}
    Address(const std::string& addr, uint32_t idx, bool change)
        : bech32(addr), index(idx), is_change(change) {}
};

struct WalletUTXO {
    dinero::uint256 txid;  // Phase M.0: uint256 identity
    uint32_t vout;
    int64_t value;
    bool spendable;
    bool locked;

    WalletUTXO() : vout(0), value(0), spendable(false), locked(false) {}  // Phase M.3: Fixed constructor name
    WalletUTXO(const dinero::uint256& tx, uint32_t v, int64_t val, bool spend, bool lock)
        : txid(tx), vout(v), value(val), spendable(spend), locked(lock) {}
};

struct FundResult { 
    std::string psbt_base64; 
    int64_t fee; 
    int64_t change; 
    
    FundResult() : fee(0), change(0) {}
    FundResult(const std::string& psbt, int64_t f, int64_t c)
        : psbt_base64(psbt), fee(f), change(c) {}
};

/**
 * @brief Chain view interface for wallet blockchain queries
 * 
 * Provides wallet with read-only access to blockchain state
 * without tight coupling to full blockchain implementation.
 */
struct IChainView {
    virtual ~IChainView() = default;
    virtual int getHeight() const = 0;
    virtual bool isTxConfirmed(const std::string& txid, int* confs) const = 0;
    virtual std::optional<std::string> getBlockHash(int height) const = 0;
    virtual int64_t getMedianTimePast() const = 0;
};

/**
 * @brief Fee estimation interface for transaction fee calculation
 * 
 * Provides fee rate estimates based on confirmation targets
 * without coupling to mempool or policy engine internals.
 */
struct IFeeEstimator {
    virtual ~IFeeEstimator() = default;
    virtual std::optional<double> estimate(int target_blocks) const = 0; // sat/vB
    virtual double getMinRelayFee() const = 0; // sat/vB
    virtual double getFallbackFee() const = 0; // sat/vB when estimation unavailable
};

/**
 * @brief Wallet database interface for persistent storage
 * 
 * Abstracts wallet storage backend (RocksDB CF, SQLite, etc.)
 * for keys, addresses, UTXOs, and transaction metadata.
 */
struct IWalletDB {
    virtual ~IWalletDB() = default;
    
    // Basic operations
    virtual bool put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual bool remove(const std::string& key) = 0;
    
    // Batch operations for atomicity
    virtual bool beginBatch() = 0;
    virtual bool commitBatch() = 0;
    virtual bool abortBatch() = 0;
    
    // Iteration for backup/recovery
    virtual bool iterate(const std::string& prefix, 
                        std::function<bool(const std::string&, const std::string&)> callback) const = 0;
};

/**
 * @brief Key store interface for cryptographic operations
 * 
 * Handles key derivation, storage, and signing operations
 * with proper security and hardware wallet support.
 */
struct IKeyStore {
    virtual ~IKeyStore() = default;
    
    // Key derivation
    virtual std::optional<std::string> getXPub(const std::string& path) const = 0;
    virtual std::optional<std::string> getXPriv(const std::string& path) const = 0; // encrypted wallets may refuse
    
    // Signing operations  
    virtual std::optional<std::vector<uint8_t>> sign(const std::vector<uint8_t>& hash, 
                                                    const std::string& key_path) = 0;
    virtual bool canSign(const std::string& key_path) const = 0;
    
    // Key management
    virtual bool hasKey(const std::string& key_path) const = 0;
    virtual std::vector<std::string> listKeyPaths() const = 0;
};

/**
 * @brief Main wallet interface for all wallet operations
 * 
 * Clean interface for RPC layer without heavy dependencies.
 * Implemented by DescriptorWallet with proper component integration.
 */
struct IWallet {
    virtual ~IWallet() = default;
    
    // Address management
    virtual Address getNewAddress(const std::string& label = "") = 0;
    virtual Address getNewChangeAddress() = 0;
    virtual bool validateAddress(const std::string& address) const = 0;
    virtual std::vector<std::string> listAddresses() const = 0;
    
    // UTXO management
    virtual std::vector<WalletUTXO> listUnspent(int min_conf = 1, int max_conf = 999999) const = 0;  // Phase M.3: WalletUTXO
    virtual int64_t getBalance(bool include_unconfirmed = false) const = 0;
    virtual bool lockUTXO(const std::string& txid, uint32_t vout) = 0;
    virtual bool unlockUTXO(const std::string& txid, uint32_t vout) = 0;

    // Transaction creation
    virtual FundResult createFundedPsbt(const std::vector<WalletUTXO>& inputs,  // Phase M.3: WalletUTXO
                                       const std::vector<std::pair<std::string,int64_t>>& outputs,
                                       int target_blocks) = 0;
    virtual std::string finalizePsbt(const std::string& psbt_b64) = 0; // returns hex
    virtual std::string sendToAddress(const std::string& address, int64_t amount, 
                                     int target_blocks = 6) = 0; // returns txid
    
    // Wallet state
    virtual bool isEncrypted() const = 0;
    virtual bool isLocked() const = 0;
    virtual bool unlock(const std::string& passphrase, int timeout_seconds = 0) = 0;
    virtual bool lock() = 0;
    virtual bool changePassphrase(const std::string& old_pass, const std::string& new_pass) = 0;
};

/**
 * @brief Execution context for wallet operations
 * 
 * Provides wallet with access to required services without global dependencies.
 * Passed to wallet methods instead of using global variables.
 */
struct WalletExecutionContext {
    IChainView* chain = nullptr;
    IFeeEstimator* fee_estimator = nullptr;
    IWalletDB* wallet_db = nullptr;
    IKeyStore* key_store = nullptr;
    
    // Validation
    bool isValid() const {
        return chain && fee_estimator && wallet_db && key_store;
    }
};

} // namespace din
