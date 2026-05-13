#include "daemon/wallet.h"
#include "wallet/address.h"
#include "wallet/taproot_keys.h"
#include "consensus/chainparams.h"
#include "common/address_script_builder.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>

namespace dinero {

DaemonWallet::DaemonWallet() {
    g_logger.info("Initializing wallet component");
}

DaemonWallet::~DaemonWallet() {
    g_logger.info("Shutting down wallet component");
}

bool DaemonWallet::initialize() {
    g_logger.info("Wallet initialization started");
    
    // Initialize watch-only storage
    m_watch_addresses.clear();
    m_watch_xpubs.clear();
    m_generated_addresses.clear();
    
    g_logger.info("Wallet component initialized successfully");
    return true;
}

void DaemonWallet::shutdown() {
    g_logger.info("Wallet shutdown started");
    m_watch_addresses.clear();
    m_watch_xpubs.clear();
    m_generated_addresses.clear();
}

// Address validation using the Address class
bool DaemonWallet::isValidAddress(const std::string& address) {
    if (address.empty()) {
        return false;
    }
    
    // Use the Address class validation
    return dinero::Address::validateAddress(address);
}

// Generate scriptPubKey for an address
// ✅ SAFE - RPC INPUT PARSING ONLY (Bitcoin Core pattern)
// This function exists at the RPC boundary to convert user-provided address strings
// to scriptPubKey (consensus data). Address checks here are ALLOWED because:
// 1. This is input validation/parsing, NOT ownership determination
// 2. The scriptPubKey output is what gets used downstream for ownership
// 3. This pattern matches Bitcoin Core's rpc/ directory functions
std::string DaemonWallet::getScriptPubKey(const std::string& address) {
    if (!isValidAddress(address)) {
        g_logger.error("Invalid address for scriptPubKey generation: " + address);
        return "";
    }

    std::vector<uint8_t> script_pubkey;
    std::string error;
    if (!dinero::BuildScriptPubKeyFromAddress(address, script_pubkey, error)) {
        g_logger.error("Failed to build scriptPubKey from address: " + address + " error: " + error);
        return "";
    }

    return dinero::ScriptPubKeyToHex(script_pubkey);
}

// Import watch-only extended public key
bool DaemonWallet::importWatchXPub(const std::string& xpub) {
    if (xpub.empty()) {
        g_logger.error("Empty xpub for import");
        return false;
    }
    
    // Basic xpub validation (should start with xpub, ypub, zpub, etc.)
    if (xpub.length() < 111 || xpub.length() > 115) {
        g_logger.error("Invalid xpub length: " + xpub);
        return false;
    }
    
    // Check if it's already imported
    if (m_watch_xpubs.find(xpub) != m_watch_xpubs.end()) {
        g_logger.info("Xpub already imported: " + xpub);
        return true;
    }
    
    // Import the xpub
    m_watch_xpubs[xpub] = "Imported xpub";
    g_logger.info("Successfully imported watch-only xpub: " + xpub);
    
    return true;
}

// Import watch-only address
bool DaemonWallet::importWatchAddr(const std::string& address) {
    if (!isValidAddress(address)) {
        g_logger.error("Invalid address for watch-only import: " + address);
        return false;
    }
    
    // Check if it's already imported
    if (m_watch_addresses.find(address) != m_watch_addresses.end()) {
        g_logger.info("Address already imported for watching: " + address);
        return true;
    }
    
    // Generate scriptPubKey for the address
    std::string scriptPubKey = getScriptPubKey(address);
    if (scriptPubKey.empty()) {
        g_logger.error("Failed to generate scriptPubKey for address: " + address);
        return false;
    }
    
    // Import the address
    m_watch_addresses[address] = scriptPubKey;
    g_logger.info("Successfully imported watch-only address: " + address);
    
    return true;
}

// Get watch-only balances (simplified implementation)
Json::Value DaemonWallet::getWatchBalances() {
    Json::Value result(Json::objectValue);
    Json::Value addresses(Json::arrayValue);
    
    // Add watch-only addresses
    for (const auto& pair : m_watch_addresses) {
        Json::Value addr_info(Json::objectValue);
        addr_info["address"] = pair.first;
        addr_info["scriptPubKey"] = pair.second;
        addr_info["balance"] = "0.0"; // In production, query blockchain for actual balance
        addr_info["type"] = "watch-only";
        addresses.append(addr_info);
    }
    
    // Add watch-only xpubs
    for (const auto& pair : m_watch_xpubs) {
        Json::Value xpub_info(Json::objectValue);
        xpub_info["xpub"] = pair.first;
        xpub_info["label"] = pair.second;
        xpub_info["type"] = "watch-only-xpub";
        addresses.append(xpub_info);
    }
    
    result["watch_addresses"] = addresses;
    result["total_watch_addresses"] = static_cast<int>(m_watch_addresses.size());
    result["total_watch_xpubs"] = static_cast<int>(m_watch_xpubs.size());
    
    return result;
}

// Generate a new address
std::string DaemonWallet::generateNewAddress() {
    // Generate a new private key and address
    std::array<uint8_t, 32> privateKey = dinero::Address::generatePrivateKey();
    std::vector<uint8_t> publicKey = dinero::Address::derivePublicKey(privateKey, true);
    std::string address = dinero::Address::createAddress(publicKey, dinero::AddressType::DINERO_P2PKH);

    if (!address.empty()) {
        m_generated_addresses.push_back(address);
        g_logger.info("Generated new address: " + address);
    }

    return address;
}

// Generate a new Taproot (P2TR) address
std::string DaemonWallet::generateTaprootAddress() {
    // Generate Taproot keypair
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 32> xonly_pubkey;
    int parity;

    if (!dinero::TaprootKeys::GenerateKeypair(privkey, xonly_pubkey, parity)) {
        g_logger.error("Failed to generate Taproot keypair");
        return "";
    }

    // Get active network HRP
    const std::string& hrp = dinero::HrpForActiveNetworkRef();

    // Create Taproot address
    std::string address = dinero::TaprootKeys::CreateTaprootAddress(xonly_pubkey, hrp);

    if (!address.empty()) {
        m_generated_addresses.push_back(address);
        g_logger.info("Generated new Taproot address: " + address);
    } else {
        g_logger.error("Failed to create Taproot address");
    }

    return address;
}

// Get all generated addresses
std::vector<std::string> DaemonWallet::getAddresses() {
    return m_generated_addresses;
}

} // namespace dinero
