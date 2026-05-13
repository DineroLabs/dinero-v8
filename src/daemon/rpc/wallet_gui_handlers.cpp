#include "daemon/rpc/wallet_gui_handlers.h"
#include "address/addr_codec.h"
#include "crypto/hd_keychain.h"
#include "crypto/hash.h"
#include "wallet/wallet_manager.h"
#include "wallet/taproot_keys.h" // Canonical TapTweak (ComputeTweakedPubkey)
#include "wallet/bip39.h"
#include "common/logger.h"
#include "consensus/coin_type.h"
#include "storage/chain_direct.h"     // For g_chain_db_direct (birthday height)
#include "external/bech32/bech32.hpp"  // For address decoding
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <openssl/crypto.h>  // For OPENSSL_cleanse
#include <openssl/rand.h>    // For secure random generation
#include <openssl/sha.h>     // For TapTweak tagged hash
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <sqlite3.h>         // For wallet policy database operations

// Wallet encryption functions (Argon2id + AES-256-GCM)
namespace dinero {
namespace crypto {
    bool deriveKeyArgon2id(
        const std::string& password,
        const std::vector<uint8_t>& salt,
        int iterations,
        int memory_kb,
        int parallelism,
        std::array<uint8_t, 32>& output
    );

    std::vector<uint8_t> encryptAesGcm(
        const std::vector<uint8_t>& plaintext,
        const std::array<uint8_t, 32>& key,
        const std::vector<uint8_t>& nonce
    );

    std::vector<uint8_t> decryptAesGcm(
        const std::vector<uint8_t>& ciphertext,
        const std::array<uint8_t, 32>& key,
        const std::vector<uint8_t>& nonce
    );
}
}

namespace {

// Thin adapter — delegates to canonical TaprootKeys::ComputeTweakedPubkey
bool ComputeTaprootOutputKeyForRpc(const std::vector<uint8_t>& internal_xonly,
                                   std::array<uint8_t, 32>& output_key) {
    if (internal_xonly.size() != 32) {
        return false;
    }
    std::array<uint8_t, 32> internal_arr;
    std::copy(internal_xonly.begin(), internal_xonly.end(), internal_arr.begin());
    return dinero::TaprootKeys::ComputeTweakedPubkey(internal_arr, output_key);
}

std::optional<std::string> DeriveBip86FirstAddressFromSeed(const std::vector<uint8_t>& seed) {
    if (seed.size() != 64) {
        return std::nullopt;
    }

    constexpr uint32_t HARDENED = 0x80000000;
    constexpr uint32_t DINERO_COIN_TYPE = dinero::consensus::DINERO_COIN_TYPE;

    auto master_key = dinero::crypto::HDKeychain::fromSeed(seed);
    auto purpose_key = master_key.derive(86 | HARDENED);                // 86'
    auto coin_key = purpose_key.derive(DINERO_COIN_TYPE | HARDENED);    // 1448'
    auto account_key = coin_key.derive(0 | HARDENED);                   // 0'
    auto chain_key = account_key.derive(0);                             // external chain
    auto first_key = chain_key.derive(0);                               // index 0

    auto pubkey = first_key.getPublicKey();
    if (pubkey.size() != 33) {
        return std::nullopt;
    }

    std::vector<uint8_t> xonly_pubkey(pubkey.begin() + 1, pubkey.end());
    std::array<uint8_t, 32> output_key{};
    if (!ComputeTaprootOutputKeyForRpc(xonly_pubkey, output_key)) {
        return std::nullopt;
    }

    std::string hrp = dinero::HrpForActiveNetworkRef();
    if (hrp.empty()) {
        hrp = "din";
    }

    std::vector<uint8_t> witness_program(output_key.begin(), output_key.end());
    std::string address = bech32::Encode(hrp, 1, witness_program, bech32::Encoding::BECH32M);
    if (address.empty()) {
        return std::nullopt;
    }

    return address;
}

std::optional<uint32_t> DeriveMasterFingerprintFromSeed(const std::vector<uint8_t>& seed) {
    if (seed.size() != 64) {
        return std::nullopt;
    }

    auto master_key = dinero::crypto::HDKeychain::fromSeed(seed);
    auto master_pubkey = master_key.getPublicKey();
    auto fp_hash = din::crypto::HASH160(master_pubkey.data(), master_pubkey.size());

    uint32_t fingerprint =
        (static_cast<uint32_t>(fp_hash[0]) << 24) |
        (static_cast<uint32_t>(fp_hash[1]) << 16) |
        (static_cast<uint32_t>(fp_hash[2]) << 8) |
        static_cast<uint32_t>(fp_hash[3]);
    return fingerprint;
}

bool PersistWalletPolicyInOpenDb(sqlite3* db,
                                 const std::string& wallet_name,
                                 const std::string& policy,
                                 std::string* error_out) {
    if (!db) {
        if (error_out) {
            *error_out = "database handle is null";
        }
        return false;
    }

    auto set_error = [&](const std::string& msg) {
        if (error_out) {
            *error_out = msg;
        }
    };

    char* exec_err = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &exec_err) != SQLITE_OK) {
        std::string err = exec_err ? exec_err : sqlite3_errmsg(db);
        sqlite3_free(exec_err);
        set_error("BEGIN IMMEDIATE failed: " + err);
        return false;
    }

    auto rollback = [&]() {
        char* rb_err = nullptr;
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, &rb_err);
        sqlite3_free(rb_err);
    };

    sqlite3_stmt* stmt = nullptr;
    const char* upsert_sql = R"(
        INSERT INTO wallet_meta (id, name, network, wallet_policy)
        VALUES (1, ?, 'mainnet', ?)
        ON CONFLICT(id) DO UPDATE SET wallet_policy = excluded.wallet_policy
    )";

    if (sqlite3_prepare_v2(db, upsert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db);
        rollback();
        set_error("prepare wallet_policy upsert failed: " + err);
        return false;
    }

    const std::string safe_wallet_name = wallet_name.empty() ? "default" : wallet_name;
    sqlite3_bind_text(stmt, 1, safe_wallet_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, policy.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(db);
        rollback();
        set_error("wallet_policy upsert failed: " + err);
        return false;
    }

    stmt = nullptr;
    const char* verify_sql = "SELECT wallet_policy FROM wallet_meta WHERE id = 1 LIMIT 1";
    if (sqlite3_prepare_v2(db, verify_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db);
        rollback();
        set_error("prepare wallet_policy verify failed: " + err);
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        rollback();
        set_error("wallet_policy verify failed: wallet_meta row id=1 missing");
        return false;
    }

    const char* policy_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string persisted_policy = policy_cstr ? policy_cstr : "";
    sqlite3_finalize(stmt);

    if (persisted_policy != policy) {
        rollback();
        set_error("wallet_policy verify mismatch: expected '" + policy + "', got '" + persisted_policy + "'");
        return false;
    }

    exec_err = nullptr;
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &exec_err) != SQLITE_OK) {
        std::string err = exec_err ? exec_err : sqlite3_errmsg(db);
        sqlite3_free(exec_err);
        rollback();
        set_error("COMMIT failed while persisting wallet_policy: " + err);
        return false;
    }

    return true;
}

bool PersistWalletPolicyWithRetry(dinero::WalletManager* wallet_manager,
                                  const std::string& wallet_name,
                                  const std::string& policy,
                                  const std::string& context,
                                  std::string* error_out) {
    std::string first_error;
    sqlite3* db = wallet_manager->getCurrentDatabase();
    if (PersistWalletPolicyInOpenDb(db, wallet_name, policy, &first_error)) {
        dinero::g_logger.info("[" + context + "] Persisted wallet_policy='" + policy + "'");
        return true;
    }

    dinero::g_logger.warning("[" + context + "] wallet_policy persistence failed on first attempt: " + first_error);

    try {
        wallet_manager->open(wallet_name);
    } catch (const std::exception& e) {
        std::string combined = first_error + "; reopen failed: " + std::string(e.what());
        if (error_out) {
            *error_out = combined;
        }
        return false;
    }

    std::string retry_error;
    db = wallet_manager->getCurrentDatabase();
    if (PersistWalletPolicyInOpenDb(db, wallet_name, policy, &retry_error)) {
        dinero::g_logger.info("[" + context + "] Persisted wallet_policy after reopen retry");
        return true;
    }

    const std::string final_error = retry_error.empty() ? first_error : retry_error;
    if (error_out) {
        *error_out = final_error;
    }
    return false;
}

bool ResetWalletEncryptionState(dinero::WalletManager* wallet_manager,
                                const std::string& wallet_name,
                                std::string* error_out) {
    if (!wallet_manager) {
        if (error_out) {
            *error_out = "wallet manager is null";
        }
        return false;
    }

    sqlite3* db = wallet_manager->getCurrentDatabase();
    if (!db) {
        if (error_out) {
            *error_out = "wallet database is not open";
        }
        return false;
    }

    try {
        // Clear legacy settings used by wallet.unlock verification.
        wallet_manager->setSetting("wallet_encrypted", "");
        wallet_manager->setSetting("wallet_salt", "");
        wallet_manager->setSetting("wallet_verify_hash", "");
    } catch (const std::exception& e) {
        if (error_out) {
            *error_out = std::string("failed to clear encryption settings: ") + e.what();
        }
        return false;
    }

    const char* sql = R"(
        INSERT INTO encryption_metadata (
            id, encrypted, kdf, cipher, created_at, updated_at
        )
        VALUES (1, 0, 'none', 'none', strftime('%s','now'), strftime('%s','now'))
        ON CONFLICT(id) DO UPDATE SET
            encrypted = 0,
            kdf = 'none',
            cipher = 'none',
            updated_at = strftime('%s','now')
    )";

    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        if (error_out) {
            *error_out = "failed to update encryption_metadata: " + msg;
        }
        return false;
    }

    // Re-open to refresh in-memory wallet_encrypted_/wallet_locked_ flags.
    try {
        wallet_manager->open(wallet_name);
    } catch (const std::exception& e) {
        if (error_out) {
            *error_out = std::string("failed to reopen wallet after reset: ") + e.what();
        }
        return false;
    }

    return true;
}

void TryRollbackWalletCreate(dinero::WalletManager* wallet_manager, const std::string& wallet_name) {
    try {
        const std::string rollback_wallet = wallet_name + "__rollback_tmp";
        if (!wallet_manager->exists(rollback_wallet)) {
            wallet_manager->create(rollback_wallet);
        }
        wallet_manager->open(rollback_wallet);
        wallet_manager->remove(wallet_name);
    } catch (const std::exception& e) {
        dinero::g_logger.warning("[RpcRestoreWallet] Failed to rollback wallet: " + std::string(e.what()));
    }
}

}  // namespace

namespace dinero::rpc {

din::Json RpcCreateHDWallet(const din::Json& params, dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            result["error"] = "Wallet manager not available";
            return result;
        }
        
        // Parse parameters
        // POSITIONAL PARAMETERS (from CLI):
        //   params[0] = wallet_name (string, REQUIRED)
        //   params[1] = word_count (int, optional, default: 12)
        //   params[2] = bip39_passphrase (string, optional)
        //   params[3] = encryption_password (string, optional)
        //   params[4] = policy (string, optional, default: "bip86")

        std::string wallet_name = "default";  // Fallback if not provided
        int word_count = 12;  // Default to 12 words (128 bits entropy)
        std::string bip39_passphrase = "";
        std::string encryption_password = "";
        std::string policy = "bip86";  // Default to BIP86 Taproot
        bool replace_existing = false;

        if (params.isArray()) {
            // First parameter MUST be wallet name
            if (params.size() > 0 && params[0].isString()) {
                wallet_name = params[0].asString();
            }
            if (params.size() > 1 && params[1].isInt()) {
                word_count = params[1].asInt();
            }
            if (params.size() > 2 && params[2].isString()) {
                bip39_passphrase = params[2].asString();
            }
            if (params.size() > 3 && params[3].isString()) {
                encryption_password = params[3].asString();
            }
            if (params.size() > 4 && params[4].isString()) {
                policy = params[4].asString();
            }
            if (params.size() > 5) {
                if (params[5].isBool()) {
                    replace_existing = params[5].asBool();
                } else if (params[5].isString() && params[5].asString() == "true") {
                    replace_existing = true;
                }
            }
        } else if (params.isObject()) {
            if (params.isMember("name")) wallet_name = params["name"].asString();
            if (params.isMember("word_count")) word_count = params["word_count"].asInt();
            if (params.isMember("passphrase")) bip39_passphrase = params["passphrase"].asString();
            if (params.isMember("password")) encryption_password = params["password"].asString();
            if (params.isMember("policy")) policy = params["policy"].asString();
            if (params.isMember("replace_existing")) replace_existing = params["replace_existing"].asBool();
        }
        
        // Validate word count (12, 15, 18, 21, or 24)
        if (word_count != 12 && word_count != 15 && word_count != 18 && word_count != 21 && word_count != 24) {
            result["error"] = "Invalid word_count. Must be 12, 15, 18, 21, or 24";
            return result;
        }

        // Mainnet hardening: Taproot-only wallet policy from genesis.
        if (policy != "bip86") {
            result["error"] = "Invalid policy. Dinero mainnet only supports 'bip86' (Taproot from genesis)";
            return result;
        }

        // Create (if needed) and open target wallet before any DB writes.
        const bool wallet_exists = wallet_manager->exists(wallet_name);
        if (wallet_exists && !replace_existing) {
            result["error"] = "Wallet already exists: " + wallet_name;
            return result;
        }

        if (!wallet_exists) {
            wallet_manager->create(wallet_name);
        }
        wallet_manager->open(wallet_name);

        // Persist wallet_policy atomically on the active wallet DB.
        std::string policy_error;
        if (!PersistWalletPolicyWithRetry(wallet_manager, wallet_name, policy, "RpcCreateHDWallet", &policy_error)) {
            result["error"] = "Failed to persist wallet policy: " + policy_error;
            return result;
        }

        // Store birth_height = current chain tip for fast future restoration
        {
            uint32_t birth_height = 0;
            if (dinero::g_chain_db_direct) {
                auto tip_result = dinero::g_chain_db_direct->getTip();
                if (tip_result.status() == dinero::Status::Ok) {
                    birth_height = tip_result.value().height;
                }
            }
            sqlite3* wdb = wallet_manager->getCurrentDatabase();
            if (wdb) {
                const char* sql = "INSERT OR REPLACE INTO sync_meta (id, birth_height, gap_limit) VALUES (1, ?, 20)";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(wdb, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, static_cast<int>(birth_height));
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
            dinero::g_logger.info("Wallet birth_height set to " + std::to_string(birth_height));
        }

        // Generate BIP39 mnemonic
        dinero::bip39::WordCount wc = dinero::bip39::WordCount::Words12;
        switch (word_count) {
            case 15: wc = dinero::bip39::WordCount::Words15; break;
            case 18: wc = dinero::bip39::WordCount::Words18; break;
            case 21: wc = dinero::bip39::WordCount::Words21; break;
            case 24: wc = dinero::bip39::WordCount::Words24; break;
        }
        
        std::string mnemonic = dinero::bip39::Generate(wc);
        if (mnemonic.empty()) {
            result["error"] = "Failed to generate BIP39 mnemonic";
            return result;
        }
        
        // Convert mnemonic to seed (with optional BIP39 passphrase)
        std::vector<uint8_t> seed;
        if (!dinero::bip39::MnemonicToSeed(mnemonic, bip39_passphrase, seed)) {
            result["error"] = "Failed to convert mnemonic to seed";
            return result;
        }

        auto preflight_first_address = DeriveBip86FirstAddressFromSeed(seed);
        if (!preflight_first_address.has_value()) {
            result["error"] = "Failed to derive first BIP86 Taproot address from seed";
            return result;
        }

        auto master_fingerprint_opt = DeriveMasterFingerprintFromSeed(seed);
        if (!master_fingerprint_opt.has_value()) {
            result["error"] = "Failed to derive BIP32 master fingerprint";
            return result;
        }
        uint32_t master_fingerprint = master_fingerprint_opt.value();
        char fingerprint_hex[9];
        snprintf(fingerprint_hex, sizeof(fingerprint_hex), "%08X", master_fingerprint);
        std::string fingerprint(fingerprint_hex);

        // Generate first Taproot address for verification (BIP86-only policy)
        std::string first_address = preflight_first_address.value();

        // Encrypt wallet if password provided
        if (!encryption_password.empty()) {
            try {
                // === STEP 1: Generate random salt and nonce ===
                std::vector<uint8_t> salt(16);
                std::vector<uint8_t> nonce(12);

                if (RAND_bytes(salt.data(), salt.size()) != 1) {
                    result["error"] = "Failed to generate random salt";
                    return result;
                }

                if (RAND_bytes(nonce.data(), nonce.size()) != 1) {
                    result["error"] = "Failed to generate random nonce";
                    return result;
                }

                // === STEP 2: Derive encryption key from password using Argon2id ===
                // Parameters: OWASP 2023 recommendations
                // - Memory: 64 MB (65536 KB)
                // - Iterations: 3
                // - Parallelism: 1
                std::array<uint8_t, 32> encryption_key;
                if (!dinero::crypto::deriveKeyArgon2id(
                    encryption_password,
                    salt,
                    3,      // iterations
                    65536,  // 64 MB
                    1,      // parallelism
                    encryption_key
                )) {
                    result["error"] = "Failed to derive encryption key from password";
                    return result;
                }

                // === STEP 3: Encrypt the master seed using AES-256-GCM ===
                std::vector<uint8_t> encrypted_seed_with_tag =
                    dinero::crypto::encryptAesGcm(seed, encryption_key, nonce);

                // === STEP 4: Secure memory cleanup ===
                OPENSSL_cleanse(encryption_key.data(), encryption_key.size());
                OPENSSL_cleanse(seed.data(), seed.size());

                // === STEP 5: Store encryption metadata in wallet database ===
                // Call WalletManager to store:
                // - encrypted_seed_with_tag (ciphertext + 16-byte GCM tag)
                // - salt (16 bytes)
                // - nonce (12 bytes)
                // - KDF parameters (iterations, memory, parallelism)
                // - master_fingerprint

                if (!wallet_manager->storeEncryptedWallet(
                    wallet_name,
                    encrypted_seed_with_tag,
                    salt,
                    nonce,
                    3,      // argon2_iterations
                    65536,  // argon2_memory_kb
                    1,      // argon2_parallelism
                    master_fingerprint
                )) {
                    result["error"] = "Failed to store encrypted wallet metadata";
                    return result;
                }

                dinero::g_logger.info("✅ Wallet encrypted successfully: " + wallet_name);
                result["encrypted"] = true;

            } catch (const std::exception& e) {
                result["error"] = std::string("Wallet encryption failed: ") + e.what();
                return result;
            }
        } else {
            // Unencrypted wallet - still store metadata but mark as unencrypted
            if (!wallet_manager->storeUnencryptedWallet(
                wallet_name,
                seed,
                master_fingerprint
            )) {
                result["error"] = "Failed to store unencrypted wallet metadata";
                return result;
            }
            result["encrypted"] = false;
        }

        // Now that seed is stored in database, open the wallet
        // This allows master_seed to be auto-loaded correctly
        wallet_manager->open(wallet_name);

        // Persist canonical index-0 address via WalletManager so derivation metadata
        // is stored in DB (no sidecar files).
        std::string registered_address;
        {
            bool relock_after_registration = false;
            try {
                if (!encryption_password.empty()) {
                    wallet_manager->unlockWallet(encryption_password);
                    relock_after_registration = true;
                }

                registered_address = wallet_manager->getNewAddress("first", "taproot");

                if (relock_after_registration) {
                    wallet_manager->lockWallet();
                }
            } catch (...) {
                if (relock_after_registration) {
                    try {
                        wallet_manager->lockWallet();
                    } catch (...) {
                    }
                }
                throw;
            }
        }

        if (registered_address.empty()) {
            result["error"] = "Failed to register first address in wallet database";
            return result;
        }

        if (registered_address != first_address) {
            result["error"] = "Invariant violation: persisted first address diverges from seed-derived BIP86 address";
            result["expected_first_address"] = first_address;
            result["actual_first_address"] = registered_address;
            return result;
        }

        first_address = registered_address;
        dinero::g_logger.info("[RpcCreateHDWallet] ✅ First address registered with derivation path: " + first_address);

        // Return success with mnemonic (GUI expects "mnemonic" not "seed_phrase")
        result["success"] = true;
        result["mnemonic"] = mnemonic;  // GUI expects this field name
        result["fingerprint"] = fingerprint;
        result["first_address"] = first_address;
        result["word_count"] = word_count;
        result["wallet_name"] = wallet_name;
        result["policy"] = policy;  // BIP86
        
        dinero::g_logger.info("Created HD wallet: " + wallet_name + " with " + 
                             std::to_string(word_count) + " words");
        
    } catch (const std::exception& e) {
        result["error"] = std::string("HD wallet creation failed: ") + e.what();
        dinero::g_logger.error(result["error"].asString());
    }
    
    return result;
}

din::Json RpcRestoreWallet(const din::Json& params, dinero::WalletManager* wallet_manager) {
    din::Json result = din::obj();
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            result["error"] = "Wallet manager not available";
            return result;
        }
        
        // Parse parameters
        // POSITIONAL PARAMETERS (from CLI):
        //   params[0] = wallet_name (string, REQUIRED)
        //   params[1] = mnemonic (string, REQUIRED)
        //   params[2] = bip39_passphrase (string, optional)
        //   params[3] = encryption_password (string, optional)
        //   params[4] = policy (string, optional, default: "bip86")
        //   params[5] = expected_first_address (string, optional safety guard)

        std::string wallet_name = "default";  // Fallback if not provided
        std::string mnemonic;
        std::string bip39_passphrase = "";
        std::string encryption_password = "";
        std::string policy = "bip86";  // Default to BIP86 Taproot
        std::string expected_first_address = "";
        bool skip_checksum = false;
        bool replace_existing = false;
        bool wallet_created = false;

        if (params.isArray()) {
            // First parameter is wallet name
            if (params.size() > 0 && params[0].isString()) {
                wallet_name = params[0].asString();
            }
            // Second parameter is mnemonic
            if (params.size() > 1 && params[1].isString()) {
                mnemonic = params[1].asString();
            }
            if (params.size() > 2 && params[2].isString()) {
                bip39_passphrase = params[2].asString();
            }
            if (params.size() > 3 && params[3].isString()) {
                encryption_password = params[3].asString();
            }
            if (params.size() > 4 && params[4].isString()) {
                policy = params[4].asString();
            }
            if (params.size() > 5 && params[5].isString()) {
                expected_first_address = params[5].asString();
            }
            // params[6]: skip_checksum (bool or string "true") — bypass BIP39 checksum for recovery
            if (params.size() > 6) {
                if (params[6].isBool()) {
                    skip_checksum = params[6].asBool();
                } else if (params[6].isString() && params[6].asString() == "true") {
                    skip_checksum = true;
                }
            }
            if (params.size() > 7) {
                if (params[7].isBool()) {
                    replace_existing = params[7].asBool();
                } else if (params[7].isString() && params[7].asString() == "true") {
                    replace_existing = true;
                }
            }
        } else if (params.isObject()) {
            if (params.isMember("name")) wallet_name = params["name"].asString();
            if (params.isMember("mnemonic")) mnemonic = params["mnemonic"].asString();
            if (params.isMember("passphrase")) bip39_passphrase = params["passphrase"].asString();
            if (params.isMember("password")) encryption_password = params["password"].asString();
            if (params.isMember("policy")) policy = params["policy"].asString();
            if (params.isMember("expected_first_address")) {
                expected_first_address = params["expected_first_address"].asString();
            }
            if (params.isMember("skip_checksum")) skip_checksum = params["skip_checksum"].asBool();
            if (params.isMember("replace_existing")) replace_existing = params["replace_existing"].asBool();
        }
        
        // Validate mnemonic
        if (mnemonic.empty()) {
            result["error"] = "Mnemonic is required";
            return result;
        }

        if (!dinero::bip39::ValidateMnemonic(mnemonic)) {
            if (!skip_checksum) {
                result["error"] = "Invalid BIP39 mnemonic (checksum failed)";
                return result;
            }
            // Checksum bypass requested — warn but proceed with derivation.
            // BIP39 seed derivation (PBKDF2) works regardless of checksum validity.
            dinero::g_logger.warning("[RpcRestoreWallet] BIP39 checksum FAILED but skip_checksum=true — proceeding with recovery");
            result["checksum_warning"] = "BIP39 checksum is invalid — mnemonic may have a typo";
        }

        // Mainnet hardening: Taproot-only wallet policy from genesis.
        if (policy != "bip86") {
            result["error"] = "Invalid policy. Dinero mainnet only supports 'bip86' (Taproot from genesis)";
            return result;
        }

        // Step 1 (pre-DB invariant): Convert mnemonic to seed and derive first address
        // BEFORE creating/opening wallet DB state.
        std::vector<uint8_t> master_seed;
        if (!dinero::bip39::MnemonicToSeed(mnemonic, bip39_passphrase, master_seed, skip_checksum)) {
            result["error"] = "Failed to derive seed from mnemonic";
            return result;
        }

        const auto preflight_first_address = DeriveBip86FirstAddressFromSeed(master_seed);
        if (!preflight_first_address.has_value()) {
            result["error"] = "Failed to derive first BIP86 Taproot address from mnemonic";
            return result;
        }

        // Optional identity guard: fail before any wallet DB write if caller
        // supplied an expected address and it does not match mnemonic-derived index 0.
        if (!expected_first_address.empty() && preflight_first_address.value() != expected_first_address) {
            result["error"] = "Restored first address does not match expected_first_address (mnemonic/passphrase mismatch)";
            result["expected_first_address"] = expected_first_address;
            result["actual_first_address"] = preflight_first_address.value();
            return result;
        }

        // Create/open wallet in wallet manager
        const bool wallet_exists = wallet_manager->exists(wallet_name);
        if (wallet_exists && !replace_existing) {
            result["error"] = "Wallet already exists: " + wallet_name;
            return result;
        }

        if (!wallet_exists) {
            wallet_manager->create(wallet_name);
            wallet_created = true;
        }
        wallet_manager->open(wallet_name);

        std::string policy_error;
        if (!PersistWalletPolicyWithRetry(wallet_manager, wallet_name, policy, "RpcRestoreWallet", &policy_error)) {
            if (wallet_created) {
                TryRollbackWalletCreate(wallet_manager, wallet_name);
            }
            result["error"] = "Failed to persist wallet policy: " + policy_error;
            return result;
        }

        // Restore is authoritative for wallet identity. Clear any previous
        // encryption state so legacy passphrase metadata cannot survive across
        // seed replacement. The caller can then set a fresh passphrase.
        std::string encryption_reset_error;
        if (!ResetWalletEncryptionState(wallet_manager, wallet_name, &encryption_reset_error)) {
            if (wallet_created) {
                TryRollbackWalletCreate(wallet_manager, wallet_name);
            }
            result["error"] = "Failed to reset wallet encryption state: " + encryption_reset_error;
            return result;
        }
        
        // NOTE: We no longer use HDWallet::Restore - instead we convert mnemonic to seed
        // and use wallet_manager for ALL address derivation to ensure consistency

        // ═══════════════════════════════════════════════════════════════
        // CANONICAL FIX: Convert mnemonic to seed and store in wallet_manager
        // Then use getNewAddress() for proper persistence
        // ═══════════════════════════════════════════════════════════════

        // Step 2: Store seed in wallet_manager (required for getNewAddress to work)
        // NOTE: This sets master_seed_ member variable in WalletManager
        if (!wallet_manager->storeMasterSeed(master_seed, "")) {
            result["error"] = "Failed to store master seed in wallet";
            return result;
        }

        // Reset birthday to 0 so rescan covers full chain history.
        // The restored seed may have been used before this wallet was created.
        wallet_manager->setBirthdayHeight(0);

        // Step 3: Generate and persist addresses for rescan discovery.
        // BIP44 gap limit: derive 20 external (receive) + 20 internal (change) addresses
        // so that wallet.rescanblockchain can find all historical UTXOs.
        // Without this, only index-0 would be in watch_scripts and the rescan
        // would miss every UTXO sent to addresses at index 1, 2, 3, ...
        constexpr int kGapLimit = 20;
        std::vector<std::string> restored_addresses;

        // BIP86-only: generate Taproot addresses exclusively.
        std::string address_type = "taproot";

        // Generate and persist first address
        std::string first_address = wallet_manager->getNewAddress("", address_type);
        if (first_address.empty()) {
            result["error"] = "Failed to generate address 0";
            return result;
        }

        // Invariant: index-0 address generated from persisted wallet state must
        // match the mnemonic-derived preflight address.
        if (first_address != preflight_first_address.value()) {
            if (wallet_created) {
                TryRollbackWalletCreate(wallet_manager, wallet_name);
            }
            result["error"] = "Invariant violation: wallet-derived first address diverges from mnemonic-derived BIP86 address";
            result["expected_first_address"] = preflight_first_address.value();
            result["actual_first_address"] = first_address;
            return result;
        }
        restored_addresses.push_back(first_address);

        // Pre-derive remaining external (receive) addresses up to gap limit.
        // Each getNewAddress() call persists the address + watch_script to DB.
        for (int i = 1; i < kGapLimit; ++i) {
            std::string addr = wallet_manager->getNewAddress("", address_type);
            if (addr.empty()) {
                dinero::g_logger.warn("Restore: failed to derive external address at index " + std::to_string(i));
                break;
            }
            restored_addresses.push_back(addr);
        }

        // Pre-derive internal (change) addresses up to gap limit.
        for (int i = 0; i < kGapLimit; ++i) {
            std::string change_addr = wallet_manager->getNewChangeAddress("", address_type);
            if (change_addr.empty()) {
                dinero::g_logger.warn("Restore: failed to derive change address at index " + std::to_string(i));
                break;
            }
        }

        dinero::g_logger.info("Restore: derived " + std::to_string(restored_addresses.size()) +
                              " external + " + std::to_string(kGapLimit) + " change addresses for rescan discovery");

        // Compute fingerprint (first 12 chars of first address)
        std::string fingerprint = restored_addresses[0].substr(0, 12);

        // Encrypt wallet if password provided.
        if (!encryption_password.empty()) {
            wallet_manager->encryptWallet(encryption_password);
        }

        // Return success
        result["success"] = true;
        result["encrypted"] = !encryption_password.empty();
        result["fingerprint"] = fingerprint;
        result["first_address"] = restored_addresses[0];
        result["addresses_restored"] = static_cast<int>(restored_addresses.size());
        result["wallet_name"] = wallet_name;
        result["policy"] = policy;  // BIP86

        // Return all pre-derived addresses (gap limit coverage for rescan)
        din::Json addresses_array;
        for (const auto& addr : restored_addresses) {
            addresses_array.append(addr);
        }
        result["addresses"] = addresses_array;
        result["gap_limit"] = kGapLimit;

        dinero::g_logger.info("✅ Restored HD wallet: " + wallet_name + " from mnemonic (addresses persisted)");
        
    } catch (const std::exception& e) {
        result["error"] = std::string("Wallet restoration failed: ") + e.what();
        dinero::g_logger.error(result["error"].asString());
    }
    
    return result;
}

din::Json RpcExportMnemonic(const din::Json& params, dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            result["error"] = "Wallet manager not available";
            return result;
        }
        
        // Parse parameters
        std::string wallet_name = "default";  // Default to currently open wallet
        
        if (params.isArray() && params.size() > 0 && params[0].isString()) {
            wallet_name = params[0].asString();
        } else if (params.isObject() && params.isMember("name")) {
            wallet_name = params["name"].asString();
        }
        
        // Check if wallet exists
        if (!wallet_manager->exists(wallet_name)) {
            result["error"] = "Wallet not found: " + wallet_name;
            return result;
        }
        
        // Open wallet (open() handles already-open state)
        wallet_manager->open(wallet_name);
        
        result["error"] = "Mnemonic export is unavailable: runtime wallets do not persist mnemonic sidecar files.";
        result["suggestion"] = "Use your original offline seed backup from wallet creation/restore.";
        return result;
        
    } catch (const std::exception& e) {
        result["error"] = std::string("Mnemonic export failed: ") + e.what();
        dinero::g_logger.error(result["error"].asString());
    }
    
    return result;
}

} // namespace dinero::rpc
