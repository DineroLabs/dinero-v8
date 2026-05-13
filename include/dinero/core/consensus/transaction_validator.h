#pragma once

#include "din_json.h"
#include <string>
#include <vector>
#include <memory>

namespace dinero {
namespace consensus {

// Transaction validation result
struct ValidationResult {
    bool valid = false;
    std::string error_message;
    int error_code = 0;
    
    // Additional validation info
    double fee_rate = 0.0;
    uint64_t total_input_value = 0;
    uint64_t total_output_value = 0;
    uint64_t calculated_fee = 0;
    bool has_witness_data = false;
    uint32_t sigops_count = 0;
};

// Transaction input for validation
struct TxInput {
    std::string prev_txid;
    uint32_t prev_vout;
    std::string script_sig;
    uint32_t sequence;
    std::vector<std::string> witness;  // For segwit
};

// Transaction output for validation
struct TxOutput {
    uint64_t value;
    std::string script_pubkey;
};

// Complete transaction for validation
struct ValidatedTransaction {
    uint32_t version;
    std::vector<TxInput> vin;
    std::vector<TxOutput> vout;
    uint32_t lockTime;
    std::string txid;
    uint32_t size;
    uint32_t weight;
    uint64_t fee;
};

// UTXO information for input validation
struct WalletUTXO {
    std::string txid;
    uint32_t vout;
    uint64_t value;
    std::string script_pubkey;
    uint32_t height;
    bool is_coinbase;
    bool is_spent;
};

// Forward declaration
class UTXOProvider;

/**
 * Comprehensive transaction validator
 * Validates transactions according to consensus rules and policy
 */
class TransactionValidator {
public:
    TransactionValidator();
    ~TransactionValidator() = default;
    
    // Set UTXO provider for input validation
    void setUTXOProvider(std::shared_ptr<UTXOProvider> provider);
    
    // Main validation methods
    ValidationResult validateTransaction(const ValidatedTransaction& tx) const;
    ValidationResult validateRawTransaction(const std::string& hex_tx) const;
    ValidationResult validateTransactionInputs(const ValidatedTransaction& tx) const;
    
    // Individual validation checks
    bool validateFormat(const ValidatedTransaction& tx, std::string& error) const;
    bool validateSize(const ValidatedTransaction& tx, std::string& error) const;
    bool validateFee(const ValidatedTransaction& tx, std::string& error) const;
    bool validateInputs(const ValidatedTransaction& tx, std::string& error) const;
    bool validateOutputs(const ValidatedTransaction& tx, std::string& error) const;
    bool validateScripts(const ValidatedTransaction& tx, std::string& error) const;
    bool validateTimelock(const ValidatedTransaction& tx, std::string& error) const;
    
    // Policy validation
    bool validatePolicy(const ValidatedTransaction& tx, std::string& error) const;
    bool checkDustOutputs(const ValidatedTransaction& tx, std::string& error) const;
    bool checkRBF(const ValidatedTransaction& tx, std::string& error) const;
    
    // Utility methods
    static ValidatedTransaction parseRawTransaction(const std::string& hex_tx);
    static std::string calculateTxId(const ValidatedTransaction& tx);
    static uint32_t calculateTxSize(const ValidatedTransaction& tx);
    static uint32_t calculateTxWeight(const ValidatedTransaction& tx);
    static uint32_t countSigOps(const ValidatedTransaction& tx);
    
    // Configuration
    void setMinRelayFee(uint64_t fee_rate) { min_relay_fee_ = fee_rate; }
    void setDustThreshold(uint64_t threshold) { dust_threshold_ = threshold; }
    void setMaxTxSize(uint32_t size) { max_tx_size_ = size; }
    void setMaxSigOps(uint32_t sigops) { max_sigops_ = sigops; }
    
private:
    std::shared_ptr<UTXOProvider> utxo_provider_;
    
    // Policy parameters
    uint64_t min_relay_fee_ = 1000;  // 1000 sat/kB
    uint64_t dust_threshold_ = 546;  // 546 una
    uint32_t max_tx_size_ = 100000;  // 100KB
    uint32_t max_sigops_ = 20000;    // Max signature operations
    uint32_t max_tx_weight_ = 400000; // 400K weight units
    
    // Helper methods
    bool isValidScriptPubKey(const std::string& script) const;
    bool isValidScriptSig(const std::string& script) const;
    uint64_t calculateMinimumFee(uint32_t tx_size) const;
};

/**
 * UTXO provider interface for input validation
 */
class UTXOProvider {
public:
    virtual ~UTXOProvider() = default;
    
    // Get UTXO information
    virtual bool getUTXO(const std::string& txid, uint32_t vout, UTXO& utxo) const = 0;
    virtual bool isUTXOSpent(const std::string& txid, uint32_t vout) const = 0;
    virtual std::vector<UTXO> getUTXOsForAddress(const std::string& address) const = 0;
    
    // Check if transaction exists
    virtual bool hasTransaction(const std::string& txid) const = 0;
    
    // Get current blockchain height
    virtual uint32_t getCurrentHeight() const = 0;
};

/**
 * Database-backed UTXO provider
 */
class DatabaseUTXOProvider : public UTXOProvider {
public:
    explicit DatabaseUTXOProvider(class ChainDB* chain_db);
    ~DatabaseUTXOProvider() override = default;
    
    bool getUTXO(const std::string& txid, uint32_t vout, UTXO& utxo) const override;
    bool isUTXOSpent(const std::string& txid, uint32_t vout) const override;
    std::vector<UTXO> getUTXOsForAddress(const std::string& address) const override;
    bool hasTransaction(const std::string& txid) const override;
    uint32_t getCurrentHeight() const override;
    
private:
    class ChainDB* chain_db_;  // Week 5: ChainDB for UTXO lookups (no globals)
};

} // namespace consensus
} // namespace dinero
