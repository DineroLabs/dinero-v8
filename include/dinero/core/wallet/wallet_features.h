#pragma once

namespace dinero {
namespace wallet {

/**
 * Wallet feature flags for gradual rollout
 */
struct WalletFeatures {
    bool wallet_enabled = false;           // Master wallet enable flag
    bool read_only_mode = true;           // Only allow read operations
    bool descriptor_creation = true;      // Allow creating new descriptors
    bool address_generation = true;       // Allow generating addresses
    bool utxo_tracking = false;          // Track UTXOs (requires storage)
    bool transaction_creation = false;    // Create transactions (requires policy)
    bool transaction_signing = false;     // Sign transactions (requires full UTXO set)
    bool transaction_broadcast = false;   // Broadcast transactions (requires P2P)
    
    /**
     * Check if a specific wallet operation is allowed
     */
    bool isOperationAllowed(const std::string& operation) const {
        if (!wallet_enabled) return false;
        
        if (operation == "getnewaddress" || operation == "listdescriptors" || 
            operation == "deriveaddresses") {
            return descriptor_creation && address_generation;
        }
        
        if (operation == "listunspent" || operation == "getbalance") {
            return utxo_tracking;
        }
        
        if (operation == "createrawtransaction" || operation == "fundrawtransaction") {
            return transaction_creation && utxo_tracking;
        }
        
        if (operation == "signrawtransactionwithwallet") {
            return transaction_signing && transaction_creation;
        }
        
        if (operation == "sendrawtransaction") {
            return transaction_broadcast && transaction_signing;
        }
        
        return false;
    }
    
    /**
     * Get error message for disabled operation
     */
    std::string getDisabledMessage(const std::string& operation) const {
        if (!wallet_enabled) {
            return "Wallet functionality disabled";
        }
        
        if (read_only_mode && (operation.find("send") != std::string::npos || 
                              operation.find("sign") != std::string::npos)) {
            return "Wallet in read-only mode - signing/broadcasting disabled";
        }
        
        return "Operation '" + operation + "' not available in current configuration";
    }
};

/**
 * Global wallet features instance
 */
extern WalletFeatures g_wallet_features;

} // namespace wallet
} // namespace dinero
