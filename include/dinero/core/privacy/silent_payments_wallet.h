#pragma once
#include "privacy/silent_payments.h"
#include "privacy/silent_scanner_manager.h"
#include "dinero/core/wallet/sqlite_wallet.h"
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <cstdint>

namespace din::sp {

class SilentPaymentsWallet {
public:
    explicit SilentPaymentsWallet(std::shared_ptr<Dinero::Wallet::SQLiteWallet> wallet);
    ~SilentPaymentsWallet();
    
    // Silent Payment address management
    std::string generateSilentPaymentAddress(const std::string& wallet_id = "default");
    bool addSilentPaymentAddress(const std::string& wallet_id, 
                                const std::array<uint8_t,33>& scan_pub,
                                const std::array<uint8_t,33>& spend_pub);
    
    // Key management
    struct SilentPaymentKeys {
        std::array<uint8_t,32> scan_priv;
        std::array<uint8_t,32> spend_priv;
        std::array<uint8_t,33> scan_pub;
        std::array<uint8_t,33> spend_pub;
        std::string address;
        uint64_t created_at;
    };
    
    SilentPaymentKeys generateKeys(const std::string& wallet_id = "default");
    std::vector<SilentPaymentKeys> getKeys(const std::string& wallet_id = "");
    bool storeKeys(const std::string& wallet_id, const SilentPaymentKeys& keys);
    
    // Scanner management
    void registerScanner(const std::string& wallet_id, 
                        const std::array<uint8_t,32>& scan_priv,
                        const std::array<uint8_t,33>& spend_pub);
    void unregisterScanner(const std::string& wallet_id);
    
    // Transaction creation
    struct SilentPaymentTx {
        std::string txid;
        std::string hex;
        std::vector<std::string> input_txids;
        std::vector<uint32_t> input_vouts;
        std::string recipient_address;
        uint64_t amount;
        uint64_t fee;
    };
    
    SilentPaymentTx createSilentPayment(const std::string& recipient_address,
                                       uint64_t amount,
                                       const std::vector<std::string>& input_txids,
                                       const std::vector<uint32_t>& input_vouts,
                                       uint64_t fee_rate = 5);
    
    // Scan results
    std::vector<ScannerManager::ScanResult> getScanResults(const std::string& wallet_id = "");
    void clearScanResults(const std::string& wallet_id = "");
    
    // Statistics
    ScannerManager::ScanStats getScanStats() const;

private:
    std::shared_ptr<Dinero::Wallet::SQLiteWallet> wallet_;
    std::unique_ptr<ScannerManager> scanner_manager_;
    
    // Helper methods
    std::array<uint8_t,32> deriveScanPrivateKey(const std::string& wallet_id, uint32_t index);
    std::array<uint8_t,32> deriveSpendPrivateKey(const std::string& wallet_id, uint32_t index);
    std::array<uint8_t,33> derivePublicKey(const std::array<uint8_t,32>& priv_key);
    std::string createSilentPaymentAddress(const std::array<uint8_t,33>& scan_pub,
                                          const std::array<uint8_t,33>& spend_pub);
    
    // Database operations
    bool storeSilentPaymentKeys(const std::string& wallet_id, const SilentPaymentKeys& keys);
    std::vector<SilentPaymentKeys> loadSilentPaymentKeys(const std::string& wallet_id = "");
    bool createSilentPaymentsTable();
    
    // Transaction helpers
    std::vector<uint8_t> serializeTransaction(const SilentPaymentTx& tx);
    std::string calculateTxId(const std::vector<uint8_t>& tx_bytes);
};

} // namespace din::sp
