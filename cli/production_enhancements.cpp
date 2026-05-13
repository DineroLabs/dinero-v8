// SPDX-License-Identifier: MIT
// Dinero CLI - Production-Grade Enhancements Implementation

#include "production_enhancements.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace dinero::cli {

// Define wallet-required commands
const std::set<std::string> WalletScoping::WALLET_REQUIRED_COMMANDS = {
    "wallet.create",
    "wallet.load", 
    "wallet.encrypt",
    "wallet.lock",
    "wallet.unlock",
    "wallet.change_passphrase",
    "wallet.balance",
    "wallet.history",
    "wallet.utxos",
    "wallet.addresses",
    "wallet.getnewaddress",
    "wallet.label",
    "send"
};

} // namespace dinero::cli
