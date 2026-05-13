// SPDX-License-Identifier: MIT
// Dinero - Descriptor Wallet RPC Handlers

#include "wallet_descriptor_rpc_handlers.h"
#include "wallet/wallet_manager.h"
#include "wallet/taproot_keys.h"  // Canonical TapTweak (ComputeTweakedPubkey)
#include "wallet/bip84_descriptor.h"
#include "wallet/bip86_descriptor.h"
#include "wallet/wallet_policy.h"
#include "wallet/descriptor_checksum.h"
#include "wallet/descriptor_activation.h"
#include "wallet/retired_coin_type_guard.h"
#include "consensus/coin_type.h"
#include "address/addr_codec.h"
#include "crypto/hd_keychain.h"
#include "crypto/extended_pubkey.h"
#include "crypto/hash.h"
#include "common/AddressCodec.h"
#include "common/logger.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <sstream>
#include <ctime>
#include <sqlite3.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

namespace dinero {

// Parameter validation helpers
namespace {
    constexpr uint32_t HARDENED_INDEX = 0x80000000;

    Network ActiveNetworkForEncoding() {
        const std::string& hrp = dinero::HrpForActiveNetworkRef();
        if (hrp == "tdin") {
            return Network::TEST;
        }
        if (hrp == "rdin") {
            return Network::REGTEST;
        }
        return Network::MAIN;
    }

    std::string DetectWalletPolicy(sqlite3* db) {
        if (!db) {
            return "bip84";
        }

        std::string wallet_policy = "bip84";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT wallet_policy FROM wallet_meta WHERE id = 1", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* policy_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (policy_cstr && *policy_cstr) {
                    wallet_policy = policy_cstr;
                }
            }
            sqlite3_finalize(stmt);
        }
        return wallet_policy;
    }

    std::string ToFingerprintHex(uint32_t fingerprint) {
        std::ostringstream oss;
        oss << std::uppercase << std::hex;
        oss.width(8);
        oss.fill('0');
        oss << fingerprint;
        return oss.str();
    }

    bool DeriveMasterFingerprintAndAccountXpub(
        const std::vector<uint8_t>& seed,
        uint32_t purpose,
        std::string& master_fp_hex,
        std::string& account_xpub,
        std::string* error_out = nullptr
    ) {
        if (seed.size() != 64) {
            if (error_out) {
                *error_out = "master seed must be 64 bytes";
            }
            return false;
        }

        try {
            auto master_key = dinero::crypto::HDKeychain::fromSeed(seed);

            const auto master_pub = master_key.getPublicKey();
            const auto fp_hash = din::crypto::HASH160(master_pub.data(), master_pub.size());
            const uint32_t fingerprint =
                (static_cast<uint32_t>(fp_hash[0]) << 24) |
                (static_cast<uint32_t>(fp_hash[1]) << 16) |
                (static_cast<uint32_t>(fp_hash[2]) << 8) |
                static_cast<uint32_t>(fp_hash[3]);

            const auto purpose_key = master_key.derive(purpose | HARDENED_INDEX);
            const auto coin_key = purpose_key.derive(dinero::consensus::DINERO_COIN_TYPE | HARDENED_INDEX);
            const auto account_key = coin_key.derive(0 | HARDENED_INDEX);

            master_fp_hex = ToFingerprintHex(fingerprint);
            account_xpub = account_key.serialize(true);
            return true;
        } catch (const std::exception& e) {
            if (error_out) {
                *error_out = e.what();
            }
            return false;
        }
    }

    bool DeriveAddressFromMasterSeed(
        const std::vector<uint8_t>& seed,
        bool taproot,
        bool is_change,
        uint32_t index,
        std::string& address_out,
        std::string* error_out = nullptr
    ) {
        if (seed.size() != 64) {
            if (error_out) {
                *error_out = "master seed must be 64 bytes";
            }
            return false;
        }

        try {
            const uint32_t purpose = taproot ? 86u : 84u;
            const uint32_t chain = is_change ? 1u : 0u;

            auto master_key = dinero::crypto::HDKeychain::fromSeed(seed);
            auto purpose_key = master_key.derive(purpose | HARDENED_INDEX);
            auto coin_key = purpose_key.derive(dinero::consensus::DINERO_COIN_TYPE | HARDENED_INDEX);
            auto account_key = coin_key.derive(0 | HARDENED_INDEX);
            auto chain_key = account_key.derive(chain);
            auto child_key = chain_key.derive(index);
            const auto pubkey = child_key.getPublicKey();

            const Network net = ActiveNetworkForEncoding();
            if (taproot) {
                if (pubkey.size() != 33) {
                    if (error_out) {
                        *error_out = "invalid compressed pubkey size";
                    }
                    return false;
                }

                std::array<uint8_t, 32> internal_key{};
                std::copy(pubkey.begin() + 1, pubkey.end(), internal_key.begin());

                std::array<uint8_t, 32> output_key{};
                if (!dinero::TaprootKeys::ComputeTweakedPubkey(internal_key, output_key)) {
                    if (error_out) {
                        *error_out = "Taproot key tweak failed";
                    }
                    return false;
                }

                std::vector<uint8_t> output_key_vec(output_key.begin(), output_key.end());
                address_out = AddressCodec::encodeP2TR(net, output_key_vec);
                return true;
            }

            const auto hash160 = din::crypto::HASH160(pubkey.data(), pubkey.size());
            std::vector<uint8_t> hash160_vec(hash160.begin(), hash160.end());
            address_out = AddressCodec::encodeP2WPKH(net, hash160_vec);
            return true;
        } catch (const std::exception& e) {
            if (error_out) {
                *error_out = e.what();
            }
            return false;
        }
    }

    void require_string(const Json::Value& params, const std::string& field) {
        if (!params.isObject() || !params.isMember(field) || !params[field].isString()) {
            throw std::invalid_argument("Missing or invalid string field: " + field);
        }
    }

    bool get_bool(const Json::Value& params, const std::string& field, bool default_val) {
        if (params.isObject() && params.isMember(field) && params[field].isBool()) {
            return params[field].asBool();
        }
        return default_val;
    }

    int get_int(const Json::Value& params, const std::string& field, int default_val) {
        if (params.isObject() && params.isMember(field) && params[field].isNumeric()) {
            return params[field].asInt();
        }
        return default_val;
    }
}

Json::Value rpc_wallet_listdescriptors(const Json::Value& params, WalletManager* wallet_manager) {
    try {
        // Check WalletManager instance
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }

        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }

        bool include_private = get_bool(params, "private", false);

        sqlite3* db = wallet_manager->getCurrentDatabase();
        if (!db) {
            throw std::runtime_error("No active wallet database");
        }

        // Detect wallet policy from database
        std::string wallet_policy = DetectWalletPolicy(db);
        std::string descriptor_type = "wpkh";
        if (wallet_policy == "bip86") {
            descriptor_type = "tr";
        }
        g_logger.info("Detected wallet policy: " + wallet_policy + " (" + descriptor_type + ")");

        // Derive master fingerprint + account xpub from active wallet master seed.
        auto seed_opt = wallet_manager->GetMasterSeed();
        if (!seed_opt.has_value()) {
            throw std::runtime_error("Wallet master seed unavailable (wallet may be locked)");
        }

        std::string master_fp;
        std::string account_xpub;
        std::string derive_error;
        const uint32_t purpose = (wallet_policy == "bip86") ? 86u : 84u;
        if (!DeriveMasterFingerprintAndAccountXpub(
                seed_opt.value(),
                purpose,
                master_fp,
                account_xpub,
                &derive_error)) {
            throw std::runtime_error("Failed to derive descriptor root keys: " + derive_error);
        }

        // Create descriptors based on policy
        std::string receive_desc, change_desc;
        if (wallet_policy == "bip86") {
            // BIP86 Taproot descriptors
            auto [recv, chg] = din::BIP86DescriptorFactory::createDefaultDescriptors(
                master_fp, account_xpub, dinero::consensus::DINERO_COIN_TYPE);
            receive_desc = recv;
            change_desc = chg;
        } else {
            // BIP84 SegWit descriptors
            auto [recv, chg] = din::BIP84DescriptorFactory::createDefaultDescriptors(
                master_fp, account_xpub, dinero::consensus::DINERO_COIN_TYPE);
            receive_desc = recv;
            change_desc = chg;
        }

        // Add checksums
        std::string receive_desc_with_checksum = din::DescriptorChecksum::AddChecksum(receive_desc);
        std::string change_desc_with_checksum = din::DescriptorChecksum::AddChecksum(change_desc);

        // Query wallet DB for next indices
        int next_receive = wallet_manager->getNextAddressIndex(0, 0);  // account=0, change=0
        int next_change = wallet_manager->getNextAddressIndex(0, 1);   // account=0, change=1

        Json::Value result;
        result["wallet_name"] = wallet_manager->current();
        result["descriptors"] = Json::Value(Json::arrayValue);

        // Receive descriptor
        Json::Value recv;
        recv["desc"] = include_private ? "*** private descriptor redacted ***" : receive_desc_with_checksum;
        recv["timestamp"] = 0;  // TODO: Add timestamp tracking
        recv["active"] = true;
        recv["internal"] = false;
        recv["range"] = Json::Value(Json::arrayValue);
        recv["range"].append(0);
        recv["range"].append(1000);  // Standard gap limit * 50
        recv["next"] = next_receive;
        result["descriptors"].append(recv);

        // Change descriptor
        Json::Value chg;
        chg["desc"] = include_private ? "*** private descriptor redacted ***" : change_desc_with_checksum;
        chg["timestamp"] = 0;
        chg["active"] = true;
        chg["internal"] = true;
        chg["range"] = Json::Value(Json::arrayValue);
        chg["range"].append(0);
        chg["range"].append(1000);
        chg["next"] = next_change;
        result["descriptors"].append(chg);

        g_logger.info("Listed descriptors for wallet: " + wallet_manager->current());
        return result;

    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list descriptors: ") + e.what();
        g_logger.error("wallet.listdescriptors error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_getdescriptorinfo(const Json::Value& params, WalletManager* wallet_manager) {
    try {
        // Note: wallet_manager can be null for descriptor-only operations

        // Support both positional array and object parameters
        std::string descriptor;
        if (params.isArray() && params.size() > 0 && params[0].isString()) {
            // Positional: wallet.getdescriptorinfo "descriptor_string"
            descriptor = params[0].asString();
        } else if (params.isObject() && params.isMember("descriptor") && params["descriptor"].isString()) {
            // Named: wallet.getdescriptorinfo {"descriptor": "descriptor_string"}
            descriptor = params["descriptor"].asString();
        } else {
            throw std::invalid_argument("Missing or invalid descriptor parameter");
        }

        // Strip checksum if present
        std::string clean_descriptor = din::DescriptorChecksum::StripChecksum(descriptor);
        dinero::wallet::RejectRetiredLegacyCoinTypeText(clean_descriptor, "wallet.getdescriptorinfo");

        // Detect descriptor type and parse accordingly
        bool is_bip86 = (clean_descriptor.substr(0, 3) == "tr(");
        bool is_bip84 = (clean_descriptor.substr(0, 5) == "wpkh(");

        std::string descriptor_type;
        std::string fingerprint;
        std::vector<uint32_t> derivation_path;
        bool valid = false;
        std::string error_msg;

        if (is_bip86) {
            // Parse as BIP86 Taproot descriptor
            auto parsed = din::BIP86DescriptorFactory::parseDescriptor(clean_descriptor);
            valid = parsed.valid;
            if (valid) {
                descriptor_type = "tr";
                fingerprint = parsed.fingerprint;
                derivation_path = parsed.derivation_path;
            } else {
                error_msg = parsed.error;
            }
        } else if (is_bip84) {
            // Parse as BIP84 SegWit descriptor
            auto parsed = din::BIP84DescriptorFactory::parseDescriptor(clean_descriptor);
            valid = parsed.valid;
            if (valid) {
                descriptor_type = "wpkh";
                fingerprint = parsed.fingerprint;
                derivation_path = parsed.derivation_path;
            } else {
                error_msg = parsed.error;
            }
        } else {
            throw std::invalid_argument("Unsupported descriptor type. Only wpkh() and tr() are supported.");
        }

        if (!valid) {
            throw std::invalid_argument("Invalid descriptor: " + error_msg);
        }

        // Compute checksum
        std::string checksum = din::DescriptorChecksum::Compute(clean_descriptor);

        Json::Value result;
        result["descriptor"] = clean_descriptor;
        result["checksum"] = checksum;
        result["isrange"] = true;  // BIP84 and BIP86 descriptors always have wildcard
        result["issolvable"] = true;
        result["hasprivatekeys"] = false;  // xpub only
        result["fingerprint"] = fingerprint;
        result["type"] = descriptor_type;

        // Convert derivation_path vector to string
        std::ostringstream path_stream;
        path_stream << "m";
        for (uint32_t idx : derivation_path) {
            uint32_t clean_idx = idx & 0x7FFFFFFF;
            bool hardened = (idx & 0x80000000) != 0;
            path_stream << "/" << clean_idx;
            if (hardened) path_stream << "h";
        }
        result["derivation_path"] = path_stream.str();

        g_logger.info("Analyzed descriptor: " + clean_descriptor);
        return result;

    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to analyze descriptor: ") + e.what();
        g_logger.error("wallet.getdescriptorinfo error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_deriveaddresses(const Json::Value& params, WalletManager* wallet_manager) {
    try {
        // Support both positional array and object parameters
        std::string descriptor;
        int start = 0, end = 0;

        if (params.isArray() && params.size() > 0) {
            // Positional parameters: wallet.deriveaddresses "descriptor" start end
            // OR: wallet.deriveaddresses "descriptor" [start, end]
            if (!params[0].isString()) {
                throw std::invalid_argument("First parameter must be descriptor string");
            }
            descriptor = params[0].asString();

            // Parse range - support both separate start/end and [start, end] array
            if (params.size() >= 3 && params[1].isNumeric() && params[2].isNumeric()) {
                // Format: wallet.deriveaddresses "desc" 0 5
                start = params[1].asInt();
                end = params[2].asInt();
            } else if (params.size() >= 2 && params[1].isArray() && params[1].size() >= 2) {
                // Format: wallet.deriveaddresses "desc" [0, 5]
                start = params[1][0].asInt();
                end = params[1][1].asInt();
            } else if (params.size() >= 2 && params[1].isNumeric()) {
                // Single index: wallet.deriveaddresses "desc" 0
                start = params[1].asInt();
                end = start;
            }
            // If no range provided, default to [0, 0] - single address
        } else if (params.isObject()) {
            // Named parameters: {"descriptor": "...", "range": [start, end]}
            if (!params.isMember("descriptor") || !params["descriptor"].isString()) {
                throw std::invalid_argument("Missing or invalid descriptor parameter");
            }
            descriptor = params["descriptor"].asString();

            if (params.isMember("range") && params["range"].isArray() && params["range"].size() >= 2) {
                start = params["range"][0].asInt();
                end = params["range"][1].asInt();
            }
        } else {
            throw std::invalid_argument("Invalid parameters format");
        }

        // Strip checksum if present
        std::string clean_descriptor = din::DescriptorChecksum::StripChecksum(descriptor);
        dinero::wallet::RejectRetiredLegacyCoinTypeText(clean_descriptor, "wallet.deriveaddresses");

        // Detect descriptor type and parse accordingly
        bool is_bip86 = (clean_descriptor.substr(0, 3) == "tr(");
        bool is_bip84 = (clean_descriptor.substr(0, 5) == "wpkh(");

        bool is_change = false;
        bool valid = false;
        std::string error_msg;

        if (is_bip86) {
            // Parse as BIP86 Taproot descriptor
            auto parsed = din::BIP86DescriptorFactory::parseDescriptor(clean_descriptor);
            valid = parsed.valid;
            if (valid) {
                is_change = parsed.is_change;
            } else {
                error_msg = parsed.error;
            }
        } else if (is_bip84) {
            // Parse as BIP84 SegWit descriptor
            auto parsed = din::BIP84DescriptorFactory::parseDescriptor(clean_descriptor);
            valid = parsed.valid;
            if (valid) {
                is_change = parsed.is_change;
            } else {
                error_msg = parsed.error;
            }
        } else {
            throw std::invalid_argument("Unsupported descriptor type. Only wpkh() and tr() are supported.");
        }

        if (!valid) {
            throw std::invalid_argument("Invalid descriptor: " + error_msg);
        }

        if (end < start) {
            throw std::invalid_argument("Invalid range: end must be >= start");
        }

        if (end - start > 1000) {
            throw std::invalid_argument("Range too large (max 1000 addresses)");
        }

        if (!wallet_manager || !wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }

        auto seed_opt = wallet_manager->GetMasterSeed();
        if (!seed_opt.has_value()) {
            throw std::runtime_error("Wallet master seed unavailable (wallet may be locked)");
        }

        Json::Value result;
        result["addresses"] = Json::Value(Json::arrayValue);

        // Derive addresses from the active wallet seed using canonical path rules.
        for (int i = start; i <= end; i++) {
            std::string address;
            std::string derive_error;
            if (!DeriveAddressFromMasterSeed(
                    seed_opt.value(),
                    is_bip86,
                    is_change,
                    static_cast<uint32_t>(i),
                    address,
                    &derive_error)) {
                throw std::runtime_error("Failed to derive address at index " + std::to_string(i) + ": " + derive_error);
            }
            result["addresses"].append(address);
        }

        g_logger.info("Derived " + std::to_string(end - start + 1) + " addresses from descriptor");
        return result;

    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to derive addresses: ") + e.what();
        g_logger.error("wallet.deriveaddresses error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_exportdescriptors(const Json::Value& params, WalletManager* wallet_manager) {
    try {
        // Check WalletManager instance
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }

        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }

        sqlite3* db = wallet_manager->getCurrentDatabase();
        if (!db) {
            throw std::runtime_error("No active wallet database");
        }

        // Detect wallet policy from database
        const std::string wallet_policy = DetectWalletPolicy(db);

        auto seed_opt = wallet_manager->GetMasterSeed();
        if (!seed_opt.has_value()) {
            throw std::runtime_error("Wallet master seed unavailable (wallet may be locked)");
        }

        std::string master_fp;
        std::string account_xpub;
        std::string derive_error;
        const uint32_t purpose = (wallet_policy == "bip86") ? 86u : 84u;
        if (!DeriveMasterFingerprintAndAccountXpub(
                seed_opt.value(),
                purpose,
                master_fp,
                account_xpub,
                &derive_error)) {
            throw std::runtime_error("Failed to derive descriptor root keys: " + derive_error);
        }

        // Create descriptors based on policy
        std::string receive_desc, change_desc;
        if (wallet_policy == "bip86") {
            // BIP86 Taproot descriptors
            auto [recv, chg] = din::BIP86DescriptorFactory::createDefaultDescriptors(
                master_fp, account_xpub, dinero::consensus::DINERO_COIN_TYPE);
            receive_desc = recv;
            change_desc = chg;
        } else {
            // BIP84 SegWit descriptors
            auto [recv, chg] = din::BIP84DescriptorFactory::createDefaultDescriptors(
                master_fp, account_xpub, dinero::consensus::DINERO_COIN_TYPE);
            receive_desc = recv;
            change_desc = chg;
        }

        // Add checksums
        std::string receive_desc_with_checksum = din::DescriptorChecksum::AddChecksum(receive_desc);
        std::string change_desc_with_checksum = din::DescriptorChecksum::AddChecksum(change_desc);

        // Build export format (simplified for backup/import)
        Json::Value result;
        result["wallet_name"] = wallet_manager->current();
        result["policy"] = wallet_policy;
        result["descriptors"] = Json::Value(Json::arrayValue);

        // Receive descriptor (external chain)
        Json::Value recv;
        recv["desc"] = receive_desc_with_checksum;
        recv["active"] = true;
        recv["internal"] = false;
        recv["timestamp"] = "now";  // Import as of now
        result["descriptors"].append(recv);

        // Change descriptor (internal chain)
        Json::Value chg;
        chg["desc"] = change_desc_with_checksum;
        chg["active"] = true;
        chg["internal"] = true;
        chg["timestamp"] = "now";
        result["descriptors"].append(chg);

        g_logger.info("Exported descriptors for wallet: " + wallet_manager->current());
        return result;

    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to export descriptors: ") + e.what();
        g_logger.error("wallet.exportdescriptors error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_importdescriptors(const Json::Value& params, WalletManager* wallet_manager) {
    try {
        // Validate wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }

        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }

        // Parse requests array
        Json::Value requests;
        if (params.isArray() && params.size() > 0) {
            // Direct array format: wallet.importdescriptors [{...}, {...}]
            requests = params;
        } else if (params.isObject() && params.isMember("requests") && params["requests"].isArray()) {
            // Named parameter format: wallet.importdescriptors {"requests": [{...}]}
            requests = params["requests"];
        } else {
            throw std::invalid_argument("Expected array of descriptor import requests");
        }

        if (requests.empty()) {
            throw std::invalid_argument("At least one descriptor required");
        }

        // Result array (one entry per descriptor)
        Json::Value results(Json::arrayValue);

        // Use active wallet database handle from WalletManager (authoritative context).
        sqlite3* db = wallet_manager->getCurrentDatabase();
        if (!db) {
            throw std::runtime_error("No active wallet database");
        }

        // Process each descriptor import request
        for (Json::ArrayIndex i = 0; i < requests.size(); i++) {
            Json::Value result;
            result["success"] = false;

            try {
                const Json::Value& req = requests[i];

                // Validate that request is an object
                if (!req.isObject()) {
                    throw std::invalid_argument("Each request must be a JSON object, got type: " +
                                               std::to_string(req.type()));
                }

                // Extract required fields
                if (!req.isMember("desc") || !req["desc"].isString()) {
                    throw std::invalid_argument("Missing or invalid 'desc' field");
                }

                std::string descriptor = req["desc"].asString();
                bool active = req.isMember("active") && req["active"].isBool() ? req["active"].asBool() : false;
                bool internal = req.isMember("internal") && req["internal"].isBool() ? req["internal"].asBool() : false;
                std::string timestamp_str = (req.isMember("timestamp") && req["timestamp"].isString()) ?
                                           req["timestamp"].asString() : "now";
                std::string label = (req.isMember("label") && req["label"].isString()) ?
                                   req["label"].asString() : "";

                // Parse range (default: [0, 1000])
                int range_start = 0;
                int range_end = 1000;
                if (req.isMember("range") && req["range"].isArray() && req["range"].size() >= 2) {
                    range_start = req["range"][0].asInt();
                    range_end = req["range"][1].asInt();
                }

                // PHASE 2: Descriptor Activation Gate (Security Critical)
                // Get wallet policy and master fingerprint for activation validation
                std::string wallet_policy_str = "bip84";  // Default fallback
                std::string wallet_fingerprint;

                try {
                    // Query wallet metadata
                    sqlite3_stmt* meta_stmt = nullptr;
                    const char* meta_sql = "SELECT wallet_policy FROM wallet_meta WHERE id = 1";
                    if (sqlite3_prepare_v2(db, meta_sql, -1, &meta_stmt, nullptr) == SQLITE_OK) {
                        if (sqlite3_step(meta_stmt) == SQLITE_ROW) {
                            const char* policy_cstr = reinterpret_cast<const char*>(sqlite3_column_text(meta_stmt, 0));
                            if (policy_cstr) {
                                wallet_policy_str = policy_cstr;
                            }
                        }
                        sqlite3_finalize(meta_stmt);
                    }

                    auto seed_opt = wallet_manager->GetMasterSeed();
                    if (seed_opt.has_value()) {
                        std::string ignored_xpub;
                        std::string derive_error;
                        const uint32_t purpose = (wallet_policy_str == "bip86") ? 86u : 84u;
                        DeriveMasterFingerprintAndAccountXpub(
                            seed_opt.value(),
                            purpose,
                            wallet_fingerprint,
                            ignored_xpub,
                            &derive_error
                        );
                    }
                } catch (const std::exception& e) {
                    g_logger.warning("Failed to retrieve wallet metadata: " + std::string(e.what()));
                }

                // Validate range
                if (range_end < range_start) {
                    throw std::invalid_argument("Invalid range: end must be >= start");
                }
                if (range_end - range_start > 10000) {
                    throw std::invalid_argument("Range too large (max 10000 addresses)");
                }

                // STEP 0: Watch-only descriptors bypass checksum validation
                // Watch-only descriptors grant no signing authority, so strict validation is optional
                // Policy enforcement still skipped via activation gate
                if (active) {
                    // Validate checksum for active descriptors only
                    if (!din::DescriptorChecksum::Verify(descriptor)) {
                        throw std::invalid_argument("Invalid descriptor checksum");
                    }
                } else {
                    // Watch-only: checksum optional, but if present it must be valid
                    // Strip checksum if present for parsing
                    g_logger.info("Watch-only descriptor import: skipping strict checksum validation");
                }

                std::string clean_desc = din::DescriptorChecksum::StripChecksum(descriptor);
                dinero::wallet::RejectRetiredLegacyCoinTypeText(clean_desc, "wallet.importdescriptors");

                // Determine descriptor type and parse
                std::string descriptor_type;
                std::string fingerprint;
                std::vector<uint32_t> derivation_path;

                if (clean_desc.substr(0, 5) == "wpkh(") {
                    descriptor_type = "wpkh";
                    auto parsed = din::BIP84DescriptorFactory::parseDescriptor(clean_desc);
                    if (!parsed.valid) {
                        throw std::invalid_argument("Failed to parse BIP84 descriptor: " + parsed.error);
                    }
                    fingerprint = parsed.fingerprint;
                    derivation_path = parsed.derivation_path;
                } else if (clean_desc.substr(0, 3) == "tr(") {
                    descriptor_type = "tr";
                    auto parsed = din::BIP86DescriptorFactory::parseDescriptor(clean_desc);
                    if (!parsed.valid) {
                        throw std::invalid_argument("Failed to parse BIP86 descriptor: " + parsed.error);
                    }
                    fingerprint = parsed.fingerprint;
                    derivation_path = parsed.derivation_path;
                } else {
                    throw std::invalid_argument("Unsupported descriptor type (only wpkh and tr supported)");
                }

                // PHASE 2: Activation Gate Validation
                // Parse wallet policy and validate descriptor activation
                WalletPolicy wallet_policy = DescriptorActivationValidator::ParseWalletPolicy(wallet_policy_str);

                // For now, assume no private key material (xpub only, watch-only or external)
                // Future: Check descriptor for xprv vs xpub
                bool has_private_key = false;

                // Validate descriptor activation
                ActivationValidationResult validation = DescriptorActivationValidator::ValidateActivation(
                    descriptor_type,
                    wallet_policy,
                    active,
                    fingerprint,
                    wallet_fingerprint,
                    derivation_path,
                    has_private_key
                );

                if (!validation.valid) {
                    throw std::runtime_error("Descriptor activation rejected: " + validation.error_message);
                }

                std::string signing_capability = DescriptorActivationValidator::SigningCapabilityToString(
                    validation.signing_capability
                );

                g_logger.info("Descriptor activation validated: type=" + descriptor_type +
                             ", policy=" + wallet_policy_str +
                             ", active=" + (active ? "true" : "false") +
                             ", capability=" + signing_capability);

                // Check if descriptor already imported
                sqlite3_stmt* check_stmt = nullptr;
                const char* check_sql = "SELECT id FROM imported_descriptors WHERE descriptor = ?";
                if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(check_stmt, 1, descriptor.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(check_stmt) == SQLITE_ROW) {
                        sqlite3_finalize(check_stmt);
                        throw std::runtime_error("Descriptor already imported");
                    }
                    sqlite3_finalize(check_stmt);
                }

                // Convert timestamp
                int64_t timestamp_value = 0;
                if (timestamp_str == "now") {
                    timestamp_value = std::time(nullptr);
                } else {
                    try {
                        timestamp_value = std::stoll(timestamp_str);
                    } catch (...) {
                        throw std::invalid_argument("Invalid timestamp format");
                    }
                }

                // Format derivation path for storage
                std::ostringstream path_oss;
                for (size_t i = 0; i < derivation_path.size(); ++i) {
                    if (i > 0) path_oss << "/";
                    uint32_t idx = derivation_path[i];
                    if (idx & 0x80000000) {
                        path_oss << (idx & 0x7FFFFFFF) << "h";
                    } else {
                        path_oss << idx;
                    }
                }
                std::string derivation_path_str = path_oss.str();

                // Insert descriptor into database (with Phase 2 fields)
                sqlite3_stmt* insert_stmt = nullptr;
                const char* insert_sql = R"(
                    INSERT INTO imported_descriptors
                    (descriptor, descriptor_type, internal, active, range_start, range_end, timestamp, label, fingerprint,
                     signing_capability, key_origin_fingerprint, derivation_path_prefix, activation_timestamp, activated_by)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                )";

                if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
                    throw std::runtime_error("Failed to prepare insert statement: " + std::string(sqlite3_errmsg(db)));
                }

                sqlite3_bind_text(insert_stmt, 1, descriptor.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_stmt, 2, descriptor_type.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insert_stmt, 3, internal ? 1 : 0);
                sqlite3_bind_int(insert_stmt, 4, active ? 1 : 0);
                sqlite3_bind_int(insert_stmt, 5, range_start);
                sqlite3_bind_int(insert_stmt, 6, range_end);
                sqlite3_bind_int64(insert_stmt, 7, timestamp_value);
                sqlite3_bind_text(insert_stmt, 8, label.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_stmt, 9, fingerprint.c_str(), -1, SQLITE_TRANSIENT);

                // Phase 2 fields
                sqlite3_bind_text(insert_stmt, 10, signing_capability.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_stmt, 11, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_stmt, 12, derivation_path_str.c_str(), -1, SQLITE_TRANSIENT);

                if (active) {
                    sqlite3_bind_int64(insert_stmt, 13, std::time(nullptr));
                    sqlite3_bind_text(insert_stmt, 14, "rpc", -1, SQLITE_STATIC);
                } else {
                    sqlite3_bind_null(insert_stmt, 13);
                    sqlite3_bind_null(insert_stmt, 14);
                }

                if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
                    sqlite3_finalize(insert_stmt);
                    throw std::runtime_error("Failed to insert descriptor: " + std::string(sqlite3_errmsg(db)));
                }

                int64_t descriptor_id = sqlite3_last_insert_rowid(db);
                sqlite3_finalize(insert_stmt);

                // Derive addresses and register watch scripts
                std::string xpub;
                bool is_change_path = false;

                if (descriptor_type == "wpkh") {
                    auto parsed = din::BIP84DescriptorFactory::parseDescriptor(clean_desc);
                    xpub = parsed.xpub;
                    is_change_path = parsed.is_change;
                } else if (descriptor_type == "tr") {
                    auto parsed = din::BIP86DescriptorFactory::parseDescriptor(clean_desc);
                    xpub = parsed.xpub;
                    is_change_path = parsed.is_change;
                }

                // Derive addresses for the specified range
                if (!xpub.empty()) {
                    try {
                        using dinero::crypto::ExtendedPubKey;

                        // Deserialize xpub
                        ExtendedPubKey ext_key = ExtendedPubKey::FromString(xpub);

                        // BIP32 derivation: Derive change key if needed (xpub/0 or xpub/1)
                        if (is_change_path) {
                            ext_key = ext_key.Derive(1);  // Derive m/.../1 for change addresses
                        } else {
                            ext_key = ext_key.Derive(0);  // Derive m/.../0 for receive addresses
                        }

                        // Prepare INSERT statement for addresses
                        const char* insert_addr_sql = R"(
                            INSERT INTO imported_descriptor_addresses
                            (descriptor_id, address_index, address, script_pubkey, key_id, internal_key_id, output_key_id)
                            VALUES (?, ?, ?, ?, ?, ?, ?)
                        )";

                        sqlite3_stmt* addr_stmt;
                        if (sqlite3_prepare_v2(db, insert_addr_sql, -1, &addr_stmt, nullptr) != SQLITE_OK) {
                            throw std::runtime_error("Failed to prepare address insert: " + std::string(sqlite3_errmsg(db)));
                        }

                        int addresses_derived = 0;
                        Network net = ActiveNetworkForEncoding();
                        secp256k1_context* secp_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

                        for (int i = range_start; i <= range_end; ++i) {
                            // Derive child key
                            ExtendedPubKey child = ext_key.Derive(static_cast<uint32_t>(i));
                            std::vector<uint8_t> pubkey_bytes = child.GetPublicKey();

                            std::string address;
                            std::vector<uint8_t> script_pubkey;
                            std::vector<uint8_t> key_id_vec;
                            std::vector<uint8_t> internal_key_vec;
                            std::vector<uint8_t> output_key_vec;

                            if (descriptor_type == "wpkh") {
                                // BIP84: HASH160(pubkey) -> P2WPKH
                                din::crypto::Ripemd160 hash160 = din::crypto::HASH160(pubkey_bytes);
                                key_id_vec.assign(hash160.begin(), hash160.end());

                                // Create P2WPKH scriptPubKey: OP_0 <20-byte-hash>
                                script_pubkey.push_back(0x00); // OP_0
                                script_pubkey.push_back(0x14); // Push 20 bytes
                                script_pubkey.insert(script_pubkey.end(), hash160.begin(), hash160.end());

                                // Encode bech32 address
                                address = AddressCodec::encodeP2WPKH(net, key_id_vec);

                            } else if (descriptor_type == "tr") {
                                // BIP86: Convert to x-only pubkey, apply BIP86 tweak, create P2TR
                                secp256k1_pubkey pubkey_obj;
                                if (!secp256k1_ec_pubkey_parse(secp_ctx, &pubkey_obj, pubkey_bytes.data(), pubkey_bytes.size())) {
                                    throw std::runtime_error("Failed to parse pubkey");
                                }

                                // Convert to x-only pubkey (internal key)
                                secp256k1_xonly_pubkey xonly_internal;
                                if (!secp256k1_xonly_pubkey_from_pubkey(secp_ctx, &xonly_internal, nullptr, &pubkey_obj)) {
                                    throw std::runtime_error("Failed to create x-only pubkey");
                                }

                                // Serialize internal key
                                uint8_t internal_key[32];
                                if (!secp256k1_xonly_pubkey_serialize(secp_ctx, internal_key, &xonly_internal)) {
                                    throw std::runtime_error("Failed to serialize internal key");
                                }
                                internal_key_vec.assign(internal_key, internal_key + 32);

                                // Canonical TapTweak via TaprootKeys::ComputeTweakedPubkey
                                std::array<uint8_t, 32> internal_arr, output_arr;
                                std::copy(internal_key, internal_key + 32, internal_arr.begin());
                                if (!dinero::TaprootKeys::ComputeTweakedPubkey(internal_arr, output_arr)) {
                                    throw std::runtime_error("Failed to compute BIP86 tweaked output key");
                                }
                                uint8_t output_key[32];
                                std::copy(output_arr.begin(), output_arr.end(), output_key);
                                output_key_vec.assign(output_key, output_key + 32);

                                // Create P2TR scriptPubKey: OP_1 <32-byte-xonly-pubkey>
                                script_pubkey.push_back(0x51); // OP_1
                                script_pubkey.push_back(0x20); // Push 32 bytes
                                script_pubkey.insert(script_pubkey.end(), output_key, output_key + 32);

                                // Encode bech32m address
                                address = AddressCodec::encodeP2TR(net, output_key_vec);

                                // For BIP86, key_id is the HASH160 of the output key (for compatibility)
                                din::crypto::Ripemd160 hash160 = din::crypto::HASH160(output_key_vec);
                                key_id_vec.assign(hash160.begin(), hash160.end());
                            }

                            // Insert address into database
                            sqlite3_bind_int64(addr_stmt, 1, descriptor_id);
                            sqlite3_bind_int(addr_stmt, 2, i);
                            sqlite3_bind_text(addr_stmt, 3, address.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_blob(addr_stmt, 4, script_pubkey.data(), script_pubkey.size(), SQLITE_TRANSIENT);
                            sqlite3_bind_blob(addr_stmt, 5, key_id_vec.data(), key_id_vec.size(), SQLITE_TRANSIENT);

                            if (descriptor_type == "tr") {
                                sqlite3_bind_blob(addr_stmt, 6, internal_key_vec.data(), internal_key_vec.size(), SQLITE_TRANSIENT);
                                sqlite3_bind_blob(addr_stmt, 7, output_key_vec.data(), output_key_vec.size(), SQLITE_TRANSIENT);
                            } else {
                                sqlite3_bind_null(addr_stmt, 6);
                                sqlite3_bind_null(addr_stmt, 7);
                            }

                            if (sqlite3_step(addr_stmt) != SQLITE_DONE) {
                                sqlite3_finalize(addr_stmt);
                                secp256k1_context_destroy(secp_ctx);
                                throw std::runtime_error("Failed to insert address: " + std::string(sqlite3_errmsg(db)));
                            }

                            sqlite3_reset(addr_stmt);
                            addresses_derived++;
                        }

                        sqlite3_finalize(addr_stmt);
                        secp256k1_context_destroy(secp_ctx);

                        result["success"] = true;
                        result["addresses_derived"] = addresses_derived;
                        g_logger.info("Imported descriptor (id=" + std::to_string(descriptor_id) + "): " +
                                     descriptor.substr(0, 50) + "... [" + descriptor_type + ", " +
                                     std::to_string(addresses_derived) + " addresses derived]");

                    } catch (const std::exception& e) {
                        result["success"] = false;
                        result["warning"] = std::string("Descriptor stored but address derivation failed: ") + e.what();
                        g_logger.warning("Address derivation failed for descriptor " + std::to_string(descriptor_id) + ": " + e.what());
                    }
                } else {
                    result["success"] = true;
                    result["warning"] = "No xpub found in descriptor - watch-only without address derivation";
                    g_logger.info("Imported descriptor (id=" + std::to_string(descriptor_id) + "): " +
                                 descriptor.substr(0, 50) + "... [" + descriptor_type + ", no xpub]");
                }

            } catch (const std::exception& e) {
                result["success"] = false;
                Json::Value error;
                error["code"] = -5;
                error["message"] = e.what();
                result["error"] = error;
                g_logger.warning("Failed to import descriptor: " + std::string(e.what()));
            }

            results.append(result);
        }

        return results;

    } catch (const std::exception& e) {
        Json::Value response;
        Json::Value error;
        error["code"] = -1;
        error["message"] = std::string("Failed to import descriptors: ") + e.what();
        response["error"] = error;
        g_logger.error("wallet.importdescriptors error: " + std::string(e.what()));
        return response;
    }
}

} // namespace dinero
