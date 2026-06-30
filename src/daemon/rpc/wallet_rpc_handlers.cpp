#include "daemon/rpc/wallet_rpc_handlers.h"
#include "daemon/address_helpers.h"
#include "wallet/wallet_manager.h"
#include "compat/jsoncpp_compat.h"
#include "common/logger.h"
#include "bech32.hpp"
#include "daemon/address_metrics.h"
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <cctype>

namespace dinero {

Json::Value wallet_getnewaddress(const Json::Value& params, dinero::WalletManager* wallet_manager) {
    try {
        // Validate parameters
        if (!params.empty() && !params.isArray()) {
            throw std::runtime_error("Invalid parameters: expected array");
        }
        
        // Check if wallet manager is available
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not initialized");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use createwallet or loadwallet first");
        }
        
        // Generate new address using real wallet manager
        std::string addr = wallet_manager->getNewAddress();
        
        if (addr.empty()) {
            throw std::runtime_error("Failed to generate new address");
        }
        
        dinero::g_logger.info("Generated new address: " + addr);
        return Json::Value(addr);
    } catch (const std::exception& e) {
        dinero::g_logger.error("wallet.getnewaddress failed: " + std::string(e.what()));
        throw std::runtime_error("wallet.getnewaddress failed: " + std::string(e.what()));
    }
}

Json::Value wallet_validateaddress(const Json::Value& params, dinero::WalletManager* wallet_manager) {
    try {
        // Validate parameters
        if (params.empty()) {
            throw std::runtime_error("Missing required parameter: address");
        }
        
        if (!params.isArray() || params.size() != 1) {
            throw std::runtime_error("Invalid parameters: expected exactly one address parameter");
        }
        
        if (!params[0].isString()) {
            throw std::runtime_error("Invalid parameter type: address must be string");
        }
        
        std::string addr = params[0].asString();
        
        // Check for empty address
        if (addr.empty()) {
            throw std::runtime_error("Address cannot be empty");
        }
        
        // Check address length constraints
        if (addr.length() < 10 || addr.length() > 100) {
            throw std::runtime_error("Invalid address length");
        }
        
        // Strict Bech32 decoding (vNext implementation)
        Json::Value result(Json::objectValue);
        result["isvalid"] = false;
        result["address"] = addr;
        
        // Get the HRP from active chain params (regtest => "rdin")
        const std::string expected_hrp = GetChainParams().HRP();
        
        // Decode Bech32
        auto dec = bech32::DecodeWithEncoding(addr);
        std::string hrp = std::get<0>(dec);
        std::vector<uint8_t> data = std::get<1>(dec);
        bech32::Encoding encoding = std::get<2>(dec);
        
        // Check encoding and HRP
        if (encoding == bech32::Encoding::BECH32 || encoding == bech32::Encoding::BECH32M) {
            // Case-insensitive HRP comparison
            bool hrp_match = (hrp.length() == expected_hrp.length());
            if (hrp_match) {
                for (size_t i = 0; i < hrp.length(); i++) {
                    if (std::tolower(hrp[i]) != std::tolower(expected_hrp[i])) {
                        hrp_match = false;
                        break;
                    }
                }
            }
            
            // Record HRP mismatch if applicable
            if (!hrp_match) {
                AddressValidationMetrics::getInstance().recordFailure(
                    AddressValidationMetrics::FailureReason::HRP_MISMATCH, addr);
            }
            
            if (hrp_match && !data.empty()) {
                int witver = data[0];
                std::vector<uint8_t> program;
                
                // Convert 5-bit to 8-bit (exclude version byte and 6-byte checksum)
                std::vector<uint8_t> data_slice(data.begin()+1, data.end()-6);
                if (bech32::convertbits(program, data_slice, 5, 8, false)) {
                    // BIP173: v0 must be bech32 with a 20 (P2WPKH) or 32 (P2WSH) byte program.
                    // BIP350: v1..v16 (incl. Taproot v1) must be bech32m, program 2..40 bytes.
                    bool v0_ok = (witver == 0 &&
                                  encoding == bech32::Encoding::BECH32 &&
                                  (program.size() == 20 || program.size() == 32));
                    bool vN_ok = (witver >= 1 && witver <= 16 &&
                                  encoding == bech32::Encoding::BECH32M &&
                                  program.size() >= 2 && program.size() <= 40);

                    if (v0_ok || vN_ok) {
                        result["isvalid"] = true;
                        result["iswitness"] = true;
                        result["isscript"] = (witver == 0 && program.size() == 32);
                        result["witness_version"] = witver;

                        // Convert program to hex
                        std::ostringstream oss;
                        for (uint8_t byte : program) {
                            oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
                        }
                        result["witness_program"] = oss.str();

                        // Canonical scriptPubKey: OP_<witver> OP_PUSHBYTES_<n> <program>.
                        // OP_0 = 0x00; OP_1..OP_16 = 0x51..0x60. Taproot => 5120<program>.
                        std::ostringstream spk;
                        int op = (witver == 0) ? 0x00 : (0x50 + witver);
                        spk << std::hex << std::setfill('0') << std::setw(2) << op;
                        spk << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(program.size());
                        for (uint8_t byte : program) {
                            spk << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
                        }
                        result["scriptPubKey"] = spk.str();
                    } else if (witver >= 1) {
                        result["isvalid"] = false;
                        result["witness_version"] = witver;
                        AddressValidationMetrics::getInstance().recordFailure(
                            AddressValidationMetrics::FailureReason::WITVER_NOT_SUPPORTED, addr);
                    }
                }
            }
        }
        
        // Check if address belongs to wallet
        if (result["isvalid"].asBool()) {
            if (wallet_manager && wallet_manager->hasActiveWallet()) {
                result["ismine"] = wallet_manager->isAddressMine(addr);
                if (result["ismine"].asBool()) {
                    result["account"] = "default";
                }
            } else {
                result["ismine"] = false;
            }
        } else {
            result["ismine"] = false;
        }
        
        return result;
    } catch (const std::exception& e) {
        // Note: addr may be out of scope here, so we use a generic message
        AddressValidationMetrics::getInstance().recordFailure(
            AddressValidationMetrics::FailureReason::MALFORMED, "unknown");
        throw std::runtime_error("wallet.validateaddress failed: " + std::string(e.what()));
    }
}

} // namespace dinero
