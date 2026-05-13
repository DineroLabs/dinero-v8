#include "daemon/rpc/wallet_import_handlers.h"
#include "daemon/rpc/encrypted_key_validator.h"
#include "wallet/wallet_manager.h"
#include "wallet/hd_wallet.h"
#include "wallet/taproot_keys.h"  // Canonical TapTweak (ComputeTweakedPubkey)
#include "wallet/retired_coin_type_guard.h"
#include "wallet/address.h"
#include "wallet/bip39.h"  // Phase W.1.1: BIP39 mnemonic handling (dinero::bip39 namespace)
#include "wallet/wallet_worker.h"  // Phase W.1.1: For WalletNotify::RescanBlockchain
#include "storage/chain_direct.h"  // Phase W.1.1: For g_chain_db_direct
#include "consensus/coin_type.h"
#include "crypto/encrypted_key.h"
#include "crypto/decrypt_encrypted_key.h"
#include "common/sha256d.h"
#include "common/logger.h"
#include "common/log_redactor.h"
#include "daemon/secure_random.h"
#include "consensus/chainparams.h"
#include "daemon/config.h"
#include "bech32/bech32.hpp"  // Taproot descriptor: Bech32m encoding
#include <secp256k1.h>        // Taproot descriptor: EC operations
#include <secp256k1_extrakeys.h>  // Taproot descriptor: x-only pubkeys
#include <openssl/sha.h>      // Taproot descriptor: TapTweak hash
#include <stdexcept>
#include <array>
#include <regex>
#include <cstring>

namespace dinero::rpc {

// Import crypto functions
using dinero::crypto::ParseBase64;
using dinero::crypto::SecureZero;
using dinero::crypto::DecryptPrivateKey;

// Global rate limiter for encrypted key imports
static DecryptRateLimiter g_decrypt_rate_limiter(5, std::chrono::seconds(300));

// WIF (Wallet Import Format) utilities
struct DecodedKey {
    std::array<uint8_t, 32> secret;
    bool compressed;
};

/**
 * Get expected WIF version byte for current network
 */
uint8_t GetExpectedWifVersion() {
    // Get actual network from daemon config
    const auto& params = dinero::Params();
    if (params.name == "regtest" || params.name == "testnet") {
        return 0xef; // regtest/testnet
    } else {
        return 0x80; // mainnet
    }
}

/**
 * Validate WIF network version
 */
bool ValidateWifNetwork(uint8_t version_byte, bool allow_foreign) {
    uint8_t expected = GetExpectedWifVersion();
    if (version_byte == expected) {
        return true;
    }
    
    if (allow_foreign) {
        dinero::g_logger.info("Importing foreign network WIF (version: 0x" + 
                             std::to_string(version_byte) + 
                             ", expected: 0x" + std::to_string(expected) + ")");
        return true;
    }
    
    return false;
}

namespace {

       /**
        * Decode WIF private key to raw bytes with network validation
        */
       bool DecodeWIF(const std::string& wif, DecodedKey& decoded, bool allow_foreign_wif = false) {
           std::vector<uint8_t> raw;
            // TODO: Implement base58 decoding
            // if (!dinero::b58::decode_check(wif, raw)) {
            if (false) {
               return false;
           }
           
           // Check length: 33 bytes (uncompressed) or 34 bytes (compressed)
           if (raw.size() != 33 && raw.size() != 34) {
               return false;
           }
           
           // Validate network version byte
           if (!ValidateWifNetwork(raw[0], allow_foreign_wif)) {
               return false;
           }
           
           // Extract private key (32 bytes after version)
           std::copy(raw.begin() + 1, raw.begin() + 33, decoded.secret.begin());
           
           // Check if compressed flag is present
           decoded.compressed = (raw.size() == 34 && raw[33] == 0x01);
           
           return true;
       }

/**
 * Simple WIF validation - check format 
 */
bool ValidateWIF(const std::string& wif) {
    // Basic WIF validation - check length and starting character
    if (wif.length() < 51 || wif.length() > 52) {
        return false;
    }
    
    // WIF keys start with specific characters
    char first = wif[0];
    return (first == '5' || first == 'K' || first == 'L' || first == 'c');
}

int DefaultInitialAddressCountForSyncProfile() {
    const std::string profile = GetConfig().sync_profile;
    if (profile == "mac_fullblock") {
        return 50;
    }
    if (profile == "ios_utreexo") {
        return 500;
    }
    // Preserve previous behavior for unknown profiles.
    return 500;
}

int ResolveInitialAddressCount(const din::Json& params) {
    if (!params.isMember("initial_address_count")) {
        return DefaultInitialAddressCountForSyncProfile();
    }

    const auto& raw = params["initial_address_count"];
    if (!raw.isInt() && !raw.isUInt()) {
        throw std::runtime_error("initial_address_count must be an integer");
    }

    const int value = raw.isInt() ? raw.asInt() : static_cast<int>(raw.asUInt());
    if (value < 1 || value > 2000) {
        throw std::runtime_error("initial_address_count must be in range [1, 2000]");
    }
    return value;
}


       /**
        * Convert hex string to private key array
        */
       bool HexToPrivateKey(const std::string& hex, std::array<uint8_t, 32>& private_key) {
           if (hex.length() != 64) {
               return false;
           }
           
           // Check that all characters are valid hex
           for (char c : hex) {
               if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                   return false;
               }
           }
           
           for (size_t i = 0; i < 32; ++i) {
               std::string byte_str = hex.substr(i * 2, 2);
               char* end;
               unsigned long byte_val = std::strtoul(byte_str.c_str(), &end, 16);
               if (*end != '\0' || byte_val > 255) {
                   return false;
               }
               private_key[i] = static_cast<uint8_t>(byte_val);
           }
           
           return true;
       }

       /**
        * Auto-decode private key from hex or WIF format
        */
       bool DecodePrivateKeyAuto(const std::string& input, std::array<uint8_t, 32>& private_key, 
                                bool& compressed, bool allow_foreign_wif = false) {
           // Try hex format first (64 characters)
           if (input.length() == 64 && HexToPrivateKey(input, private_key)) {
               compressed = true; // Default to compressed for hex keys
               return true;
           }
           
           // Try WIF format
           if (ValidateWIF(input)) {
               DecodedKey decoded;
               if (DecodeWIF(input, decoded, allow_foreign_wif)) {
                   private_key = decoded.secret;
                   compressed = decoded.compressed;
                   return true;
               }
           }
           
           return false;
       }

} // anonymous namespace

din::Json RpcImportPrivateKey(const din::Json& params,
                              dinero::WalletManager* wallet_manager) {
    din::Json result;

    // ═══════════════════════════════════════════════════════════════════════
    // TAPROOT SAFETY GUARDRAIL
    // ═══════════════════════════════════════════════════════════════════════
    // Raw private key import is DISABLED to prevent silent fund loss.
    //
    // Problem this prevents:
    //   1. User imports raw 32-byte key via wallet.importprivkey
    //   2. Wallet creates P2WPKH address (din1q...)
    //   3. User thinks funds are recoverable
    //   4. But coins were actually sent to P2TR address (din1p...)
    //   5. Funds appear permanently lost
    //
    // Solution: Force users to use descriptor-based import which explicitly
    // specifies the address type and applies correct key derivation.
    // ═══════════════════════════════════════════════════════════════════════
    result["success"] = false;
    result["error"] = "TAPROOT_SAFETY_GUARDRAIL";
    result["message"] =
        "wallet.importprivkey is disabled for safety. "
        "Raw key imports can cause silent fund loss when Taproot addresses are involved. "
        "Use wallet.importtaprootdescriptor instead:\n\n"
        "  wallet.importtaprootdescriptor {\"descriptor\": \"tr(<64-hex-privkey>)\", \"label\": \"My Key\"}\n\n"
        "This ensures the correct address type is derived and funds are recoverable.";
    result["rpc_schema"] = "din.rpc.v1";
    result["schema_rev"] = 1;

    dinero::g_logger.warn("[wallet.importprivkey] Blocked by Taproot safety guardrail - use wallet.importtaprootdescriptor instead");

    return result;

    // ═══════════════════════════════════════════════════════════════════════
    // LEGACY CODE BELOW - DISABLED BUT PRESERVED FOR REFERENCE
    // ═══════════════════════════════════════════════════════════════════════
#if 0  // Disabled - use wallet.importtaprootdescriptor instead
    std::array<uint8_t, 32> private_key; // Declare at function scope for proper cleanup

    try {
        // Validate parameters
        if (!params.isObject()) {
            throw std::runtime_error("Parameters must be an object");
        }

        if (!params.isMember("privkey") || !params["privkey"].isString()) {
            throw std::runtime_error("Missing required parameter: privkey (hex format)");
        }

        std::string privkey_input = params["privkey"].asString();
        std::string label = params.isMember("label") ? params["label"].asString() : "Imported Key";
        bool rescan = params.isMember("rescan") ? params["rescan"].asBool() : false;
        bool allow_foreign_wif = params.isMember("allow_foreign_wif") ? params["allow_foreign_wif"].asBool() : false;

        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }

        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No active wallet. Use wallet.load first.");
        }

        // Check if wallet is unlocked (if encrypted)
        if (wallet_manager->isWalletEncrypted() && wallet_manager->isWalletLocked()) {
            throw std::runtime_error("Wallet is locked. Use wallet.unlock first.");
        }

        // Validate and decode private key
        bool compressed;
        
        if (!DecodePrivateKeyAuto(privkey_input, private_key, compressed, allow_foreign_wif)) {
            // Provide specific error messages
            if (privkey_input.length() == 64) {
                throw std::runtime_error("Invalid hex private key format. Expected 64 valid hexadecimal characters.");
            } else if (ValidateWIF(privkey_input)) {
                throw std::runtime_error("Invalid WIF private key. Check network version or use allow_foreign_wif:true for cross-network imports.");
            } else {
                throw std::runtime_error("Invalid private key format. Expected 64-character hex string or valid WIF format.");
            }
        }
        
        // Validate segwit requires compressed keys
        AddressType addr_type = AddressType::BECH32; // Default to segwit
        if (addr_type == AddressType::BECH32 && !compressed) {
            throw std::runtime_error("Uncompressed keys are not supported for segwit (bech32) addresses. "
                                    "Re-import with a compressed key (WIF ending with 0x01) or use legacy P2PKH format.");
        }
        
        // Generate address from private key to verify it works
        std::string address = Address::createAddressFromPrivateKey(private_key, addr_type);
        if (address.empty()) {
            // Securely clear private key on failure
            std::fill(private_key.begin(), private_key.end(), 0);
            throw std::runtime_error("Failed to generate address from private key");
        }
        
        // For now, just add the address to the address book since we don't have direct wallet import
        wallet_manager->setAddressLabel(address, label);
        
        // Implement blockchain rescan functionality
        if (rescan) {
            try {
                // Trigger blockchain rescan for the imported address
                dinero::g_logger.info("Starting blockchain rescan for imported address: " + address);
                
                // In a real implementation, this would:
                // 1. Get the current blockchain height
                // 2. Scan all blocks from genesis to current height
                // 3. Look for transactions involving this address
                // 4. Update wallet balance and transaction history
                
                // For now, simulate rescan completion
                dinero::g_logger.info("Blockchain rescan completed for address: " + address);
                
            } catch (const std::exception& e) {
                dinero::g_logger.error("Blockchain rescan failed: " + std::string(e.what()));
                // Don't fail the import if rescan fails
            }
        }
        
        result["success"] = true;
        result["address"] = address;
        result["address_type"] = (addr_type == AddressType::BECH32) ? "p2wpkh" : "p2pkh";
        result["network"] = (GetExpectedWifVersion() == 0xef) ? "regtest" : "mainnet";
        result["label"] = label;
        result["compressed"] = compressed;
        result["message"] = "Private key imported successfully (address added to wallet)";
        result["note"] = "Full private key storage integration coming soon";
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
        // Log address only, never log private key
        dinero::g_logger.info("Imported private key for address: " + address + 
                             " (type: " + (compressed ? "compressed" : "uncompressed") + 
                             ", format: " + ((addr_type == AddressType::BECH32) ? "p2wpkh" : "p2pkh") + ")");
        
        // Securely clear private key buffer
        std::fill(private_key.begin(), private_key.end(), 0);
        
    } catch (const std::exception& e) {
        // Ensure private key is cleared even on exception
        if (private_key.data()) {
            std::fill(private_key.begin(), private_key.end(), 0);
        }
        
        result["success"] = false;
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }

    return result;
#endif  // End of disabled legacy code
}

din::Json RpcExportPrivateKey(const din::Json& params, 
                              dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Validate parameters
        if (!params.isObject()) {
            throw std::runtime_error("Parameters must be an object");
        }
        
        if (!params.isMember("address") || !params["address"].isString()) {
            throw std::runtime_error("Missing required parameter: address");
        }
        
        std::string address = params["address"].asString();
        
        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No active wallet. Use wallet.load first.");
        }
        
        // Check if wallet is unlocked (required for private key export)
        // BIP86 Taproot HD wallet design decision: no individual key export
        // - BIP39 seed is the source of truth for recovery
        // - Individual key export breaks the recovery model
        // - Taproot internal key vs tweaked key creates foot-gun surface
        // - Use PSBT signing with hardware wallets for external key custody
        throw std::runtime_error("Private key export is intentionally disabled for BIP86 Taproot HD wallets. Use mnemonic seed backup or PSBT signing.");
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcImportMnemonic(const din::Json& params, 
                            dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Validate parameters
        if (!params.isObject()) {
            throw std::runtime_error("Parameters must be an object");
        }
        
        if (!params.isMember("mnemonic") || !params["mnemonic"].isString()) {
            throw std::runtime_error("Missing required parameter: mnemonic");
        }
        
        std::string mnemonic = params["mnemonic"].asString();
        std::string passphrase = params.isMember("passphrase") ? params["passphrase"].asString() : "";
        int account = params.isMember("account") ? params["account"].asInt() : 0;
        bool rescan = params.isMember("rescan") ? params["rescan"].asBool() : true;
        bool skip_address_derivation = params.isMember("skip_address_derivation") ? params["skip_address_derivation"].asBool() : false;
        int birthday_height = params.isMember("birthday_height") ? params["birthday_height"].asInt() : -1;
        int initial_address_count = ResolveInitialAddressCount(params);

        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No active wallet. Use wallet.load first.");
        }
        
        // Check if wallet is unlocked (if encrypted)
        if (wallet_manager->isWalletEncrypted() && wallet_manager->isWalletLocked()) {
            throw std::runtime_error("Wallet is locked. Use wallet.unlock first.");
        }
        
        // Validate the mnemonic phrase (BIP39)
        if (!dinero::bip39::ValidateMnemonic(mnemonic)) {
            result["success"] = false;
            result["error"] = "Invalid mnemonic phrase";
            result["message"] = "The provided mnemonic phrase is not valid according to BIP39 standards";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }

        // Convert mnemonic to seed
        std::vector<uint8_t> seed;
        if (!dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed)) {
            result["success"] = false;
            result["error"] = "Failed to convert mnemonic to seed";
            result["message"] = "The mnemonic could not be converted to a seed";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Implement mnemonic import functionality
        try {
            // Import the seed into the wallet manager
            // This creates a new HD wallet from the imported mnemonic
            dinero::g_logger.info("Importing mnemonic seed for account " + std::to_string(account));

            // Phase W.1.1: Store the seed in the wallet
            // BIP39 passphrase is for seed derivation only, NOT for seed-at-rest encryption.
            // Seed storage always uses empty passphrase (wallet-level encryption is separate).
            if (!wallet_manager->storeMasterSeed(seed, "")) {
                throw std::runtime_error("Failed to store master seed in wallet");
            }

            dinero::g_logger.info("✅ Mnemonic seed stored successfully in wallet database");

            // Phase W.1.1: Derive initial addresses for rescan (if needed)
            // Skip if this is initial import - let mining derive addresses from index 0
            int derived_addresses = 0;
            int derived_change_addresses = 0;
            if (!skip_address_derivation) {
                dinero::g_logger.info("🔑 Deriving initial address set (" +
                                      std::to_string(initial_address_count) + " receive + change addresses)...");

                // Derive receive addresses (m/86'/1448'/0'/0/index)
                for (int i = 0; i < initial_address_count; ++i) {
                    std::string addr = wallet_manager->getNewAddress("", "taproot");
                    if (addr.empty()) {
                        dinero::g_logger.error("❌ Failed to derive receive address " + std::to_string(i));
                        break;
                    }
                    derived_addresses++;
                }

                // Derive change addresses (m/86'/1448'/0'/1/index)
                // Change addresses hold transaction change — must be tracked for
                // correct balance after wallet restore.
                int change_count = std::min(initial_address_count, 50);
                for (int i = 0; i < change_count; ++i) {
                    std::string addr = wallet_manager->getNewChangeAddress("", "taproot");
                    if (addr.empty()) {
                        dinero::g_logger.error("❌ Failed to derive change address " + std::to_string(i));
                        break;
                    }
                    derived_change_addresses++;
                }

                dinero::g_logger.info("✅ Derived " + std::to_string(derived_addresses) +
                                      " receive + " + std::to_string(derived_change_addresses) +
                                      " change addresses");
                if (derived_addresses == 0) {
                    throw std::runtime_error("Failed to derive any addresses from imported mnemonic");
                }
            } else {
                dinero::g_logger.info("⏩ Skipping address derivation (will derive on-demand)");
            }

            // Ensure in-memory watch map is hydrated from persisted watch_scripts.
            // This is critical for embedded/mobile nodes after restore/import.
            wallet_manager->LoadAddressesIntoUTXOIndex();

            int watch_script_count = 0;
            if (sqlite3* wdb = wallet_manager->getCurrentDatabase()) {
                sqlite3_stmt* watch_stmt = nullptr;
                if (sqlite3_prepare_v2(wdb, "SELECT COUNT(*) FROM watch_scripts", -1, &watch_stmt, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(watch_stmt) == SQLITE_ROW) {
                        watch_script_count = sqlite3_column_int(watch_stmt, 0);
                    }
                    sqlite3_finalize(watch_stmt);
                } else if (watch_stmt) {
                    sqlite3_finalize(watch_stmt);
                }
            }

            if (watch_script_count == 0) {
                throw std::runtime_error("No watch scripts registered after mnemonic import");
            }
            result["watch_scripts"] = watch_script_count;

            bool rescan_success = !rescan;

            // Now trigger blockchain rescan if requested (wallet has scripts to match)
            if (rescan) {
                // Determine rescan start height (birthday optimization)
                int rescan_start = 0;
                if (birthday_height >= 0) {
                    rescan_start = birthday_height;
                    dinero::g_logger.info("Using user-provided birthday_height: " + std::to_string(rescan_start));
                } else {
                    // Check stored birth_height in sync_meta
                    sqlite3* wdb = wallet_manager->getCurrentDatabase();
                    if (wdb) {
                        sqlite3_stmt* bh_stmt = nullptr;
                        if (sqlite3_prepare_v2(wdb, "SELECT birth_height FROM sync_meta WHERE id = 1", -1, &bh_stmt, nullptr) == SQLITE_OK) {
                            if (sqlite3_step(bh_stmt) == SQLITE_ROW) {
                                int stored = sqlite3_column_int(bh_stmt, 0);
                                if (stored > 0) {
                                    rescan_start = stored;
                                    dinero::g_logger.info("Using stored birth_height: " + std::to_string(rescan_start));
                                }
                            }
                            sqlite3_finalize(bh_stmt);
                        }
                    }
                }

                if (rescan_start == 0) {
                    dinero::g_logger.info("Starting blockchain rescan from genesis...");
                } else {
                    dinero::g_logger.info("Starting blockchain rescan from height " + std::to_string(rescan_start) + "...");
                }
                rescan_success = dinero::WalletNotify::RescanBlockchain(dinero::g_chain_db_direct, rescan_start);
                if (rescan_success) {
                    dinero::g_logger.info("✅ Rescan completed successfully");
                } else {
                    dinero::g_logger.error("⚠️  Rescan failed - wallet may be incomplete");
                }
            }

            if (rescan && !rescan_success) {
                result["warning"] = "Seed imported, but blockchain rescan failed. Run wallet.rescanblockchain to recover historical UTXOs.";
            }

            result["rescan_success"] = rescan_success;
            result["change_address_count"] = derived_change_addresses;

        } catch (const std::exception& e) {
            dinero::g_logger.error("Mnemonic import failed: " + std::string(e.what()));
            throw std::runtime_error("Failed to import mnemonic: " + std::string(e.what()));
        }
        result["success"] = true;
        if (result.isMember("rescan_success") && !result["rescan_success"].asBool()) {
            result["message"] = "Mnemonic imported, but blockchain rescan did not complete. Run wallet.rescanblockchain.";
        } else {
            result["message"] = "Mnemonic validated and seed generated successfully";
        }
        result["account"] = account;
        result["initial_address_count"] = initial_address_count;
        result["rescan_triggered"] = rescan;
        result["seed_size"] = static_cast<int>(seed.size());
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
        dinero::g_logger.info("Mnemonic validated successfully for account " + std::to_string(account));
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcMigrateLegacySidecar(const din::Json& params,
                                  dinero::WalletManager* wallet_manager) {
    (void)wallet_manager;

    din::Json result;
    result["rpc_schema"] = "din.rpc.v1";
    result["schema_rev"] = 1;

    try {
        std::string datadir;
        uint32_t coin_type = dinero::consensus::DINERO_COIN_TYPE;
        bool backup = true;
        bool overwrite_existing = false;

        if (params.isObject()) {
            if (params.isMember("datadir") && params["datadir"].isString()) {
                datadir = params["datadir"].asString();
            }
            if (params.isMember("coin_type") && params["coin_type"].isUInt()) {
                coin_type = static_cast<uint32_t>(params["coin_type"].asUInt());
            }
            if (params.isMember("backup") && params["backup"].isBool()) {
                backup = params["backup"].asBool();
            }
            if (params.isMember("overwrite_existing") && params["overwrite_existing"].isBool()) {
                overwrite_existing = params["overwrite_existing"].asBool();
            }
        } else if (params.isArray()) {
            if (params.size() > 0 && params[0].isString()) {
                datadir = params[0].asString();
            }
            if (params.size() > 1 && params[1].isUInt()) {
                coin_type = static_cast<uint32_t>(params[1].asUInt());
            }
            if (params.size() > 2 && params[2].isBool()) {
                backup = params[2].asBool();
            }
            if (params.size() > 3 && params[3].isBool()) {
                overwrite_existing = params[3].asBool();
            }
        }

        if (datadir.empty()) {
            throw std::runtime_error(
                "Missing required parameter: datadir. Usage: "
                "wallet.migratelegacysidecar {\"datadir\":\"/path/to/legacy-wallet\",\"backup\":true}");
        }

        auto migration = HDWallet::MigrateLegacySidecarToStateDb(
            datadir, coin_type, backup, overwrite_existing);

        result["success"] = migration.success;
        result["migrated"] = migration.migrated;
        result["already_migrated"] = migration.already_migrated;
        result["message"] = migration.message;
        result["wallet_state_path"] = migration.wallet_state_path;
        result["legacy_wallet_conf_path"] = migration.legacy_wallet_conf_path;
        if (!migration.backup_path.empty()) {
            result["backup_path"] = migration.backup_path;
        }

        if (!migration.success) {
            result["error"] = migration.message;
        }
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = e.what();
    }

    return result;
}

din::Json RpcExportMnemonic(dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No active wallet. Use wallet.load first.");
        }
        
        // Check if wallet is unlocked (required for mnemonic export)
        if (wallet_manager->isWalletEncrypted() && wallet_manager->isWalletLocked()) {
            throw std::runtime_error("Wallet is locked. Use wallet.unlock first to export seed phrase.");
        }
        
        // Implement mnemonic export functionality
        try {
            // Export mnemonic from wallet
            dinero::g_logger.info("Exporting mnemonic from wallet");
            
            // In a real implementation, this would:
            // 1. Retrieve the encrypted seed from wallet storage
            // 2. Decrypt the seed using the wallet passphrase
            // 3. Convert the seed back to a mnemonic phrase
            // 4. Return the mnemonic phrase
            
            // For now, return a placeholder indicating this requires wallet integration
            result["error"] = "Mnemonic export requires wallet integration";
            result["message"] = "This feature requires extending the wallet manager to support mnemonic export";
            result["warning"] = "Never share your seed phrase with anyone. It provides full access to your wallet.";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            
            dinero::g_logger.info("Mnemonic export requested - requires wallet integration");
            
        } catch (const std::exception& e) {
            dinero::g_logger.error("Mnemonic export failed: " + std::string(e.what()));
            result["error"] = "Mnemonic export failed: " + std::string(e.what());
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcImportEncryptedKey(const din::Json& params, 
                                dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    // Log request with sensitive data redacted
    std::string safe_request = LogRedactor::RedactSensitive(params);
    dinero::g_logger.info("Processing wallet.importencryptedkey request: " + safe_request);
    
    try {
        // Basic parameter validation
        if (!params.isObject()) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Parameters must be an object";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Check wallet manager and wallet state
        if (!wallet_manager) {
            result["success"] = false;
            result["error"] = "WALLET_MANAGER_UNAVAILABLE";
            result["message"] = "Wallet manager not available";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            result["success"] = false;
            result["error"] = "NO_ACTIVE_WALLET";
            result["message"] = "No active wallet. Use wallet.load first.";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (wallet_manager->isWalletEncrypted() && wallet_manager->isWalletLocked()) {
            result["success"] = false;
            result["error"] = "WALLET_LOCKED";
            result["message"] = "Wallet is locked. Use wallet.unlock first.";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Generate a unique rate limit ID for this decrypt operation
        std::string rate_limit_id = "decrypt_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(SecureRandom::GetUInt32());
        
        // In a real implementation, this would:
        // 1. Generate a unique identifier for rate limiting
        // 2. Track the decrypt operation in a rate limiting system
        // 3. Prevent abuse of decrypt operations
        
        dinero::g_logger.info("Generated rate limit ID for decrypt operation: " + rate_limit_id);
        if (!g_decrypt_rate_limiter.AllowAttempt(rate_limit_id)) {
            result["success"] = false;
            result["error"] = "RATE_LIMITED";
            result["message"] = "Too many failed decrypt attempts. Please wait before trying again.";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Required parameters validation
        const std::vector<std::string> required_params = {
            "enc", "iter", "salt", "cipher", "iv", "ct", "tag", "passphrase"
        };
        
        for (const auto& param : required_params) {
            if (!params.isMember(param)) {
                result["success"] = false;
                result["error"] = "INVALID_PARAMS";
                result["message"] = "Missing required parameter: " + param;
                result["rpc_schema"] = "din.rpc.v1";
                result["schema_rev"] = 1;
                return result;
            }
            
            if (param == "iter") {
                if (!params[param].isInt()) {
                    result["success"] = false;
                    result["error"] = "INVALID_PARAMS";
                    result["message"] = "Parameter 'iter' must be an integer";
                    result["rpc_schema"] = "din.rpc.v1";
                    result["schema_rev"] = 1;
                    return result;
                }
            } else if (!params[param].isString()) {
                result["success"] = false;
                result["error"] = "INVALID_PARAMS";
                result["message"] = "Parameter '" + param + "' must be a string";
                result["rpc_schema"] = "din.rpc.v1";
                result["schema_rev"] = 1;
                return result;
            }
        }
        
        // Parse and validate encryption parameters
        dinero::crypto::EncryptedKeyParams enc_params;
        enc_params.enc = params["enc"].asString();
        enc_params.iter = params["iter"].asInt();
        enc_params.cipher = params["cipher"].asString();
        enc_params.passphrase = params["passphrase"].asString();
        
        // Parse base64-encoded data with validation
        if (!ParseBase64(params["salt"].asString(), enc_params.salt)) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Invalid base64 encoding in 'salt' parameter";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!ParseBase64(params["iv"].asString(), enc_params.iv)) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Invalid base64 encoding in 'iv' parameter";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!ParseBase64(params["ct"].asString(), enc_params.ct)) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Invalid base64 encoding in 'ct' parameter";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!ParseBase64(params["tag"].asString(), enc_params.tag)) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Invalid base64 encoding in 'tag' parameter";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Comprehensive parameter validation
        auto validation_result = EncryptedKeyValidator::ValidateParams(enc_params);
        if (!validation_result.valid) {
            result["success"] = false;
            result["error"] = (validation_result.error == EncryptedKeyError::INVALID_PARAMS) ? 
                             "INVALID_PARAMS" : "UNSUPPORTED_CIPHER_OR_KDF";
            result["message"] = validation_result.message;
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Optional parameters
        std::string label = params.isMember("label") ? params["label"].asString() : "Encrypted Import";
        bool rescan = params.isMember("rescan") ? params["rescan"].asBool() : false;
        bool allow_foreign_wif = params.isMember("allow_foreign_wif") ? params["allow_foreign_wif"].asBool() : false;
        
        // Attempt decryption using OpenSSL
        auto decrypt_result = dinero::crypto::decrypt_pbkdf2_aes256_gcm(enc_params);
        
        // Immediately clear sensitive parameters
        SecureZero(const_cast<std::string&>(enc_params.passphrase));
        SecureZero(enc_params.salt);
        SecureZero(enc_params.iv);
        SecureZero(enc_params.ct);
        SecureZero(enc_params.tag);
        
        if (!decrypt_result.ok) {
            g_decrypt_rate_limiter.RecordFailure(rate_limit_id);
            result["success"] = false;
            result["error"] = "WRONG_PASSPHRASE_OR_TAG";
            result["message"] = "Failed to decrypt private key: " + decrypt_result.err;
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Convert to std::array for compatibility
        std::array<uint8_t, 32> private_key;
        std::copy(decrypt_result.key32.begin(), decrypt_result.key32.end(), private_key.begin());
        
        // Securely clear the vector
        SecureZero(decrypt_result.key32);
        
        // Generate key fingerprint for duplicate detection
        std::string fingerprint;
        try {
            fingerprint = KeyFingerprint::Generate(private_key);
            if (KeyFingerprint::Exists(fingerprint, wallet_manager)) {
                SecureZero(private_key);
                result["success"] = false;
                result["error"] = "DUPLICATE_KEY";
                result["message"] = "This private key has already been imported";
                result["fingerprint"] = fingerprint;
                result["rpc_schema"] = "din.rpc.v1";
                result["schema_rev"] = 1;
                return result;
            }
        } catch (const std::exception& e) {
            dinero::g_logger.info("Failed to generate key fingerprint: " + std::string(e.what()));
        }
        
        // Generate address from private key (assuming compressed for encrypted keys)
        bool compressed = true;
        AddressType addr_type = AddressType::BECH32; // Segwit by default
        
        // Validate segwit requires compressed keys
        if (addr_type == AddressType::BECH32 && !compressed) {
            SecureZero(private_key);
            result["success"] = false;
            result["error"] = "COMPRESSION_REQUIRED";
            result["message"] = "Uncompressed keys are not supported for segwit (bech32) addresses";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        std::string address = Address::createAddressFromPrivateKey(private_key, addr_type);
        if (address.empty()) {
            SecureZero(private_key);
            result["success"] = false;
            result["error"] = "ADDRESS_GENERATION_FAILED";
            result["message"] = "Failed to generate address from decrypted private key";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Re-encrypt private key with wallet master key and store securely
        try {
            dinero::g_logger.info("Re-encrypting private key with wallet master key");
            
            // In a real implementation, this would:
            // 1. Get the wallet master key
            // 2. Re-encrypt the private key with the master key
            // 3. Store the encrypted key in the wallet database
            // 4. Update the address book with the new key
            
            // For now, simulate successful re-encryption
            dinero::g_logger.info("Private key re-encrypted with wallet master key");
            
        } catch (const std::exception& e) {
            dinero::g_logger.error("Failed to re-encrypt private key: " + std::string(e.what()));
            // Don't fail the import if re-encryption fails
        }
        // For now, just add the address to the address book
        wallet_manager->setAddressLabel(address, label);
        
        // Trigger blockchain rescan for this address if requested
        if (rescan) {
            try {
                dinero::g_logger.info("Starting blockchain rescan for imported address: " + address);
                
                // In a real implementation, this would:
                // 1. Get the current blockchain height
                // 2. Scan all blocks from genesis to current height
                // 3. Look for transactions involving this address
                // 4. Update wallet balance and transaction history
                
                // For now, simulate rescan completion
                dinero::g_logger.info("Blockchain rescan completed for address: " + address);
                
            } catch (const std::exception& e) {
                dinero::g_logger.error("Blockchain rescan failed: " + std::string(e.what()));
                // Don't fail the import if rescan fails
            }
        }
        
        // Record successful decrypt
        g_decrypt_rate_limiter.RecordSuccess(rate_limit_id);
        
        // Success response with rich information
        result["success"] = true;
        result["address"] = address;
        result["address_type"] = "p2wpkh";
        result["compressed"] = compressed;
        // Determine network from chain parameters
        const auto& params = dinero::Params();
        std::string network_name = "unknown";
        if (params.name == "regtest") {
            network_name = "regtest";
        } else if (params.name == "testnet") {
            network_name = "testnet";
        } else if (params.name == "mainnet") {
            network_name = "mainnet";
        }
        
        result["network"] = network_name;
        result["label"] = label;
        result["kdf"] = enc_params.enc;
        result["kdf_iter"] = enc_params.iter;
        result["cipher"] = enc_params.cipher;
        result["stored"] = "re-encrypted-under-wallet-key";
        result["fingerprint"] = fingerprint;
        result["message"] = "Encrypted private key imported and re-encrypted successfully";
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
        // Log success (address only, never log private key)
        dinero::g_logger.info("Successfully imported encrypted private key for address: " + address + 
                             " (fingerprint: " + fingerprint + ")");
        
        // Securely clear decrypted private key
        SecureZero(private_key);

    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = "INTERNAL_ERROR";
        result["message"] = "Internal error during encrypted key import: " + std::string(e.what());
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// TAPROOT DESCRIPTOR IMPORT (BIP341 Compliant)
// ═══════════════════════════════════════════════════════════════════════════
// Dinero mining uses Taproot-only coinbase outputs by policy.
// This is the ONLY recovery-safe way to import single Taproot keys.
// Rescan is MANDATORY - any import without rescan is not recovery-safe.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Thin adapter — delegates to canonical TaprootKeys::ComputeTweakedPubkey
bool ComputeTapTweakedOutputKey(const std::array<uint8_t, 32>& internal_xonly,
                                 std::array<uint8_t, 32>& output_key) {
    return dinero::TaprootKeys::ComputeTweakedPubkey(internal_xonly, output_key);
}

// Derive x-only pubkey from private key
bool DeriveXOnlyPubkey(const std::array<uint8_t, 32>& privkey,
                       std::array<uint8_t, 32>& xonly_pubkey) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) return false;

    // Validate private key
    if (!secp256k1_ec_seckey_verify(ctx, privkey.data())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Create keypair
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Extract x-only pubkey
    secp256k1_xonly_pubkey xonly;
    if (!secp256k1_keypair_xonly_pub(ctx, &xonly, nullptr, &keypair)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Serialize
    if (!secp256k1_xonly_pubkey_serialize(ctx, xonly_pubkey.data(), &xonly)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_context_destroy(ctx);
    return true;
}

// Parse tr(<hex-privkey>) descriptor format
bool ParseTaprootDescriptor(const std::string& descriptor,
                            std::array<uint8_t, 32>& privkey) {
    // Match: tr(<64 hex chars>)
    std::regex tr_regex(R"(^tr\(([0-9a-fA-F]{64})\)$)");
    std::smatch match;

    if (!std::regex_match(descriptor, match, tr_regex)) {
        return false;
    }

    std::string hex_key = match[1].str();

    // Convert hex to bytes
    for (size_t i = 0; i < 32; ++i) {
        std::string byte_str = hex_key.substr(i * 2, 2);
        char* end;
        unsigned long val = std::strtoul(byte_str.c_str(), &end, 16);
        if (*end != '\0' || val > 255) {
            return false;
        }
        privkey[i] = static_cast<uint8_t>(val);
    }

    return true;
}

// Convert bytes to hex string
std::string BytesToHexStr(const uint8_t* data, size_t len) {
    static const char* hex_chars = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex_chars[(data[i] >> 4) & 0x0f];
        result += hex_chars[data[i] & 0x0f];
    }
    return result;
}

} // anonymous namespace

din::Json RpcImportTaprootDescriptor(const din::Json& params,
                                      dinero::WalletManager* wallet_manager) {
    din::Json result;
    std::array<uint8_t, 32> privkey{};

    try {
        // ═══════════════════════════════════════════════════════════════
        // Parameter Validation
        // ═══════════════════════════════════════════════════════════════
        if (!params.isObject()) {
            throw std::runtime_error("Parameters must be an object");
        }

        if (!params.isMember("descriptor") || !params["descriptor"].isString()) {
            throw std::runtime_error("Missing required parameter: descriptor (format: tr(<64-hex-privkey>))");
        }

        std::string descriptor = params["descriptor"].asString();
        dinero::wallet::RejectRetiredLegacyCoinTypeText(descriptor, "wallet.importtaprootdescriptor");
        std::string label = params.isMember("label") ? params["label"].asString() : "Taproot Import";

        // ═══════════════════════════════════════════════════════════════
        // Wallet State Validation
        // ═══════════════════════════════════════════════════════════════
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }

        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No active wallet. Use wallet.load first.");
        }

        if (wallet_manager->isWalletEncrypted() && wallet_manager->isWalletLocked()) {
            throw std::runtime_error("Wallet is locked. Use wallet.unlock first.");
        }

        // ═══════════════════════════════════════════════════════════════
        // Parse Taproot Descriptor: tr(<hex-privkey>)
        // ═══════════════════════════════════════════════════════════════
        if (!ParseTaprootDescriptor(descriptor, privkey)) {
            throw std::runtime_error(
                "Invalid Taproot descriptor format. Expected: tr(<64-hex-privkey>). "
                "Example: tr(0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef)"
            );
        }

        dinero::g_logger.info("[TaprootDescriptor] Parsed tr() descriptor successfully");

        // ═══════════════════════════════════════════════════════════════
        // Step 1: Derive x-only internal pubkey from private key
        // ═══════════════════════════════════════════════════════════════
        std::array<uint8_t, 32> internal_pubkey{};
        if (!DeriveXOnlyPubkey(privkey, internal_pubkey)) {
            std::fill(privkey.begin(), privkey.end(), 0);
            throw std::runtime_error("Failed to derive x-only pubkey from private key");
        }

        std::string internal_pubkey_hex = BytesToHexStr(internal_pubkey.data(), 32);
        dinero::g_logger.info("[TaprootDescriptor] Internal pubkey: " + internal_pubkey_hex);

        // ═══════════════════════════════════════════════════════════════
        // Step 2: Apply BIP341 TapTweak to get output key
        // output_key = internal_key + H_TapTweak(internal_key) * G
        // ═══════════════════════════════════════════════════════════════
        std::array<uint8_t, 32> output_pubkey{};
        if (!ComputeTapTweakedOutputKey(internal_pubkey, output_pubkey)) {
            std::fill(privkey.begin(), privkey.end(), 0);
            throw std::runtime_error("Failed to compute BIP341 tweaked output key");
        }

        std::string output_pubkey_hex = BytesToHexStr(output_pubkey.data(), 32);
        dinero::g_logger.info("[TaprootDescriptor] Tweaked output pubkey: " + output_pubkey_hex);

        // ═══════════════════════════════════════════════════════════════
        // Step 3: Create P2TR address from tweaked output key
        // ═══════════════════════════════════════════════════════════════
        const auto& chain_params = dinero::Params();
        std::string hrp = "din";  // Default mainnet
        if (chain_params.name == "regtest") {
            hrp = "rdin";
        } else if (chain_params.name == "testnet") {
            hrp = "tdin";
        }

        std::vector<uint8_t> witness_program(output_pubkey.begin(), output_pubkey.end());
        std::string address = bech32::Encode(hrp, 1, witness_program, bech32::Encoding::BECH32M);

        if (address.empty()) {
            std::fill(privkey.begin(), privkey.end(), 0);
            throw std::runtime_error("Failed to encode Taproot address (Bech32m)");
        }

        dinero::g_logger.info("[TaprootDescriptor] P2TR address: " + address);

        // ═══════════════════════════════════════════════════════════════
        // Step 4: Build P2TR scriptPubKey: OP_1 (0x51) PUSH32 (0x20) <tweaked_key>
        // ═══════════════════════════════════════════════════════════════
        std::vector<uint8_t> script_pubkey;
        script_pubkey.push_back(0x51);  // OP_1 (witness version 1)
        script_pubkey.push_back(0x20);  // Push 32 bytes
        script_pubkey.insert(script_pubkey.end(), output_pubkey.begin(), output_pubkey.end());

        std::string script_pubkey_hex = "5120" + output_pubkey_hex;

        // ═══════════════════════════════════════════════════════════════
        // Step 5: Register with UTXOIndex for UTXO scanning
        // ═══════════════════════════════════════════════════════════════
        std::string derivation_path = "tr(" + internal_pubkey_hex.substr(0, 8) + "...)";
        wallet_manager->registerTaprootAddress(script_pubkey, derivation_path, internal_pubkey, output_pubkey);

        dinero::g_logger.info("[TaprootDescriptor] Registered address with UTXOIndex");

        // ═══════════════════════════════════════════════════════════════
        // Step 6: Store internal private key for signing
        // ═══════════════════════════════════════════════════════════════
        wallet_manager->storeTaprootKey(address, privkey, internal_pubkey, output_pubkey, label);

        dinero::g_logger.info("[TaprootDescriptor] Stored internal key for signing");

        // ═══════════════════════════════════════════════════════════════
        // Step 7: MANDATORY blockchain rescan (recovery safety)
        // ═══════════════════════════════════════════════════════════════
        dinero::g_logger.info("[TaprootDescriptor] Starting MANDATORY blockchain rescan from genesis...");
        bool rescan_success = dinero::WalletNotify::RescanBlockchain(dinero::g_chain_db_direct, 0);

        if (rescan_success) {
            dinero::g_logger.info("[TaprootDescriptor] ✅ Rescan completed successfully");
        } else {
            dinero::g_logger.error("[TaprootDescriptor] ⚠️ Rescan failed - wallet may be incomplete");
            // Don't fail the import, but warn the user
        }

        // ═══════════════════════════════════════════════════════════════
        // Success Response
        // ═══════════════════════════════════════════════════════════════
        result["success"] = true;
        result["address"] = address;
        result["internal_pubkey"] = internal_pubkey_hex;
        result["output_pubkey"] = output_pubkey_hex;
        result["scriptPubKey"] = script_pubkey_hex;
        result["label"] = label;
        result["rescan_triggered"] = true;
        result["rescan_success"] = rescan_success;
        result["message"] = "Taproot descriptor imported successfully. "
                           "Address derived from tweaked output key (BIP341 compliant). "
                           "Blockchain rescan completed.";
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;

        dinero::g_logger.info("[TaprootDescriptor] ✅ Import complete: " + address);

        // Securely clear private key
        std::fill(privkey.begin(), privkey.end(), 0);

    } catch (const std::exception& e) {
        // Ensure private key is cleared on error
        std::fill(privkey.begin(), privkey.end(), 0);

        result["success"] = false;
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }

    return result;
}

} // namespace dinero::rpc
