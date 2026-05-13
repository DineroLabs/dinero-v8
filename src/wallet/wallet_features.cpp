#include "wallet/wallet_features.h"

namespace dinero {
namespace wallet {

// Phase 33: Full wallet activation
// All features enabled for complete send/receive functionality
WalletFeatures g_wallet_features = {
    .wallet_enabled = true,           // Enable wallet subsystem
    .read_only_mode = false,          // Full read-write mode
    .descriptor_creation = true,      // Allow descriptor operations
    .address_generation = true,       // Allow address generation
    .utxo_tracking = true,           // Track wallet UTXOs
    .transaction_creation = true,     // Create transactions
    .transaction_signing = true,      // Sign transactions with wallet keys
    .transaction_broadcast = true     // Broadcast to network
};

} // namespace wallet
} // namespace dinero
