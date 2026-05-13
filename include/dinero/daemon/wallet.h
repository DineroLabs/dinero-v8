#pragma once
#include <string>
#include <vector>
#include <map>
#include "compat/jsoncpp_compat.h"

namespace dinero {

class DaemonWallet {
public:
    DaemonWallet();
    ~DaemonWallet();
    
    bool initialize();
    void shutdown();
    
    // Address validation and scriptPubKey generation
    bool isValidAddress(const std::string& address);
    std::string getScriptPubKey(const std::string& address);
    
    // Watch-only support for GUI balance display
    bool importWatchXPub(const std::string& xpub);
    bool importWatchAddr(const std::string& address);
    Json::Value getWatchBalances();
    
    // Address generation and management
    std::string generateNewAddress();
    std::vector<std::string> getAddresses();
    
private:
    std::map<std::string, std::string> m_watch_addresses;  // address -> scriptPubKey
    std::map<std::string, std::string> m_watch_xpubs;      // xpub -> label
    std::vector<std::string> m_generated_addresses;
};

} // namespace dinero 