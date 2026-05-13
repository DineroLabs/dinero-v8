#pragma once

#include "daemon/tx_mempool.h"
#include <string>
#include <memory>
#include <vector>

// Forward declaration
struct sqlite3;

namespace dinero {

/**
 * Database-backed UTXO view implementation
 * Queries the blockchain database (chainstate) for UTXO information
 * This is the single source of truth for UTXO validation
 */
class DatabaseUTXOView : public UTXOView {
public:
    explicit DatabaseUTXOView(const std::string& db_path);
    ~DatabaseUTXOView();
    
    // Initialize the database connection
    bool Initialize();
    
    // UTXOView interface implementation
    bool HaveUTXO(const std::string& txid, uint32_t vout) const override;
    bool GetUTXO(const std::string& txid, uint32_t vout, 
                 uint64_t& value, std::string& script) const override;
    bool HaveTransaction(const std::string& txid) const override;
    uint32_t GetHeight() const override;
    
    // Helper methods moved to common/hex_utils.h
    
private:
    std::string db_path_;
    sqlite3* db_;
    
    // Ensure UTXO table exists with correct schema
    bool EnsureUTXOTable();
};

} // namespace dinero
