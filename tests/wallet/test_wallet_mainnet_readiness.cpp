#include <gtest/gtest.h>
#include "crypto/sha256.h"

#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <sqlite3.h>
#ifdef _WIN32
#include <process.h>
#include <cstdlib>
#include <string>
#define getpid _getpid
static inline int dinero_setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static inline int dinero_unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#define setenv   dinero_setenv
#define unsetenv dinero_unsetenv
// MSVC has _mktemp_s, not mkdtemp. Provide a portable replacement.
#include <filesystem>
#include <chrono>
#include <atomic>
static inline char* mkdtemp(char* tmpl) {
    // POSIX mkdtemp replaces the last 6 X with a unique suffix
    // and creates the directory. We use pid + ns-timestamp + atomic
    // counter; same uniqueness, plus mkdir.
    static std::atomic<uint64_t> counter{0};
    size_t len = std::strlen(tmpl);
    if (len < 6) return nullptr;
    const auto pid = static_cast<unsigned long long>(_getpid());
    const auto ts = static_cast<unsigned long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%06llu",
                  (pid ^ ts ^ (unsigned long long)seq) & 0xFFFFFFULL);
    std::memcpy(tmpl + len - 6, buf, 6);
    std::error_code ec;
    if (!std::filesystem::create_directories(tmpl, ec)) return nullptr;
    return tmpl;
}
#else
#include <unistd.h>
#endif

#include <fstream>
#include <map>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include "address/addr_codec.h"
#include "consensus/subsidy.h"
#include "crypto/hd_keychain.h"
#include "daemon/rpc/wallet_gui_handlers.h"
#include "external/bech32/bech32.hpp"
#include "primitives/block.h"
#include "storage/chain_direct.h"
#include "util/hex.h"
#include "wallet/bip39.h"
#include "wallet/wallet_manager.h"
#include "wallet/v7_p2mr_store.h"
#include "wallet/p2mr_address.h"
#include "wallet/secure_keypair.h"
#include "consensus/pq/scheme_registry.h"

namespace fs = std::filesystem;

namespace dinero {
struct WalletInitialOwnerTestAccess {
    static bool Store(WalletManager& wallet, const std::vector<uint8_t>& seed, const std::string& owner) {
        return wallet.storeMasterSeedOwned(seed,"",false,&owner);
    }
};
struct WalletUnlockOwnerTestAccess {
    static void Expire(WalletManager& w) {
        std::lock_guard<std::recursive_mutex> lock(w.database_lifecycle_mutex_);
        w.unlock_time_=1;w.unlock_timeout_=1;
    }
    static bool Cleared(WalletManager& w) {
        std::lock_guard<std::recursive_mutex> lock(w.database_lifecycle_mutex_);
        return w.wallet_locked_ && w.encryption_key_.empty() && w.master_seed_.empty() &&
            w.private_key_cache_.empty() && !w.pq_master_key_loaded_ &&
            std::all_of(w.pq_master_key_.begin(),w.pq_master_key_.end(),[](uint8_t b){return b==0;}) &&
            w.unlock_time_==0 && w.unlock_timeout_==0;
    }
    static std::string State(WalletManager& w) {
        std::lock_guard<std::recursive_mutex> lock(w.database_lifecycle_mutex_);
        std::string bytes;
        struct Clear { std::string& value; ~Clear(){ if(!value.empty()) OPENSSL_cleanse(value.data(),value.size()); } } clear{bytes};
        const auto append=[&](const auto& v){ if(!v.empty()) bytes.append(reinterpret_cast<const char*>(v.data()),v.size()*sizeof(v[0])); };
        append(w.encryption_key_); append(w.master_seed_); append(w.pq_master_key_);
        append(w.shielded_incoming_viewing_keys_); append(w.shielded_outgoing_viewing_keys_);
        append(w.shielded_recipient_viewing_authorities_);
        for(const auto& item:w.private_key_cache_){bytes+=item.first;append(item.second);}
        std::array<uint8_t,32> hash{}; ::SHA256(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size(),hash.data());
        return std::to_string(w.wallet_locked_)+":"+std::to_string(w.pq_master_key_loaded_)+":"+
            std::to_string(w.unlock_time_)+":"+std::to_string(w.unlock_timeout_)+":"+util::hex(std::vector<uint8_t>(hash.begin(),hash.end()));
    }
    static std::string LegacyPayload(WalletManager& w, uint8_t last, bool raw) {
        std::vector<uint8_t> scalar(32,0);scalar.back()=last;
        const std::string plain=raw?std::string(scalar.begin(),scalar.end()):util::hex(scalar);
        // Exercise the historical binary-in-TEXT representation with an
        // embedded NUL, using explicit-length SQL binding in the fixture.
        for(int i=0;i<512;++i){auto encrypted=w.encryptData(plain,w.encryption_key_);if(encrypted.find('\0')!=std::string::npos)return encrypted;}
        throw std::runtime_error("fixture did not produce binary ciphertext");
    }
    static std::string DifferentPqWrapper(WalletManager& w) {
        std::string key=w.deriveKey("password",std::string(32,'s'));
        std::string plain(32,'x');const auto encrypted=w.encryptData(plain,key);
        OPENSSL_cleanse(key.data(),key.size());OPENSSL_cleanse(plain.data(),plain.size());
        return util::hex(std::vector<uint8_t>(encrypted.begin(),encrypted.end()));
    }
    static void UseKnownCredentials(WalletManager& w, bool legacy=false) {
        const std::string salt(32,'s');std::string key=legacy?w.deriveKeyLegacy("password",salt):w.deriveKey("password",salt);
        std::array<uint8_t,32> hash{};::SHA256(reinterpret_cast<const uint8_t*>(key.data()),key.size(),hash.data());
        w.setSetting("wallet_salt",util::hex(std::vector<uint8_t>(salt.begin(),salt.end())));
        w.setSetting("wallet_verify_hash",util::hex(std::vector<uint8_t>(hash.begin(),hash.end())));
        OPENSSL_cleanse(key.data(),key.size());
    }
};
ChainDB* g_chain_db_direct = nullptr;
UTXOIndex* g_utxo_set_direct = nullptr;
}

namespace {

class ScopedHomeEnv {
public:
    explicit ScopedHomeEnv(const fs::path& home) {
        const char* current = std::getenv("HOME");
        if (current) {
            had_home_ = true;
            old_home_ = current;
        }
        fs::create_directories(home);
        setenv("HOME", home.string().c_str(), 1);
    }

    ~ScopedHomeEnv() {
        if (had_home_) {
            setenv("HOME", old_home_.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }

private:
    bool had_home_ = false;
    std::string old_home_;
};

fs::path make_temp_dir(const std::string& prefix) {
    std::string templ = (std::filesystem::temp_directory_path() / (prefix + "XXXXXX")).string();
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* out = mkdtemp(buf.data());
    if (!out) {
        throw std::runtime_error("mkdtemp failed");
    }
    return fs::path(out);
}

din::Json make_create_params(const std::string& wallet_name,
                             int word_count,
                             const std::string& bip39_passphrase,
                             const std::string& encryption_password,
                             const std::string& policy) {
    din::Json params = din::arr();
    params.append(wallet_name);
    params.append(word_count);
    params.append(bip39_passphrase);
    params.append(encryption_password);
    params.append(policy);
    return params;
}

din::Json make_restore_params(const std::string& wallet_name,
                              const std::string& mnemonic,
                              const std::string& bip39_passphrase,
                              const std::string& encryption_password,
                              const std::string& policy,
                              const std::string& expected_first_address = "",
                              bool skip_checksum = false,
                              bool replace_existing = false) {
    din::Json params = din::arr();
    params.append(wallet_name);
    params.append(mnemonic);
    params.append(bip39_passphrase);
    params.append(encryption_password);
    params.append(policy);
    if (!expected_first_address.empty() || skip_checksum || replace_existing) {
        params.append(expected_first_address);
    }
    if (skip_checksum || replace_existing) {
        params.append(skip_checksum);
    }
    if (replace_existing) {
        params.append(true);
    }
    return params;
}

void assert_rpc_success(const din::Json& result) {
    ASSERT_TRUE(result.isObject());
    ASSERT_TRUE(result.isMember("success")) << result.toStyledString();
    ASSERT_TRUE(result["success"].asBool()) << result.toStyledString();
    ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
}

void assert_rpc_error(const din::Json& result) {
    ASSERT_TRUE(result.isObject());
    ASSERT_TRUE(result.isMember("error")) << result.toStyledString();
    ASSERT_FALSE(result["error"].asString().empty()) << result.toStyledString();
}

std::vector<std::string> split_words(const std::string& mnemonic) {
    std::istringstream iss(mnemonic);
    std::vector<std::string> words;
    std::string word;
    while (iss >> word) {
        words.push_back(word);
    }
    return words;
}

std::string join_words(const std::vector<std::string>& words) {
    std::ostringstream oss;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << words[i];
    }
    return oss.str();
}

std::string altered_mnemonic(const std::string& mnemonic) {
    auto words = split_words(mnemonic);
    if (!words.empty()) {
        words.back() = (words.back() == "abandon") ? "about" : "abandon";
    }
    return join_words(words);
}

std::string missing_word_mnemonic(const std::string& mnemonic) {
    auto words = split_words(mnemonic);
    if (!words.empty()) {
        words.pop_back();
    }
    return join_words(words);
}

bool compute_taproot_output_key_for_test(const std::vector<uint8_t>& internal_xonly,
                                         std::array<uint8_t, 32>& output_key) {
    if (internal_xonly.size() != 32) {
        return false;
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        return false;
    }

    bool ok = false;
    do {
        secp256k1_xonly_pubkey internal_pk;
        if (!secp256k1_xonly_pubkey_parse(ctx, &internal_pk, internal_xonly.data())) {
            break;
        }

        const char* tag = "TapTweak";
        unsigned char tag_hash[32];
        SHA256(reinterpret_cast<const unsigned char*>(tag), std::strlen(tag), tag_hash);

        unsigned char tweak[32];
        dinero::crypto::CSHA256()
            .Write(tag_hash, sizeof(tag_hash))
            .Write(tag_hash, sizeof(tag_hash))
            .Write(internal_xonly.data(), internal_xonly.size())
            .Finalize(tweak);

        secp256k1_pubkey output_pk;
        if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pk, &internal_pk, tweak)) {
            break;
        }

        secp256k1_xonly_pubkey output_xonly;
        int parity = 0;
        if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_xonly, &parity, &output_pk)) {
            break;
        }

        if (!secp256k1_xonly_pubkey_serialize(ctx, output_key.data(), &output_xonly)) {
            break;
        }

        ok = true;
    } while (false);

    secp256k1_context_destroy(ctx);
    return ok;
}

std::optional<std::string> derive_bip86_first_address_from_seed(const std::vector<uint8_t>& seed) {
    if (seed.size() != 64) {
        return std::nullopt;
    }

    constexpr uint32_t HARDENED = 0x80000000;
    // Canonical coin type for v7+ is 1448. The 1447 legacy scan path was
    // removed entirely on 2026-04-18; every derivation site in
    // src/wallet/* and src/daemon/* uses 1448. This helper had been
    // pinned to 1447 (stale), which caused the helper-derived first
    // address to disagree with the RPC restore path's actual derivation
    // at m/86'/1448'/0'/0/0 — same root cause as PR #58's stale 1447h
    // pin in test_wallet_descriptor_active_context.cpp.
    constexpr uint32_t DINERO_COIN_TYPE = 1448;

    auto master = dinero::crypto::HDKeychain::fromSeed(seed);
    auto purpose = master.derive(86 | HARDENED);
    auto coin = purpose.derive(DINERO_COIN_TYPE | HARDENED);
    auto account = coin.derive(0 | HARDENED);
    auto chain = account.derive(0);
    auto first = chain.derive(0);

    auto pubkey = first.getPublicKey();
    if (pubkey.size() != 33) {
        return std::nullopt;
    }

    std::vector<uint8_t> xonly(pubkey.begin() + 1, pubkey.end());
    std::array<uint8_t, 32> output_key{};
    if (!compute_taproot_output_key_for_test(xonly, output_key)) {
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

std::string random_ascii(std::mt19937_64& rng, size_t min_len, size_t max_len) {
    std::uniform_int_distribution<size_t> len_dist(min_len, max_len);
    std::uniform_int_distribution<int> ch_dist(32, 126);
    const size_t len = len_dist(rng);
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(ch_dist(rng)));
    }
    return s;
}

dinero::Block make_block_with_single_output(const std::vector<uint8_t>& script_pubkey,
                                            uint64_t amount_una,
                                            uint64_t timestamp) {
    dinero::Block block{};
    block.header.timestamp = timestamp;

    dinero::Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 1;
    tx.vout.emplace_back(dinero::AmountUna::Una(amount_una), script_pubkey);

    block.vtx.push_back(tx);
    return block;
}

dinero::Block make_empty_block(uint64_t timestamp) {
    dinero::Block block{};
    block.header.timestamp = timestamp;
    return block;
}

dinero::Block make_block_with_transaction(const dinero::Transaction& tx, uint64_t timestamp) {
    dinero::Block block{};
    block.header.timestamp = timestamp;
    block.vtx.push_back(tx);
    return block;
}

dinero::Transaction make_spend_with_change_tx(const std::string& prev_txid_hex,
                                              uint32_t prev_vout,
                                              const std::vector<uint8_t>& recipient_script_pubkey,
                                              uint64_t recipient_amount_una,
                                              const std::vector<uint8_t>& change_script_pubkey,
                                              uint64_t change_amount_una) {
    dinero::Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 1;

    dinero::TxInput input;
    input.prevout.txid = dinero::TxId(dinero::uint256::FromHexUnsafe(prev_txid_hex));
    input.prevout.vout = prev_vout;
    tx.vin.push_back(input);

    tx.vout.emplace_back(dinero::AmountUna::Una(recipient_amount_una), recipient_script_pubkey);
    tx.vout.emplace_back(dinero::AmountUna::Una(change_amount_una), change_script_pubkey);
    return tx;
}

std::vector<uint8_t> make_external_taproot_script_pubkey() {
    std::vector<uint8_t> script(34, 0);
    script[0] = 0x51;  // OP_1
    script[1] = 0x20;  // Push 32 bytes
    for (size_t i = 2; i < script.size(); ++i) {
        script[i] = static_cast<uint8_t>(i);
    }
    return script;
}

std::vector<std::string> query_derivation_paths(const fs::path& wallet_db, int change) {
    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_db.string().c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return {};
    }

    const char* sql = "SELECT derivation_path FROM address_derivation_paths WHERE change = ? ORDER BY address_index";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return {};
    }
    if (sqlite3_bind_int(stmt, 1, change) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return {};
    }

    std::vector<std::string> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (txt) {
            out.emplace_back(txt);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

int query_max_index(const fs::path& wallet_db, int change) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT COALESCE(MAX(address_index), -1) FROM address_derivation_paths WHERE change = ?";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    EXPECT_EQ(sqlite3_bind_int(stmt, 1, change), SQLITE_OK);

    int value = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

bool query_registry_has_last_opened(const fs::path& registry_db,
                                    const std::string& wallet_name) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(registry_db.string().c_str(), &db), SQLITE_OK);
    if (!db) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT last_opened IS NOT NULL FROM wallets WHERE name = ? LIMIT 1";
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    if (!stmt) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, wallet_name.c_str(), -1, SQLITE_TRANSIENT);
    const bool has_last_opened =
        sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return has_last_opened;
}

std::vector<uint8_t> query_encrypted_seed_blob(const fs::path& wallet_db) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT encrypted_seed FROM hd_seeds WHERE id = 1";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);

    std::vector<uint8_t> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size > 0) {
            out.assign(blob, blob + size);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

int query_encryption_flag(const fs::path& wallet_db) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT COALESCE(encrypted, 0) FROM encryption_metadata WHERE id = 1";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);

    int value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

int query_transaction_count_at_height(const fs::path& wallet_db, int height) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT COUNT(*) FROM transactions WHERE height = ?";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    EXPECT_EQ(sqlite3_bind_int(stmt, 1, height), SQLITE_OK);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

std::optional<double> query_transaction_amount_at_height(const fs::path& wallet_db, int height) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT amount FROM transactions WHERE height = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    EXPECT_EQ(sqlite3_bind_int(stmt, 1, height), SQLITE_OK);

    std::optional<double> amount;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        amount = sqlite3_column_double(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return amount;
}

int query_transaction_count_total(const fs::path& wallet_db) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT COUNT(*) FROM transactions";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

int query_max_transaction_height(const fs::path& wallet_db) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT COALESCE(MAX(height), -1) FROM transactions";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);

    int height = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        height = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return height;
}

int query_max_utxo_height(const fs::path& wallet_db) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT COALESCE(MAX(height), -1) FROM utxos";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);

    int height = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        height = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return height;
}

std::optional<bool> query_utxo_spent_state(const fs::path& wallet_db,
                                           const std::string& txid,
                                           int vout) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(wallet_db.string().c_str(), &db), SQLITE_OK);

    const char* sql = "SELECT is_spent FROM utxos WHERE txid = ? AND vout = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    EXPECT_EQ(sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    EXPECT_EQ(sqlite3_bind_int(stmt, 2, vout), SQLITE_OK);

    std::optional<bool> is_spent;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        is_spent = sqlite3_column_int(stmt, 0) != 0;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return is_spent;
}

void expect_path_series(const std::vector<std::string>& paths, int change, size_t expected_count) {
    ASSERT_EQ(paths.size(), expected_count);
    for (size_t i = 0; i < paths.size(); ++i) {
        // Canonical coin type for v7+ is 1448 (not 1447 — legacy path
        // removed 2026-04-18). See companion comment in
        // derive_bip86_first_address_from_seed() above.
        std::string expected = "m/86'/1448'/0'/" + std::to_string(change) + "/" + std::to_string(i);
        EXPECT_EQ(paths[i], expected);
    }
}

}  // namespace

TEST(WalletMainnetReadiness, AuthoritativeMnemonicBindingAndLegacyFailClosed) {
    const fs::path root = make_temp_dir("din_wallet_mnemonic_binding_");
    const fs::path home = root / "home";
    const fs::path legacy_data = root / "legacy";
    const fs::path atomic_data = root / "atomic";
    const fs::path mnemonic_data = root / "mnemonic";
    fs::create_directories(legacy_data);
    fs::create_directories(atomic_data);
    fs::create_directories(mnemonic_data);
    ScopedHomeEnv scoped_home(home);

    // WalletManager::create() is an internal raw-seed primitive. There is no
    // inverse from its random 64-byte seed to a BIP39 phrase, so it must never
    // fabricate recovery words.
    {
        dinero::WalletManager legacy_wallet(legacy_data);
        legacy_wallet.create("legacy");
        std::string error;
        EXPECT_FALSE(legacy_wallet.hasAuthoritativeBip39Mnemonic());
        EXPECT_FALSE(legacy_wallet.loadAuthoritativeBip39Mnemonic(&error).has_value());
        EXPECT_NE(error.find("no authoritative mnemonic"), std::string::npos);
    }

    // The user-facing primitive publishes the wallet only after its very first
    // persisted seed is BIP39-derived and the authenticated record exists.
    // RpcCreateHDWallet must not rely on a later repair write for this property.
    {
        const std::string atomic_mnemonic =
            dinero::bip39::Generate(dinero::bip39::WordCount::Words12);
        ASSERT_FALSE(atomic_mnemonic.empty());
        dinero::WalletManager atomic_wallet(atomic_data);
        ASSERT_NO_THROW(atomic_wallet.createFromBip39("atomic", atomic_mnemonic, ""));
        EXPECT_TRUE(atomic_wallet.exists("atomic"));
        std::string error;
        auto material = atomic_wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(material.has_value()) << error;
        EXPECT_EQ(material->mnemonic, atomic_mnemonic);
        EXPECT_TRUE(atomic_wallet.listAddresses(false).empty());
    }

    const std::string bip39_passphrase = "separate-passphrase-must-be-backed-up";
    std::string mnemonic;
    {
        dinero::WalletManager wallet(mnemonic_data);
        din::Json created = dinero::rpc::RpcCreateHDWallet(
            make_create_params("default", 12, bip39_passphrase, "", "bip86"), &wallet);
        assert_rpc_success(created);
        mnemonic = created["mnemonic"].asString();

        // Finalizing encryption/fingerprint metadata must update the registry
        // row in place. INSERT OR REPLACE used to clear last_opened, causing a
        // different wallet to become active after restart.
        EXPECT_TRUE(query_registry_has_last_opened(
            mnemonic_data / "wallet_registry.db", "default"));

        std::string error;
        auto material = wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(material.has_value()) << error;
        EXPECT_EQ(material->mnemonic, mnemonic);
        EXPECT_TRUE(material->passphrase_required);
        EXPECT_FALSE(material->backup_acknowledged);

        EXPECT_FALSE(wallet.acknowledgeBip39Backup(mnemonic, false, &error));
        EXPECT_NE(error.find("separate BIP39 passphrase"), std::string::npos);
        ASSERT_TRUE(wallet.acknowledgeBip39Backup(mnemonic, true, &error)) << error;
    }

    {
        dinero::WalletManager wallet(mnemonic_data);
        wallet.open("default");
        std::string error;
        auto material = wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(material.has_value()) << error;
        EXPECT_EQ(material->mnemonic, mnemonic);
        EXPECT_TRUE(material->passphrase_required);
        EXPECT_TRUE(material->backup_acknowledged);

        // Authentication is fail-closed: a one-nibble ciphertext mutation
        // cannot yield recovery material even though the wallet seed remains.
        std::string record = wallet.getSetting("bip39_recovery_v1");
        ASSERT_GT(record.size(), 4u);
        record.back() = (record.back() == '0') ? '1' : '0';
        wallet.setSetting("bip39_recovery_v1", record);
        EXPECT_FALSE(wallet.loadAuthoritativeBip39Mnemonic(&error).has_value());
        EXPECT_NE(error.find("authentication failed"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, EncryptionRoundTripRestoreAndDerivationPersistence) {
    const fs::path root = make_temp_dir("din_wallet_ready_");
    const fs::path home = root / "home";
    const fs::path data_a = root / "node_a";
    const fs::path data_b = root / "node_b";
    fs::create_directories(data_a);
    fs::create_directories(data_b);
    ScopedHomeEnv scoped_home(home);

    const fs::path db_a = data_a / "wallets" / "wallet_default.db";
    const fs::path db_b = data_b / "wallets" / "wallet_default.db";

    std::string mnemonic;
    std::vector<std::string> original_external;
    std::vector<std::string> original_change;

    {
        dinero::WalletManager wallet(data_a);
        din::Json created = dinero::rpc::RpcCreateHDWallet(
            make_create_params("default", 12, "", "", "bip86"), &wallet);
        assert_rpc_success(created);

        ASSERT_TRUE(created.isMember("mnemonic"));
        ASSERT_TRUE(created.isMember("first_address"));
        mnemonic = created["mnemonic"].asString();
        std::string recovery_error;
        auto recovery = wallet.loadAuthoritativeBip39Mnemonic(&recovery_error);
        ASSERT_TRUE(recovery.has_value()) << recovery_error;
        EXPECT_EQ(recovery->mnemonic, mnemonic);
        EXPECT_FALSE(recovery->passphrase_required);
        EXPECT_FALSE(recovery->backup_acknowledged);
        original_external.push_back(created["first_address"].asString());  // index 0

        for (int i = 0; i < 19; ++i) {  // indices 1..19
            std::string addr = wallet.getNewAddress("", "taproot");
            ASSERT_FALSE(addr.empty());
            original_external.push_back(addr);
        }

        for (int i = 0; i < 5; ++i) {  // change indices 0..4
            std::string addr = wallet.getNewChangeAddress("", "taproot");
            ASSERT_FALSE(addr.empty());
            original_change.push_back(addr);
        }

        EXPECT_EQ(query_max_index(db_a, 0), 19);
        EXPECT_EQ(query_max_index(db_a, 1), 4);

        std::vector<uint8_t> seed;
        ASSERT_TRUE(dinero::bip39::MnemonicToSeed(mnemonic, "", seed));
        ASSERT_EQ(seed.size(), 64u);

        wallet.encryptWallet("mainnet-readiness-pass");
        EXPECT_TRUE(wallet.isWalletEncrypted());
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_TRUE(wallet.getNewAddress("", "taproot").empty());

        const std::vector<uint8_t> encrypted_blob = query_encrypted_seed_blob(db_a);
        EXPECT_GE(encrypted_blob.size(), 64u);
        EXPECT_EQ(query_encryption_flag(db_a), 1);
        if (encrypted_blob.size() >= seed.size()) {
            EXPECT_FALSE(std::equal(seed.begin(), seed.end(), encrypted_blob.begin()));
        }
    }

    {
        dinero::WalletManager wallet(data_a);  // restart equivalent
        wallet.open("default");
        EXPECT_TRUE(wallet.isWalletEncrypted());
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_TRUE(wallet.getNewAddress("", "taproot").empty());

        wallet.unlockWallet("mainnet-readiness-pass");
        EXPECT_FALSE(wallet.isWalletLocked());

        std::string recovery_error;
        auto recovery = wallet.loadAuthoritativeBip39Mnemonic(&recovery_error);
        ASSERT_TRUE(recovery.has_value()) << recovery_error;
        EXPECT_EQ(recovery->mnemonic, mnemonic);

        for (int i = 0; i < 20; ++i) {  // indices 20..39
            std::string addr = wallet.getNewAddress("", "taproot");
            ASSERT_FALSE(addr.empty());
            original_external.push_back(addr);
        }

        for (int i = 0; i < 5; ++i) {  // change indices 5..9
            std::string addr = wallet.getNewChangeAddress("", "taproot");
            ASSERT_FALSE(addr.empty());
            original_change.push_back(addr);
        }

        EXPECT_EQ(query_max_index(db_a, 0), 39);
        EXPECT_EQ(query_max_index(db_a, 1), 9);

        expect_path_series(query_derivation_paths(db_a, 0), 0, 40);
        expect_path_series(query_derivation_paths(db_a, 1), 1, 10);
    }

    std::vector<std::string> restored_external;
    {
        dinero::WalletManager restored_wallet(data_b);
        din::Json restored = dinero::rpc::RpcRestoreWallet(
            make_restore_params("default", mnemonic, "", "", "bip86"), &restored_wallet);
        assert_rpc_success(restored);
        std::string recovery_error;
        auto recovery = restored_wallet.loadAuthoritativeBip39Mnemonic(&recovery_error);
        ASSERT_TRUE(recovery.has_value()) << recovery_error;
        EXPECT_EQ(recovery->mnemonic, mnemonic);
        ASSERT_TRUE(restored.isMember("addresses"));
        ASSERT_TRUE(restored["addresses"].isArray());
        ASSERT_TRUE(restored.isMember("addresses_restored"));
        ASSERT_TRUE(restored.isMember("gap_limit"));
        ASSERT_EQ(restored["gap_limit"].asInt(), 20);
        ASSERT_EQ(restored["addresses_restored"].asInt(), 20);
        ASSERT_EQ(restored["addresses"].size(), 20u);  // receive gap window: indices 0..19

        for (const auto& addr : restored["addresses"]) {
            restored_external.push_back(addr.asString());
        }
        for (int i = 0; i < 20; ++i) {  // indices 20..39
            std::string addr = restored_wallet.getNewAddress("", "taproot");
            ASSERT_FALSE(addr.empty());
            restored_external.push_back(addr);
        }
    }

    ASSERT_EQ(original_external.size(), 40u);
    ASSERT_EQ(restored_external.size(), 40u);
    for (size_t i = 0; i < original_external.size(); ++i) {
        EXPECT_EQ(restored_external[i], original_external[i]) << "address index " << i;
    }

    expect_path_series(query_derivation_paths(db_b, 0), 0, 40);
    expect_path_series(query_derivation_paths(db_b, 1), 1, 20);

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, InvalidMnemonicRestoreFailsWithoutPartialWallet) {
    const fs::path root = make_temp_dir("din_wallet_neg_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    const std::string valid =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    const std::string altered = altered_mnemonic(valid);
    const std::string missing = missing_word_mnemonic(valid);

    {
        dinero::WalletManager wallet(data);
        din::Json altered_result = dinero::rpc::RpcRestoreWallet(
            make_restore_params("bad_altered", altered, "", "", "bip86"), &wallet);
        assert_rpc_error(altered_result);
        EXPECT_FALSE(fs::exists(data / "wallets" / "wallet_bad_altered.db"));

        din::Json missing_result = dinero::rpc::RpcRestoreWallet(
            make_restore_params("bad_missing", missing, "", "", "bip86"), &wallet);
        assert_rpc_error(missing_result);
        EXPECT_FALSE(fs::exists(data / "wallets" / "wallet_bad_missing.db"));
    }

    fs::remove_all(root);
}

namespace {
// Observe only statement kinds on connections opened by this synchronous fixture.
// No SQL values, key material, or database contents leave the fixture.
class FreshRestoreSeedWrites {
public:
    FreshRestoreSeedWrites() {
        if (active) throw std::logic_error("nested restore observer");
        active = this;
        if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(Install)) != SQLITE_OK) {
            active = nullptr;
            throw std::runtime_error("restore observer registration failed");
        }
    }
    ~FreshRestoreSeedWrites() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(Install));
        active = nullptr;
    }
    int writes = 0;
private:
    static inline thread_local FreshRestoreSeedWrites* active = nullptr;
    static int Install(sqlite3* db, char**, const sqlite3_api_routines*) {
        if (active) sqlite3_trace_v2(db, SQLITE_TRACE_STMT, Trace, active);
        return SQLITE_OK;
    }
    static int Trace(unsigned, void* context, void* statement, void*) {
        const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (sql && std::strstr(sql, "INSERT OR REPLACE INTO hd_seeds"))
            ++static_cast<FreshRestoreSeedWrites*>(context)->writes;
        return 0;
    }
};
}

TEST(WalletMainnetReadiness, FreshRestoreInitialSeedAndRecoveryBinding) {
    const auto root = make_temp_dir("din_fresh_restore_identity_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    std::vector<uint8_t> seed;
    ASSERT_TRUE(dinero::bip39::MnemonicToSeed(mnemonic, "recovery passphrase", seed));
    const auto expected = derive_bip86_first_address_from_seed(seed);
    ASSERT_TRUE(expected);
    {
        FreshRestoreSeedWrites observer;
        dinero::WalletManager wallet(data);
        const auto restored = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "recovered", mnemonic, "recovery passphrase", "", "bip86"), &wallet);
        assert_rpc_success(restored);
        ASSERT_TRUE(restored.isMember("first_address"));
        EXPECT_EQ(restored["first_address"].asString(), *expected);
        EXPECT_EQ(observer.writes, 1) << "recovery must be the first and only initial seed write";
        const auto stored_seed = query_encrypted_seed_blob(data / "wallets/wallet_recovered.db");
        EXPECT_THROW(wallet.createFromBip39("recovered", mnemonic, "another passphrase"),
                     std::runtime_error);
        EXPECT_EQ(query_encrypted_seed_blob(data / "wallets/wallet_recovered.db"), stored_seed);
        EXPECT_EQ(observer.writes, 1);
        std::string error;
        const auto recovery = wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(recovery) << error;
        EXPECT_EQ(recovery->mnemonic, mnemonic);
        EXPECT_TRUE(recovery->passphrase_required);
        EXPECT_EQ(query_max_index(data / "wallets/wallet_recovered.db", 0), 19);
        EXPECT_EQ(query_max_index(data / "wallets/wallet_recovered.db", 1), 19);
    }
    {
        dinero::WalletManager wallet(data);
        wallet.open("recovered");
        EXPECT_FALSE(wallet.isWalletEncrypted());
        std::string error;
        const auto recovery = wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(recovery) << error;
        EXPECT_EQ(recovery->mnemonic, mnemonic);
        EXPECT_TRUE(recovery->passphrase_required);
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, FreshRestoreEncryptedReopen) {
    const auto root = make_temp_dir("din_fresh_restore_encrypted_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    std::vector<uint8_t> seed;
    ASSERT_TRUE(dinero::bip39::MnemonicToSeed(mnemonic, "", seed));
    const auto expected = derive_bip86_first_address_from_seed(seed);
    ASSERT_TRUE(expected);
    {
        dinero::WalletManager wallet(data);
        const auto restored = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "recovered", mnemonic, "", "restore-password", "bip86"), &wallet);
        assert_rpc_success(restored);
        ASSERT_TRUE(restored.isMember("first_address"));
        EXPECT_EQ(restored["first_address"].asString(), *expected);
        EXPECT_TRUE(wallet.isWalletEncrypted());
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_EQ(query_encryption_flag(data / "wallets/wallet_recovered.db"), 1);
    }
    {
        dinero::WalletManager wallet(data);
        wallet.open("recovered");
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_THROW(wallet.unlockWallet("wrong-password"), std::runtime_error);
        ASSERT_NO_THROW(wallet.unlockWallet("restore-password"));
        std::string error;
        const auto recovery = wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(recovery) << error;
        EXPECT_EQ(recovery->mnemonic, mnemonic);
        EXPECT_FALSE(recovery->passphrase_required);
        EXPECT_FALSE(wallet.getNewAddress("", "taproot").empty());
        EXPECT_EQ(query_max_index(data / "wallets/wallet_recovered.db", 0), 20);
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, FreshRestoreExplicitChecksumBypass) {
    const auto root = make_temp_dir("din_fresh_restore_checksum_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon";
    ASSERT_FALSE(dinero::bip39::ValidateMnemonic(mnemonic));
    std::vector<uint8_t> seed;
    ASSERT_TRUE(dinero::bip39::MnemonicToSeed(mnemonic, "", seed, true));
    const auto expected = derive_bip86_first_address_from_seed(seed);
    ASSERT_TRUE(expected);
    {
        dinero::WalletManager wallet(data);
        const auto refused = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "recovered", mnemonic, "", "restore-password", "bip86"), &wallet);
        assert_rpc_error(refused);
        EXPECT_FALSE(wallet.exists("recovered"));
        const auto restored = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "recovered", mnemonic, "", "restore-password", "bip86", *expected, true), &wallet);
        assert_rpc_success(restored);
        ASSERT_TRUE(restored.isMember("first_address"));
        EXPECT_EQ(restored["first_address"].asString(), *expected);
        EXPECT_TRUE(restored.isMember("checksum_warning"));
        EXPECT_FALSE(restored["mnemonic_exportable"].asBool());
        EXPECT_TRUE(wallet.isWalletEncrypted());
    }
    {
        dinero::WalletManager wallet(data);
        wallet.open("recovered");
        ASSERT_NO_THROW(wallet.unlockWallet("restore-password"));
        std::string error;
        EXPECT_FALSE(wallet.loadAuthoritativeBip39Mnemonic(&error));
        EXPECT_FALSE(wallet.getNewAddress("", "taproot").empty());
        EXPECT_EQ(query_max_index(data / "wallets/wallet_recovered.db", 0), 20);
    }
    fs::remove_all(root);
}

namespace {
// Compare durable fixture files, including registry and SQLite WAL journals.
// SQLite -shm contains volatile reader marks changed even by read-only SELECTs.
// Contents stay in memory and are never printed in test diagnostics.
std::map<std::string, std::string> wallet_file_snapshot(const fs::path& data) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(data)) {
        if (!entry.is_regular_file() || entry.path().extension() == ".db-shm") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        if (!input) throw std::runtime_error("cannot read wallet fixture file");
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        if (input.bad()) throw std::runtime_error("incomplete wallet fixture read");
        files.emplace(fs::relative(entry.path(), data).generic_string(), std::move(bytes));
    }
    return files;
}
std::string changed_wallet_files(const fs::path& data,
                                const std::map<std::string, std::string>& before) {
    const auto after = wallet_file_snapshot(data);
    std::string names;
    for (const auto& [name, bytes] : before) {
        const auto it = after.find(name);
        if (it == after.end() || it->second != bytes) names += name + " ";
    }
    for (const auto& [name, bytes] : after)
        if (before.count(name) == 0) names += name + " ";
    return names;
}
}

TEST(WalletMainnetReadiness, RestoreRequiresNewNamePreservesOriginal) {
    const auto root = make_temp_dir("din_restore_new_name_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    const auto db = data / "wallets/wallet_default.db";
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    std::string original_mnemonic;
    std::optional<std::array<uint8_t, 32>> original_pq;
    std::vector<uint8_t> original_seed_blob;
    std::string imported_script;
    std::vector<uint8_t> imported_key(32, 0);
    imported_key.back() = 7;
    {
        dinero::WalletManager wallet(data);
        const auto created = dinero::rpc::RpcCreateHDWallet(
            make_create_params("default", 12, "", "", "bip86"), &wallet);
        assert_rpc_success(created);
        std::string error;
        const auto recovery = wallet.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(recovery) << error;
        original_mnemonic = recovery->mnemonic;
        ASSERT_NE(original_mnemonic, mnemonic);
        const auto imported_address = wallet.importPrivateKey(imported_key, "original import");
        ASSERT_FALSE(imported_address.empty());
        const auto script = wallet.getScriptPubKeyForAddress(imported_address);
        ASSERT_TRUE(script);
        imported_script = *script;
        wallet.encryptWallet("old-passphrase");
        wallet.unlockWallet("old-passphrase");
        original_pq = wallet.GetV7PqMasterKey();
        ASSERT_TRUE(original_pq);
        wallet.lockWallet();
        original_seed_blob = query_encrypted_seed_blob(db);
        ASSERT_FALSE(original_seed_blob.empty());
        const auto original_files = wallet_file_snapshot(data);
        ASSERT_FALSE(original_files.empty());
        auto* original_db = wallet.getCurrentDatabase();
        const auto original_session = wallet.AcquireDatabaseLease()->Session();

        // Both RPC parameter forms and the former override must refuse.
        auto positional = make_restore_params(
            "default", mnemonic, "", "new-passphrase", "bip86", "", false, true);
        auto named = din::obj();
        named["name"] = "default";
        named["mnemonic"] = mnemonic;
        named["password"] = "new-passphrase";
        named["replace_existing"] = true;
        auto string_override = positional;
        string_override[7] = "true";
        for (const auto& params : {positional, named, string_override,
                make_restore_params("default", mnemonic, "", "", "bip86")}) {
            const auto refused = dinero::rpc::RpcRestoreWallet(params, &wallet);
            assert_rpc_error(refused);
            EXPECT_NE(refused["error"].asString().find("new wallet name"), std::string::npos);
            EXPECT_FALSE(refused.get("success", false).asBool());
            EXPECT_EQ(wallet.getCurrentWalletName(), "default");
            EXPECT_EQ(wallet.getCurrentDatabase(), original_db);
            EXPECT_EQ(wallet.AcquireDatabaseLease()->Session(), original_session);
            EXPECT_TRUE(wallet.isWalletEncrypted());
            EXPECT_TRUE(wallet.isWalletLocked());
            ASSERT_TRUE(wallet_file_snapshot(data) == original_files) << changed_wallet_files(data, original_files);
        }

        // The requested recovery succeeds under its own name and password.
        const auto restored = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "recovered", mnemonic, "", "new-passphrase", "bip86"), &wallet);
        assert_rpc_success(restored);
        EXPECT_EQ(restored["wallet_name"].asString(), "recovered");
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_TRUE(query_encrypted_seed_blob(db) == original_seed_blob);
        EXPECT_EQ(query_encryption_flag(db), 1);

        // Refusing an inactive existing target must not switch the selection.
        const auto both_files = wallet_file_snapshot(data);
        const auto recovered_session = wallet.AcquireDatabaseLease()->Session();
        const auto refused = dinero::rpc::RpcRestoreWallet(named, &wallet);
        assert_rpc_error(refused);
        EXPECT_EQ(wallet.getCurrentWalletName(), "recovered");
        EXPECT_EQ(wallet.AcquireDatabaseLease()->Session(), recovered_session);
        EXPECT_TRUE(wallet_file_snapshot(data) == both_files) << changed_wallet_files(data, both_files);
    }
    {
        dinero::WalletManager reopened(data);
        reopened.open("default");
        EXPECT_TRUE(reopened.isWalletEncrypted());
        EXPECT_TRUE(reopened.isWalletLocked());
        EXPECT_THROW(reopened.unlockWallet("new-passphrase"), std::runtime_error);
        ASSERT_NO_THROW(reopened.unlockWallet("old-passphrase"));
        EXPECT_TRUE(reopened.GetV7PqMasterKey() == original_pq);
        const auto imported = reopened.deriveKeyForScriptPubKey(imported_script);
        ASSERT_TRUE(imported);
        EXPECT_TRUE(*imported == imported_key);
        std::string error;
        const auto original = reopened.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(original) << error;
        EXPECT_EQ(original->mnemonic, original_mnemonic);
        EXPECT_TRUE(query_encrypted_seed_blob(db) == original_seed_blob);
        reopened.open("recovered");
        EXPECT_THROW(reopened.unlockWallet("old-passphrase"), std::runtime_error);
        ASSERT_NO_THROW(reopened.unlockWallet("new-passphrase"));
        const auto recovered = reopened.loadAuthoritativeBip39Mnemonic(&error);
        ASSERT_TRUE(recovered) << error;
        EXPECT_EQ(recovered->mnemonic, mnemonic);
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, RestoreRefusesUnencryptedAndUnregisteredTargets) {
    const auto root = make_temp_dir("din_restore_existing_targets_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    {
        dinero::WalletManager wallet(data);
        wallet.create("original");
        wallet.open("original");
        const auto before = wallet_file_snapshot(data);
        auto refused = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "original", mnemonic, "", "", "bip86", "", false, true), &wallet);
        assert_rpc_error(refused);
        EXPECT_FALSE(wallet.isWalletEncrypted());
        EXPECT_EQ(wallet.getCurrentWalletName(), "original");
        EXPECT_TRUE(wallet_file_snapshot(data) == before) << changed_wallet_files(data, before);

        // A pre-existing file is not permission to replace an unenrolled owner.
        const auto orphan = data / "wallets/wallet_unregistered.db";
        { std::ofstream file(orphan, std::ios::binary); file << "existing wallet file"; }
        ASSERT_FALSE(wallet.exists("unregistered"));
        const auto with_orphan = wallet_file_snapshot(data);
        refused = dinero::rpc::RpcRestoreWallet(make_restore_params(
            "unregistered", mnemonic, "", "", "bip86", "", false, true), &wallet);
        assert_rpc_error(refused);
        EXPECT_EQ(wallet.getCurrentWalletName(), "original");
        EXPECT_TRUE(wallet_file_snapshot(data) == with_orphan) << changed_wallet_files(data, with_orphan);
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, RejectsNonBip86PolicyWithoutPartialWallet) {
    const fs::path root = make_temp_dir("din_wallet_policy_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    const std::string valid =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    {
        dinero::WalletManager wallet(data);

        din::Json create_bip84 = dinero::rpc::RpcCreateHDWallet(
            make_create_params("bad_policy_create", 12, "", "", "bip84"), &wallet);
        assert_rpc_error(create_bip84);
        EXPECT_FALSE(fs::exists(data / "wallets" / "wallet_bad_policy_create.db"));

        din::Json restore_bip84 = dinero::rpc::RpcRestoreWallet(
            make_restore_params("bad_policy_restore", valid, "", "", "bip84"), &wallet);
        assert_rpc_error(restore_bip84);
        EXPECT_FALSE(fs::exists(data / "wallets" / "wallet_bad_policy_restore.db"));
    }

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, RestoreRpcFuzzMalformedPayloadsNoCrashNoPartialWallets) {
    const fs::path root = make_temp_dir("din_wallet_rpc_fuzz_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    std::mt19937_64 rng(0xD1CEB00CULL);
    const std::string valid =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    dinero::WalletManager wallet(data);

    for (int i = 0; i < 2000; ++i) {
        din::Json params;
        switch (i % 6) {
            case 0: {
                params = din::arr();
                params.append(random_ascii(rng, 0, 6));  // wallet name only, no mnemonic
                break;
            }
            case 1: {
                params = din::arr();
                params.append("fuzz_" + std::to_string(i));
                params.append(random_ascii(rng, 1, 48));  // invalid mnemonic gibberish
                params.append(random_ascii(rng, 0, 24));  // random passphrase
                params.append("");
                params.append("bip86");
                break;
            }
            case 2: {
                params = din::obj();
                params["name"] = "fuzz_obj_" + std::to_string(i);
                params["mnemonic"] = random_ascii(rng, 1, 64);  // invalid mnemonic
                params["policy"] = "bip86";
                break;
            }
            case 3: {
                params = din::obj();
                params["name"] = "fuzz_policy_" + std::to_string(i);
                params["mnemonic"] = valid;  // valid mnemonic, invalid policy should still fail pre-write
                params["policy"] = "bip84";
                break;
            }
            case 4: {
                params = din::obj();
                params["name"] = "fuzz_guard_" + std::to_string(i);
                params["mnemonic"] = valid;
                params["policy"] = "bip86";
                params["expected_first_address"] = random_ascii(rng, 3, 24);  // mismatch guard
                break;
            }
            default: {
                params = random_ascii(rng, 0, 40);  // completely wrong JSON type
                break;
            }
        }

        din::Json result = dinero::rpc::RpcRestoreWallet(params, &wallet);
        ASSERT_TRUE(result.isObject()) << "iteration " << i;
        if (!(result.isMember("success") && result["success"].asBool())) {
            ASSERT_TRUE(result.isMember("error")) << "iteration " << i << " result=" << result.toStyledString();
        }
    }

    size_t wallet_db_files = 0;
    const fs::path wallets_dir = data / "wallets";
    if (fs::exists(wallets_dir)) {
        for (const auto& entry : fs::directory_iterator(wallets_dir)) {
            if (entry.is_regular_file()) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("wallet_", 0) == 0 && entry.path().extension() == ".db") {
                    ++wallet_db_files;
                }
            }
        }
    }
    EXPECT_EQ(wallet_db_files, 0u);

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, Bip86DeterminismProperty1000RandomMnemonics) {
    const fs::path root = make_temp_dir("din_wallet_prop_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    const std::array<dinero::bip39::WordCount, 5> counts = {
        dinero::bip39::WordCount::Words12,
        dinero::bip39::WordCount::Words15,
        dinero::bip39::WordCount::Words18,
        dinero::bip39::WordCount::Words21,
        dinero::bip39::WordCount::Words24
    };

    std::mt19937_64 rng(0xB860C0DEULL);
    std::uniform_int_distribution<size_t> wc_dist(0, counts.size() - 1);

    dinero::WalletManager wallet(data);

    for (int i = 0; i < 1000; ++i) {
        const auto wc = counts[wc_dist(rng)];
        const std::string mnemonic = dinero::bip39::Generate(wc);
        ASSERT_FALSE(mnemonic.empty()) << "iteration " << i;
        ASSERT_TRUE(dinero::bip39::ValidateMnemonic(mnemonic)) << "iteration " << i;

        const std::string passphrase = (i % 4 == 0) ? "" : ("prop-pass-" + std::to_string(i));

        std::vector<uint8_t> seed1;
        std::vector<uint8_t> seed2;
        ASSERT_TRUE(dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed1)) << "iteration " << i;
        ASSERT_TRUE(dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed2)) << "iteration " << i;
        ASSERT_EQ(seed1.size(), 64u) << "iteration " << i;
        ASSERT_EQ(seed2.size(), 64u) << "iteration " << i;

        const auto addr1 = derive_bip86_first_address_from_seed(seed1);
        const auto addr2 = derive_bip86_first_address_from_seed(seed2);
        ASSERT_TRUE(addr1.has_value()) << "iteration " << i;
        ASSERT_TRUE(addr2.has_value()) << "iteration " << i;
        EXPECT_EQ(addr1.value(), addr2.value()) << "iteration " << i;

        std::string hrp = dinero::HrpForActiveNetworkRef();
        if (hrp.empty()) {
            hrp = "din";
        }
        EXPECT_EQ(addr1->rfind(hrp + "1p", 0), 0u) << "iteration " << i;

        // Integration sampling: every 100th sample must pass restore RPC with
        // expected-first-address guard enabled.
        if (i % 100 == 0) {
            const std::string wallet_name = "prop_wallet_" + std::to_string(i);
            din::Json restored = dinero::rpc::RpcRestoreWallet(
                make_restore_params(wallet_name,
                                    mnemonic,
                                    passphrase,
                                    "",
                                    "bip86",
                                    addr1.value()),
                &wallet);
            assert_rpc_success(restored);
            ASSERT_TRUE(restored.isMember("first_address"));
            EXPECT_EQ(restored["first_address"].asString(), addr1.value());
        }
    }

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, ReorgDepthFourClearsConfirmedBalanceWithoutChangeIndexRollback) {
    const fs::path root = make_temp_dir("din_wallet_reorg_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    const fs::path db = data / "wallets" / "wallet_default.db";

    dinero::WalletManager wallet(data);
    din::Json created = dinero::rpc::RpcCreateHDWallet(
        make_create_params("default", 12, "", "", "bip86"), &wallet);
    assert_rpc_success(created);

    const std::string receive_addr = created["first_address"].asString();
    ASSERT_FALSE(receive_addr.empty());

    auto spk_hex = wallet.getScriptPubKeyForAddress(receive_addr);
    ASSERT_TRUE(spk_hex.has_value());
    std::vector<uint8_t> spk_bytes = util::HexToBytes(spk_hex.value());
    ASSERT_FALSE(spk_bytes.empty());

    const uint64_t amount_una = 7ULL * dinero::ConsensusSubsidy::UNA_PER_DIN;

    // Height 100: wallet receives funds.
    wallet.onBlockConnected(make_block_with_single_output(spk_bytes, amount_una, 1000), 100);
    // 3 more confirmations (tip advances to 103).
    wallet.onBlockConnected(make_empty_block(1001), 101);
    wallet.onBlockConnected(make_empty_block(1002), 102);
    wallet.onBlockConnected(make_empty_block(1003), 103);

    auto before = wallet.getBalance();
    EXPECT_NEAR(before.confirmed, 7.0, 1e-12);
    EXPECT_NEAR(before.spendable, 7.0, 1e-12);
    EXPECT_NEAR(before.total, 7.0, 1e-12);
    EXPECT_EQ(query_transaction_count_at_height(db, 100), 1);
    auto tx_amount = query_transaction_amount_at_height(db, 100);
    ASSERT_TRUE(tx_amount.has_value());
    EXPECT_NEAR(tx_amount.value(), 7.0, 1e-12);

    // Simulate existing derivation state prior to reorg (change index 0 exists).
    const std::string change0 = wallet.getNewChangeAddress("", "taproot");
    ASSERT_FALSE(change0.empty());
    EXPECT_EQ(query_max_index(db, 1), 0);

    // Reorg depth 4: disconnect heights 103, 102, 101, 100.
    wallet.onBlockDisconnected(make_empty_block(1003), 103);
    wallet.onBlockDisconnected(make_empty_block(1002), 102);
    wallet.onBlockDisconnected(make_empty_block(1001), 101);
    wallet.onBlockDisconnected(make_empty_block(1000), 100);

    auto after = wallet.getBalance();
    EXPECT_NEAR(after.confirmed, 0.0, 1e-12);
    EXPECT_NEAR(after.spendable, 0.0, 1e-12);
    EXPECT_NEAR(after.total, 0.0, 1e-12);
    EXPECT_EQ(after.utxo_count, 0);
    EXPECT_EQ(query_transaction_count_at_height(db, 100), 0);

    // Change derivation state must not roll back during reorg.
    EXPECT_EQ(query_max_index(db, 1), 0);
    const std::string change1 = wallet.getNewChangeAddress("", "taproot");
    ASSERT_FALSE(change1.empty());
    EXPECT_NE(change1, change0);
    EXPECT_EQ(query_max_index(db, 1), 1);

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, ReorgSelfSpendWithChangeRestoresSpentStateAndHeightMetadata) {
    const fs::path root = make_temp_dir("din_wallet_reorg_change_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    const fs::path db = data / "wallets" / "wallet_default.db";

    dinero::WalletManager wallet(data);
    din::Json created = dinero::rpc::RpcCreateHDWallet(
        make_create_params("default", 12, "", "", "bip86"), &wallet);
    assert_rpc_success(created);

    const std::string receive_addr = created["first_address"].asString();
    ASSERT_FALSE(receive_addr.empty());

    auto receive_spk_hex = wallet.getScriptPubKeyForAddress(receive_addr);
    ASSERT_TRUE(receive_spk_hex.has_value());
    std::vector<uint8_t> receive_spk = util::HexToBytes(receive_spk_hex.value());
    ASSERT_FALSE(receive_spk.empty());

    const uint64_t funding_una = 10ULL * dinero::ConsensusSubsidy::UNA_PER_DIN;
    auto funding_block = make_block_with_single_output(receive_spk, funding_una, 2000);
    const std::string funding_txid = funding_block.vtx.at(0).GetTxid().AsUint256().GetHex();

    wallet.onBlockConnected(funding_block, 200);
    wallet.onBlockConnected(make_empty_block(2001), 201);
    wallet.onBlockConnected(make_empty_block(2002), 202);

    auto before_spend = wallet.getBalance();
    EXPECT_NEAR(before_spend.confirmed, 10.0, 1e-12);
    EXPECT_NEAR(before_spend.spendable, 10.0, 1e-12);
    EXPECT_NEAR(before_spend.total, 10.0, 1e-12);

    const int tx_count_before_mempool = query_transaction_count_total(db);

    const std::string change_addr = wallet.getNewChangeAddress("", "taproot");
    ASSERT_FALSE(change_addr.empty());
    auto change_spk_hex = wallet.getScriptPubKeyForAddress(change_addr);
    ASSERT_TRUE(change_spk_hex.has_value());
    std::vector<uint8_t> change_spk = util::HexToBytes(change_spk_hex.value());
    ASSERT_FALSE(change_spk.empty());

    const uint64_t recipient_una = 6ULL * dinero::ConsensusSubsidy::UNA_PER_DIN;
    const uint64_t change_una = 3ULL * dinero::ConsensusSubsidy::UNA_PER_DIN;
    auto spend_tx = make_spend_with_change_tx(
        funding_txid,
        0,
        make_external_taproot_script_pubkey(),
        recipient_una,
        change_spk,
        change_una);
    const std::string spend_txid = spend_tx.GetTxid().AsUint256().GetHex();

    // Pending tx should not mutate confirmed state in standalone mode.
    wallet.onMempoolTransaction(spend_tx);
    auto after_mempool = wallet.getBalance();
    EXPECT_NEAR(after_mempool.confirmed, 10.0, 1e-12);
    EXPECT_NEAR(after_mempool.spendable, 10.0, 1e-12);
    EXPECT_EQ(query_transaction_count_total(db), tx_count_before_mempool);

    auto spend_block = make_block_with_transaction(spend_tx, 2003);
    wallet.onBlockConnected(spend_block, 203);

    auto after_connect = wallet.getBalance();
    EXPECT_NEAR(after_connect.confirmed, 3.0, 1e-12);
    EXPECT_NEAR(after_connect.spendable, 3.0, 1e-12);
    EXPECT_NEAR(after_connect.total, 3.0, 1e-12);

    auto funding_spent = query_utxo_spent_state(db, funding_txid, 0);
    ASSERT_TRUE(funding_spent.has_value());
    EXPECT_TRUE(funding_spent.value());

    auto change_spent = query_utxo_spent_state(db, spend_txid, 1);
    ASSERT_TRUE(change_spent.has_value());
    EXPECT_FALSE(change_spent.value());

    EXPECT_EQ(query_transaction_count_at_height(db, 203), 1);
    auto change_tx_amount = query_transaction_amount_at_height(db, 203);
    ASSERT_TRUE(change_tx_amount.has_value());
    EXPECT_NEAR(change_tx_amount.value(), 3.0, 1e-12);

    wallet.onBlockDisconnected(spend_block, 203);

    auto after_reorg = wallet.getBalance();
    EXPECT_NEAR(after_reorg.confirmed, 10.0, 1e-12);
    EXPECT_NEAR(after_reorg.spendable, 10.0, 1e-12);
    EXPECT_NEAR(after_reorg.total, 10.0, 1e-12);

    funding_spent = query_utxo_spent_state(db, funding_txid, 0);
    ASSERT_TRUE(funding_spent.has_value());
    EXPECT_FALSE(funding_spent.value());

    change_spent = query_utxo_spent_state(db, spend_txid, 1);
    EXPECT_FALSE(change_spent.has_value());

    EXPECT_EQ(query_transaction_count_at_height(db, 203), 0);

    // Height metadata must remain consistent with current tip after disconnect.
    EXPECT_LE(query_max_transaction_height(db), 202);
    EXPECT_LE(query_max_utxo_height(db), 202);

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, WrongBip39PassphraseFailsCleanlyWithExpectedAddressGuard) {
    const fs::path root = make_temp_dir("din_wallet_pass_");
    const fs::path home = root / "home";
    const fs::path source_data = root / "source";
    const fs::path restore_data = root / "restore";
    fs::create_directories(source_data);
    fs::create_directories(restore_data);
    ScopedHomeEnv scoped_home(home);

    std::string mnemonic;
    std::string expected_first_address;

    {
        dinero::WalletManager source_wallet(source_data);
        din::Json created = dinero::rpc::RpcCreateHDWallet(
            make_create_params("source", 12, "correct-bip39-pass", "", "bip86"), &source_wallet);
        assert_rpc_success(created);
        mnemonic = created["mnemonic"].asString();
        expected_first_address = created["first_address"].asString();
    }

    {
        dinero::WalletManager restore_wallet(restore_data);
        din::Json restored = dinero::rpc::RpcRestoreWallet(
            make_restore_params("bad_wrong_passphrase",
                                mnemonic,
                                "wrong-bip39-pass",
                                "",
                                "bip86",
                                expected_first_address),
            &restore_wallet);
        assert_rpc_error(restored);
        EXPECT_FALSE(fs::exists(restore_data / "wallets" / "wallet_bad_wrong_passphrase.db"));
    }

    fs::remove_all(root);
}

// Viewing authority intentionally survives wallet.lock so the background
// scanner can keep recognizing notes. It must not survive wallet.unload: the
// next wallet opened in this process has a different shielded identity.
TEST(WalletMainnetReadiness, ShieldedViewingAuthorityClearedOnUnload) {
    const fs::path root = make_temp_dir("din_wallet_viewing_authority_");
    fs::create_directories(root);

    dinero::WalletManager wallet(root);
    wallet.create("authority");
    wallet.open("authority");
    wallet.encryptWallet("viewing-authority-passphrase");

    // First-time encryption must populate the long-lived scan caches before it
    // locks and erases the seed. Calling these accessors on an unencrypted
    // wallet would only derive temporary copies, while unlocking here would
    // hide the first-lock regression this test is meant to catch.
    ASSERT_TRUE(wallet.isWalletLocked());
    EXPECT_FALSE(wallet.HaveMasterSeed());
    const auto incoming = wallet.GetShieldedIncomingViewingKeys();
    const auto outgoing = wallet.GetShieldedOutgoingViewingKeys();
    const auto recipient = wallet.GetShieldedRecipientViewingAuthorities();
    ASSERT_FALSE(incoming.empty());
    ASSERT_FALSE(outgoing.empty());
    ASSERT_FALSE(recipient.empty());

    wallet.unload();
    EXPECT_FALSE(wallet.hasActiveWallet());
    EXPECT_TRUE(wallet.GetShieldedIncomingViewingKeys().empty())
        << "incoming viewing authority leaked across wallet unload";
    EXPECT_TRUE(wallet.GetShieldedOutgoingViewingKeys().empty())
        << "outgoing viewing authority leaked across wallet unload";
    EXPECT_TRUE(wallet.GetShieldedRecipientViewingAuthorities().empty())
        << "recipient/nullifier viewing authority leaked across wallet unload";

    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, InitialOwnerPreservesGeneratedPqAcrossWrapperLoss) {
    const auto root = make_temp_dir("din_initial_owner_generated_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    std::optional<std::array<uint8_t,32>> original;
    {
        dinero::WalletManager wallet(data);
        assert_rpc_success(dinero::rpc::RpcCreateHDWallet(
            make_create_params("original",12,"","password","bip86"), &wallet));
        wallet.unlockWallet("password");
        original = wallet.GetV7PqMasterKey();
        ASSERT_TRUE(original);
        wallet.lockWallet();
        ASSERT_EQ(sqlite3_exec(wallet.getCurrentDatabase(),
            "DELETE FROM settings WHERE key='v7_pq_master_key_encrypted'",nullptr,nullptr,nullptr), SQLITE_OK);
    }
    {
        dinero::WalletManager wallet(data);
        wallet.open("original");
        wallet.unlockWallet("password");
        EXPECT_TRUE(wallet.GetV7PqMasterKey() == original);
        wallet.lockWallet();
        ASSERT_EQ(sqlite3_exec(wallet.getCurrentDatabase(),
            "DELETE FROM settings WHERE key IN ('v7_pq_master_key_encrypted','wallet_initial_owner_v1')",nullptr,nullptr,nullptr), SQLITE_OK);
        wallet.unlockWallet("password");
        EXPECT_FALSE(wallet.GetV7PqMasterKey());
        EXPECT_TRUE(wallet.getSetting("v7_pq_master_key_encrypted").empty());
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, InitialOwnerRecoveryNeverGeneratesHistoricalPq) {
    const auto root = make_temp_dir("din_initial_owner_recovery_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    const std::string mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    {
        dinero::WalletManager wallet(data);
        assert_rpc_success(dinero::rpc::RpcRestoreWallet(
            make_restore_params("recovered",mnemonic,"","password","bip86"), &wallet));
        const auto owner = wallet.getSetting("wallet_initial_owner_v1");
        ASSERT_FALSE(owner.empty());
        wallet.unlockWallet("password");
        EXPECT_FALSE(wallet.GetV7PqMasterKey());
        EXPECT_TRUE(wallet.getSetting("v7_pq_master_key_encrypted").empty());
        wallet.open("recovered");
        wallet.unlockWallet("password");
        EXPECT_FALSE(wallet.GetV7PqMasterKey());
        EXPECT_EQ(wallet.getSetting("wallet_initial_owner_v1"), owner);
        EXPECT_TRUE(wallet.getSetting("v7_pq_master_key_encrypted").empty());
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, InitialOwnerBindingAndConsumedGeneration) {
    const auto root = make_temp_dir("din_initial_owner_binding_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    {
        dinero::WalletManager wallet(data);
        auto generated = dinero::WalletManager::GenerateBip39Identity(12, "bip39");
        const auto mnemonic = generated.Mnemonic();
        wallet.createFromGeneratedBip39("generated", std::move(generated), "bip39");
        EXPECT_THROW(wallet.createFromGeneratedBip39("reused", std::move(generated), "bip39"), std::runtime_error);
        EXPECT_FALSE(wallet.exists("reused"));
        EXPECT_FALSE(fs::exists(data / "wallets/wallet_reused.db"));
        const auto owner = wallet.getSetting("wallet_initial_owner_v1");
        ASSERT_FALSE(owner.empty());
        wallet.encryptWallet("password");
        wallet.setSetting("wallet_initial_owner_v1", "00");
        EXPECT_THROW(wallet.unlockWallet("password"), std::runtime_error);
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_FALSE(wallet.GetV7PqMasterKey());
        EXPECT_TRUE(wallet.getSetting("v7_pq_master_key_encrypted").empty());
        wallet.lockWallet();
        wallet.setSetting("wallet_initial_owner_v1", owner);
        wallet.unlockWallet("password");
        ASSERT_TRUE(wallet.GetV7PqMasterKey());
        // Same seed, different actual database identity: decryption alone is
        // insufficient authority to adopt another wallet's creation master.
        wallet.createFromBip39("other", mnemonic, "bip39");
        wallet.setSetting("wallet_initial_owner_v1", owner);
        wallet.encryptWallet("password");
        EXPECT_THROW(wallet.unlockWallet("password"), std::runtime_error);
        EXPECT_TRUE(wallet.isWalletLocked());
        EXPECT_FALSE(wallet.GetV7PqMasterKey());
        EXPECT_TRUE(wallet.getSetting("v7_pq_master_key_encrypted").empty());
    }
    fs::remove_all(root);
}

TEST(WalletMainnetReadiness, InitialOwnerSeedAndRecordRollbackTogether) {
    const auto root = make_temp_dir("din_initial_owner_atomic_");
    ScopedHomeEnv home(root / "home");
    const auto data = root / "node";
    fs::create_directories(data);
    {
        dinero::WalletManager wallet(data);
        wallet.create("generated");
        const auto seed = wallet.GetMasterSeed();
        ASSERT_TRUE(seed);
        const auto owner = wallet.getSetting("wallet_initial_owner_v1");
        ASSERT_FALSE(owner.empty());
        const auto original = wallet_file_snapshot(data);
        // An initial-owner write must never adopt an existing seed or caller transaction.
        EXPECT_FALSE(dinero::WalletInitialOwnerTestAccess::Store(wallet,*seed,owner));
        EXPECT_TRUE(wallet_file_snapshot(data) == original);
        auto* db = wallet.getCurrentDatabase();
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_FALSE(dinero::WalletInitialOwnerTestAccess::Store(wallet,*seed,owner));
        EXPECT_EQ(sqlite3_get_autocommit(db),0);
        ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
        // Exercise the actual initial seed transaction with a required-write fault.
        ASSERT_EQ(sqlite3_exec(db,"DELETE FROM hd_seeds; DELETE FROM settings WHERE key='wallet_initial_owner_v1'; CREATE TRIGGER refuse_initial_seed BEFORE INSERT ON hd_seeds BEGIN SELECT RAISE(ABORT,'seed fault'); END",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_FALSE(dinero::WalletInitialOwnerTestAccess::Store(wallet,*seed,owner));
        EXPECT_TRUE(wallet.getSetting("wallet_initial_owner_v1").empty());
        EXPECT_TRUE(wallet.GetMasterSeed() == seed);
        ASSERT_EQ(sqlite3_exec(db,"DROP TRIGGER refuse_initial_seed",nullptr,nullptr,nullptr),SQLITE_OK);
        sqlite3_commit_hook(db, [](void*) {return 1;},nullptr);
        EXPECT_FALSE(dinero::WalletInitialOwnerTestAccess::Store(wallet,*seed,owner));
        sqlite3_commit_hook(db,nullptr,nullptr);
        EXPECT_TRUE(wallet.getSetting("wallet_initial_owner_v1").empty());
        EXPECT_TRUE(wallet.GetMasterSeed() == seed);
        ASSERT_TRUE(dinero::WalletInitialOwnerTestAccess::Store(wallet,*seed,owner));
        EXPECT_EQ(wallet.getSetting("wallet_initial_owner_v1"),owner);
        wallet.open("generated");
        EXPECT_TRUE(wallet.GetMasterSeed() == seed);
        wallet.encryptWallet("password");
        wallet.unlockWallet("password");
        EXPECT_TRUE(wallet.GetV7PqMasterKey());
    }
    fs::remove_all(root);
}

namespace {
std::vector<std::string> unlock_rows(dinero::WalletManager& w) {
    std::vector<std::string> rows;
    for(const char* sql:{"SELECT quote(key)||':'||quote(value) FROM settings ORDER BY key",
        "SELECT quote(encrypted_seed)||':'||quote(encryption_version) FROM hd_seeds ORDER BY id"}) {
        sqlite3_stmt* q=nullptr;if(sqlite3_prepare_v2(w.getCurrentDatabase(),sql,-1,&q,nullptr)!=SQLITE_OK)throw std::runtime_error("unlock fixture read");
        int rc;while((rc=sqlite3_step(q))==SQLITE_ROW)rows.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(q,0)));
        sqlite3_finalize(q);if(rc!=SQLITE_DONE)throw std::runtime_error("unlock fixture EOF");
    }return rows;
}
}
TEST(WalletMainnetReadiness, StagedUnlockSeedAndPolicyFailuresPreserveState) {
    const auto root=make_temp_dir("din_unlock_seed_");ScopedHomeEnv home(root/"home");
    { dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");
      const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);
      auto* db=w.getCurrentDatabase();
      ASSERT_EQ(sqlite3_exec(db,"UPDATE hd_seeds SET encryption_version=99",nullptr,nullptr,nullptr),SQLITE_OK);
      auto rows=unlock_rows(w);
      EXPECT_THROW(w.unlockWallet("password",99),std::runtime_error);
      EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);EXPECT_TRUE(unlock_rows(w)==rows);
      ASSERT_EQ(sqlite3_exec(db,"UPDATE hd_seeds SET encryption_version=2",nullptr,nullptr,nullptr),SQLITE_OK);
      const auto verify=w.getSetting("wallet_verify_hash");w.setSetting("wallet_verify_hash","");rows=unlock_rows(w);
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);
      EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);EXPECT_TRUE(unlock_rows(w)==rows);
      w.setSetting("wallet_verify_hash",verify);w.setSetting("wallet_encrypted","0");
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);
      w.setSetting("wallet_encrypted","1");ASSERT_NO_THROW(w.unlockWallet("password",100));
      const auto unlocked=dinero::WalletUnlockOwnerTestAccess::State(w);rows=unlock_rows(w);
      EXPECT_THROW(w.unlockWallet("wrong",500),std::runtime_error);
      EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==unlocked);EXPECT_TRUE(unlock_rows(w)==rows);
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, StagedUnlockReadAndCommitFailuresPreserveState) {
    const auto root=make_temp_dir("din_unlock_commit_");ScopedHomeEnv home(root/"home");
    { dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");auto* db=w.getCurrentDatabase();
      const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);const auto rows=unlock_rows(w);
      sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*){
        return op==SQLITE_READ && table && std::string(table)=="hd_seeds"?SQLITE_DENY:SQLITE_OK;},nullptr);
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
      EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);EXPECT_TRUE(unlock_rows(w)==rows);
      ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));
      EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
      struct Hook { dinero::WalletManager* wallet;std::string before;bool called=false;bool unchanged=false;int reject=1; } hook{&w,state};
      sqlite3_commit_hook(db,[](void* p){auto& h=*static_cast<Hook*>(p);h.called=true;h.unchanged=dinero::WalletUnlockOwnerTestAccess::State(*h.wallet)==h.before;return h.reject;},&hook);
      EXPECT_THROW(w.unlockWallet("password",77),std::runtime_error);
      EXPECT_TRUE(hook.called);EXPECT_TRUE(hook.unchanged);EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);EXPECT_TRUE(unlock_rows(w)==rows);
      hook.called=false;hook.unchanged=false;hook.reject=0;
      EXPECT_NO_THROW(w.unlockWallet("password",77));sqlite3_commit_hook(db,nullptr,nullptr);
      EXPECT_TRUE(hook.called);EXPECT_TRUE(hook.unchanged);EXPECT_FALSE(w.isWalletLocked());EXPECT_TRUE(w.GetV7PqMasterKey());
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, StagedUnlockPqOwnersMustAgree) {
    const auto root=make_temp_dir("din_unlock_pq_");ScopedHomeEnv home(root/"home");
    { dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");
      dinero::WalletUnlockOwnerTestAccess::UseKnownCredentials(w);
      w.setSetting("v7_pq_master_key_encrypted",dinero::WalletUnlockOwnerTestAccess::DifferentPqWrapper(w));
      const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);auto rows=unlock_rows(w);
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);EXPECT_TRUE(unlock_rows(w)==rows);
      w.lockWallet();
      w.setSetting("v7_pq_master_key_encrypted","00");rows=unlock_rows(w);
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);EXPECT_TRUE(unlock_rows(w)==rows);
      ASSERT_EQ(sqlite3_exec(w.getCurrentDatabase(),"DELETE FROM settings WHERE key='v7_pq_master_key_encrypted'",nullptr,nullptr,nullptr),SQLITE_OK);
      ASSERT_NO_THROW(w.unlockWallet("password"));const auto pq=w.GetV7PqMasterKey();ASSERT_TRUE(pq);w.lockWallet();
      const auto owner=w.getSetting("wallet_initial_owner_v1");w.setSetting("wallet_initial_owner_v1","00");
      const auto locked=dinero::WalletUnlockOwnerTestAccess::State(w);rows=unlock_rows(w);
      EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==locked);EXPECT_TRUE(unlock_rows(w)==rows);
      w.setSetting("wallet_initial_owner_v1",owner);ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_TRUE(w.GetV7PqMasterKey()==pq);
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, StagedUnlockLegacyCredentialsPreserved) {
    const auto root=make_temp_dir("din_unlock_legacy_");ScopedHomeEnv home(root/"home");
    { dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");
      dinero::WalletUnlockOwnerTestAccess::UseKnownCredentials(w,true);
      const auto salt=w.getSetting("wallet_salt"),verify=w.getSetting("wallet_verify_hash");
      ASSERT_NO_THROW(w.unlockWallet("password"));ASSERT_TRUE(w.GetV7PqMasterKey());w.lockWallet();
      const auto rows=unlock_rows(w);ASSERT_NO_THROW(w.unlockWallet("password"));
      EXPECT_EQ(w.getSetting("wallet_salt"),salt);EXPECT_EQ(w.getSetting("wallet_verify_hash"),verify);EXPECT_TRUE(unlock_rows(w)==rows);
    }fs::remove_all(root);
}

namespace {
const std::string legacy67="din1p4rltumyufleww78u3ste2aj55slz9skpesr8rhqdkcpfgrqqpeeskv96c7";
void legacy_row(dinero::WalletManager& w,const std::string& address,const std::string& payload) {
    sqlite3_stmt* q=nullptr;auto* db=w.getCurrentDatabase();
    if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO imported_keys(address,private_key_enc,label) VALUES(?,?,'historical label')",-1,&q,nullptr)!=SQLITE_OK)throw std::runtime_error("fixture prepare");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(q,sqlite3_finalize);
    if(sqlite3_bind_text(q,1,address.data(),int(address.size()),SQLITE_TRANSIENT)!=SQLITE_OK ||
       sqlite3_bind_text(q,2,payload.data(),int(payload.size()),SQLITE_TRANSIENT)!=SQLITE_OK || sqlite3_step(q)!=SQLITE_DONE)throw std::runtime_error("fixture insert");
}
std::string legacy_rows(dinero::WalletManager& w) {
    sqlite3_stmt* q=nullptr;if(sqlite3_prepare_v2(w.getCurrentDatabase(),"SELECT hex(address)||':'||hex(CAST(private_key_enc AS BLOB))||':'||label FROM imported_keys ORDER BY address",-1,&q,nullptr)!=SQLITE_OK)throw std::runtime_error("fixture snapshot");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(q,sqlite3_finalize);std::string value;int rc;
    while((rc=sqlite3_step(q))==SQLITE_ROW)value+=std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,0)))+";";
    if(rc!=SQLITE_DONE)throw std::runtime_error("fixture snapshot EOF");return value;
}
}
TEST(WalletMainnetReadiness, LegacyInventoryHistoricalBindingAndReopen) {
    const auto root=make_temp_dir("din_legacy_binding_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password");
     const auto seed=w.GetMasterSeed();const auto pq=w.GetV7PqMasterKey();
     for(bool raw:{false,true}) {
        legacy_row(w,legacy67,dinero::WalletUnlockOwnerTestAccess::LegacyPayload(w,67,raw));
        const auto rows=legacy_rows(w);w.open("owner");ASSERT_NO_THROW(w.unlockWallet("password"));
        EXPECT_EQ(legacy_rows(w),rows);EXPECT_EQ(w.GetMasterSeed(),seed);EXPECT_EQ(w.GetV7PqMasterKey(),pq);
     }
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, LegacyInventoryMismatchAndMalformedRowsRefuse) {
    const auto root=make_temp_dir("din_legacy_mismatch_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password");
     const auto good=dinero::WalletUnlockOwnerTestAccess::LegacyPayload(w,67,false);
     const auto wrong=dinero::WalletUnlockOwnerTestAccess::LegacyPayload(w,68,true);
     legacy_row(w,legacy67,good);legacy_row(w,legacy67+"z",wrong);w.lockWallet();
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);auto rows=legacy_rows(w);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(legacy_rows(w),rows);
     ASSERT_EQ(sqlite3_exec(w.getCurrentDatabase(),"DELETE FROM imported_keys WHERE address LIKE '%z'",nullptr,nullptr,nullptr),SQLITE_OK);
     legacy_row(w,legacy67,wrong);rows=legacy_rows(w);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(legacy_rows(w),rows);
     legacy_row(w,legacy67,good);ASSERT_EQ(sqlite3_exec(w.getCurrentDatabase(),"UPDATE imported_keys SET private_key_enc=CAST(private_key_enc AS BLOB)",nullptr,nullptr,nullptr),SQLITE_OK);
     rows=legacy_rows(w);EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(legacy_rows(w),rows);
     legacy_row(w,legacy67,"truncated");rows=legacy_rows(w);EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(legacy_rows(w),rows);
     legacy_row(w,legacy67,good);ASSERT_NO_THROW(w.unlockWallet("password"));
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, LegacyInventoryIncompleteReadPreservesOwner) {
    const auto root=make_temp_dir("din_legacy_read_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password");
     legacy_row(w,legacy67,dinero::WalletUnlockOwnerTestAccess::LegacyPayload(w,67,true));w.lockWallet();auto* db=w.getCurrentDatabase();
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w),rows=legacy_rows(w);
     sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*){return op==SQLITE_READ && table && std::string(table)=="imported_keys"?SQLITE_DENY:SQLITE_OK;},nullptr);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(legacy_rows(w),rows);
     sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* db,void* statement,void*){const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));if(sql && std::string_view(sql)=="SELECT address,private_key_enc FROM imported_keys ORDER BY address")sqlite3_interrupt(static_cast<sqlite3*>(db));return 0;},db);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(legacy_rows(w),rows);
     ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_EQ(legacy_rows(w),rows);
    }fs::remove_all(root);
}

namespace {
void modern_sql(dinero::WalletManager& w,const std::string& sql) {
    if(sqlite3_exec(w.getCurrentDatabase(),sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("modern inventory fixture SQL");
}
void modern_imports(dinero::WalletManager& w) {
    for(uint8_t last:{71,72}) {
        std::vector<uint8_t> key(32,0);key.back()=last;
        if(w.importPrivateKey(key,"preserved import").empty())throw std::runtime_error("modern fixture import");
    }
}
std::string modern_rows(dinero::WalletManager& w) {
    std::string result;
    for(const char* table:{"taproot_keys","taproot_key_mapping","watch_scripts","addresses"}) {
        const auto sql=std::string("SELECT * FROM ")+table+" ORDER BY rowid";
        sqlite3_stmt* q=nullptr;
        if(sqlite3_prepare_v2(w.getCurrentDatabase(),sql.c_str(),-1,&q,nullptr)!=SQLITE_OK)
            throw std::runtime_error("modern fixture snapshot");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(q,sqlite3_finalize);int rc;
        while((rc=sqlite3_step(q))==SQLITE_ROW)for(int col=0;col<sqlite3_column_count(q);++col) {
            result+=std::to_string(sqlite3_column_type(q,col))+":";
            const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q,col));
            const int n=sqlite3_column_bytes(q,col);
            if(bytes)result+=util::hex(std::vector<uint8_t>(bytes,bytes+n));
            result+=";";
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error("modern fixture snapshot EOF");
        result+="|";
    }return result;
}
}
TEST(WalletMainnetReadiness, ModernInventoryBindingAndReopen) {
    const auto root=make_temp_dir("din_modern_binding_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");modern_imports(w);w.encryptWallet("password");
     ASSERT_NO_THROW(w.unlockWallet("password"));const auto seed=w.GetMasterSeed();
     const auto pq=w.GetV7PqMasterKey();ASSERT_TRUE(pq);const auto rows=modern_rows(w);const auto owners=unlock_rows(w);
     w.open("owner");ASSERT_NO_THROW(w.unlockWallet("password"));
     EXPECT_EQ(w.GetMasterSeed(),seed);EXPECT_EQ(w.GetV7PqMasterKey(),pq);
     EXPECT_EQ(modern_rows(w),rows);EXPECT_EQ(unlock_rows(w),owners);
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, ModernInventoryCorruptionAndBindingsRefuse) {
    const auto root=make_temp_dir("din_modern_corruption_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");modern_imports(w);w.encryptWallet("password");
     for(const char* table:{"taproot_keys","taproot_key_mapping","watch_scripts","addresses"})
        modern_sql(w,std::string("CREATE TEMP TABLE saved_")+table+" AS SELECT * FROM "+table);
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);
     const std::vector<std::pair<std::string,std::string>> mutations{
       {"taproot_keys","UPDATE taproot_keys SET internal_privkey=zeroblob(60) WHERE address=(SELECT MAX(address) FROM taproot_keys)"},
       {"taproot_keys","UPDATE taproot_keys SET internal_privkey=(SELECT internal_privkey FROM taproot_keys ORDER BY address LIMIT 1) WHERE address=(SELECT MAX(address) FROM taproot_keys)"},
       {"taproot_keys","UPDATE taproot_keys SET is_privkey_encrypted=0"},
       {"taproot_keys","UPDATE taproot_keys SET internal_pubkey=zeroblob(32)"},
       {"taproot_keys","UPDATE taproot_keys SET output_pubkey=CAST(output_pubkey AS TEXT)"},
       {"taproot_keys","UPDATE taproot_keys SET address=address||'x'"},
       {"taproot_key_mapping","UPDATE taproot_key_mapping SET internal_pubkey=zeroblob(32)"},
       {"taproot_key_mapping","DELETE FROM taproot_key_mapping"},
       {"watch_scripts","UPDATE watch_scripts SET path='m/0' WHERE path LIKE 'tr(%'"},
       {"watch_scripts","DELETE FROM watch_scripts WHERE path LIKE 'tr(%'"},
       {"addresses","UPDATE addresses SET script_pubkey='5120' WHERE account=-1"},
       {"addresses","UPDATE addresses SET account=0 WHERE account=-1"},
       {"addresses","DELETE FROM addresses WHERE account=-1"}
     };
     for(const auto& [table,sql]:mutations) {
        SCOPED_TRACE(sql);modern_sql(w,sql);const auto rows=modern_rows(w);const auto owners=unlock_rows(w);
        EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);
        EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);
        EXPECT_EQ(modern_rows(w),rows);EXPECT_EQ(unlock_rows(w),owners);
        modern_sql(w,"DELETE FROM "+table+"; INSERT INTO "+table+" SELECT * FROM saved_"+table);
     }
     ASSERT_NO_THROW(w.unlockWallet("password",100));const auto unlocked=dinero::WalletUnlockOwnerTestAccess::State(w);
     modern_sql(w,"UPDATE taproot_keys SET output_pubkey=zeroblob(32)");
     EXPECT_THROW(w.unlockWallet("password",500),std::runtime_error);
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),unlocked);
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, ModernInventoryIncompleteReadPreservesOwner) {
    const auto root=make_temp_dir("din_modern_read_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");modern_imports(w);w.encryptWallet("password");auto* db=w.getCurrentDatabase();
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w),rows=modern_rows(w);const auto owners=unlock_rows(w);
     sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*){return op==SQLITE_READ && table && std::string(table)=="taproot_key_mapping"?SQLITE_DENY:SQLITE_OK;},nullptr);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(modern_rows(w),rows);EXPECT_EQ(unlock_rows(w),owners);
     sqlite3_trace_v2(db,SQLITE_TRACE_STMT,[](unsigned,void* db,void* statement,void*) {
        const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(sql && std::string_view(sql)=="SELECT address,internal_privkey,internal_pubkey,output_pubkey,is_privkey_encrypted FROM taproot_keys ORDER BY address")
            sqlite3_interrupt(static_cast<sqlite3*>(db));return 0;
     },db);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(modern_rows(w),rows);EXPECT_EQ(unlock_rows(w),owners);
     ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_EQ(modern_rows(w),rows);
    }fs::remove_all(root);
}

namespace {
std::string pq_watch_rows(dinero::WalletManager& w) {
    std::string result;
    for(const char* table:{"watch_scripts"}) {
        const auto sql=std::string("SELECT * FROM ")+table+" ORDER BY rowid";
        sqlite3_stmt* q=nullptr;
        if(sqlite3_prepare_v2(w.getCurrentDatabase(),sql.c_str(),-1,&q,nullptr)!=SQLITE_OK)
            throw std::runtime_error("PQ fixture snapshot");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(q,sqlite3_finalize);int rc;
        while((rc=sqlite3_step(q))==SQLITE_ROW)for(int col=0;col<sqlite3_column_count(q);++col) {
            result+=std::to_string(sqlite3_column_type(q,col))+":";
            const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q,col));
            const int n=sqlite3_column_bytes(q,col);
            if(bytes)result+=util::hex(std::vector<uint8_t>(bytes,bytes+n));
            result+=";";
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error("PQ fixture snapshot EOF");
        result+="|";
    }return result;
}
namespace pw = dinero::wallet;
namespace pm = dinero::consensus::pq::ml_dsa_65;
std::string pq_file_bytes(const std::string& path) {
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("PQ fixture bytes");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void pq_sql(sqlite3* db,const std::string& sql) {
    if(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("PQ inventory fixture SQL");
}
std::unique_ptr<sqlite3,decltype(&sqlite3_close)> pq_db(const std::string& path) {
    sqlite3* db=nullptr;
    if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK){if(db)sqlite3_close(db);throw std::runtime_error("PQ fixture open");}
    return {db,sqlite3_close};
}
void pq_imports(dinero::WalletManager& w) {
    const auto master=w.GetV7PqMasterKey();if(!master)throw std::runtime_error("PQ fixture master");
    pw::V7P2MRStore store;
    if(store.Open(w.GetV7P2MRStorePath())!=pw::V7P2MRStore::OpenResult::Ok)throw std::runtime_error("PQ fixture store");
    for(uint8_t last:{77,78}) {
        pw::SecureSeed seed;seed.mutable_bytes().fill(last);
        pw::SecureKeypair pair(pm::KeygenFromSeed(seed.bytes()));
        std::array<uint8_t,32> root{};const uint8_t scheme=dinero::consensus::pq::SCHEME_ID_ML_DSA_65;
        dinero::crypto::CSHA256().Write(&scheme,1).Write(pair.pubkey().data(),pair.pubkey().size()).Finalize(root.data());
        const auto sealed=pw::SealSeed(seed.bytes(),*master);
        // Imported seed is intentionally unrelated to its recorded path.
        const auto path=last==77?"m/88'/1448'/9'/1/900":"imported:external";
        const auto address=pw::EncodeP2MRAddress(last==77?"din":"rdin",root);
        if(store.AddAddress(1,address,root,pair.pubkey(),sealed.ciphertext,sealed.nonce,sealed.tag,path,37,"preserve",last)!=pw::V7P2MRStore::AddResult::Ok)
            throw std::runtime_error("PQ fixture insert");
        w.registerP2MRAddress(pw::BuildP2MRScriptPubKey(root),path);
    }
}
bool pq_interrupt=false,pq_deny=false,pq_replace=false;
sqlite3* pq_writer=nullptr;
sqlite3* pq_reader=nullptr;
int pq_mutation=SQLITE_OK;
int pq_extension(sqlite3* db,char**,const sqlite3_api_routines*) {
    pq_reader=db;
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*) {
        return pq_deny && op==SQLITE_READ && table && std::string_view(table)=="v7_p2mr_addresses"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    return sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* db,void* stmt,void*) {
        const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
        if(sql && std::string_view(sql).find("seed_ciphertext, seed_nonce, seed_tag FROM v7_p2mr_addresses")!=std::string_view::npos) {
            if(pq_interrupt)sqlite3_interrupt(static_cast<sqlite3*>(db));
            if(pq_replace){pq_replace=false;pq_mutation=sqlite3_exec(pq_writer,"UPDATE v7_p2mr_addresses SET seed_tag=zeroblob(16) WHERE created_at=78",nullptr,nullptr,nullptr);}
        }return 0;
    },db);
}
struct PqTrace {
    PqTrace(){if(sqlite3_auto_extension(reinterpret_cast<void(*)()>(pq_extension))!=SQLITE_OK)throw std::runtime_error("PQ trace install");}
    ~PqTrace(){sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(pq_extension));pq_interrupt=pq_deny=pq_replace=false;pq_writer=nullptr;pq_reader=nullptr;}
};
}
TEST(WalletMainnetReadiness, PqInventoryBindingAndReopen) {
    const auto root=make_temp_dir("din_pq_binding_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password");pq_imports(w);
     const auto seed=w.GetMasterSeed();const auto master=w.GetV7PqMasterKey();const auto owners=unlock_rows(w);const auto rows=pq_watch_rows(w);
     const auto path=w.GetV7P2MRStorePath();const auto bytes=pq_file_bytes(path);
     w.open("owner");ASSERT_NO_THROW(w.unlockWallet("password"));
     EXPECT_EQ(w.GetMasterSeed(),seed);EXPECT_EQ(w.GetV7PqMasterKey(),master);EXPECT_EQ(unlock_rows(w),owners);EXPECT_EQ(pq_watch_rows(w),rows);EXPECT_EQ(pq_file_bytes(path),bytes);
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, PqInventoryCorruptionPreservesOwner) {
    const auto root=make_temp_dir("din_pq_corrupt_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password");pq_imports(w);w.lockWallet();
     const auto path=w.GetV7P2MRStorePath();auto db=pq_db(path);pq_sql(db.get(),"CREATE TEMP TABLE saved AS SELECT * FROM v7_p2mr_addresses");
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);const auto owners=unlock_rows(w);const auto rows=pq_watch_rows(w);
     for(const auto& sql:std::vector<std::string>{
        "UPDATE v7_p2mr_addresses SET seed_tag=zeroblob(16) WHERE created_at=78",
        "UPDATE v7_p2mr_addresses SET seed_ciphertext=(SELECT seed_ciphertext FROM v7_p2mr_addresses WHERE created_at=77),seed_nonce=(SELECT seed_nonce FROM v7_p2mr_addresses WHERE created_at=77),seed_tag=(SELECT seed_tag FROM v7_p2mr_addresses WHERE created_at=77) WHERE created_at=78",
        "UPDATE v7_p2mr_addresses SET pubkey=zeroblob(1952) WHERE created_at=78",
        "UPDATE v7_p2mr_addresses SET merkle_root=zeroblob(32) WHERE created_at=78",
        "UPDATE v7_p2mr_addresses SET address=address||'x' WHERE created_at=78",
        "UPDATE v7_p2mr_addresses SET derivation_path='unbound' WHERE created_at=78",
        "UPDATE v7_p2mr_addresses SET seed_nonce=CAST(seed_nonce AS TEXT) WHERE created_at=78"}) {
        SCOPED_TRACE(sql);pq_sql(db.get(),sql);const auto bytes=pq_file_bytes(path);
        EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);
        EXPECT_EQ(unlock_rows(w),owners);EXPECT_EQ(pq_watch_rows(w),rows);EXPECT_EQ(pq_file_bytes(path),bytes);
        pq_sql(db.get(),"DELETE FROM v7_p2mr_addresses;INSERT INTO v7_p2mr_addresses SELECT * FROM saved");
     }
     pq_sql(db.get(),"ALTER TABLE v7_p2mr_addresses RENAME TO unavailable");
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);
     pq_sql(db.get(),"ALTER TABLE unavailable RENAME TO v7_p2mr_addresses");
     modern_sql(w,"UPDATE watch_scripts SET path='unbound' WHERE substr(script_pubkey,1,2)=x'5320'");
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);
     // Restore exact recognition paths from the stored imported metadata.
     pw::V7P2MRStore store;ASSERT_EQ(store.OpenExistingReadOnly(path),pw::V7P2MRStore::OpenResult::Ok);
     for(const auto& row:store.CaptureKeysByWallet(1)) {
        sqlite3_stmt* q=nullptr;ASSERT_EQ(sqlite3_prepare_v2(w.getCurrentDatabase(),"UPDATE watch_scripts SET path=? WHERE script_pubkey=?",-1,&q,nullptr),SQLITE_OK);
        auto script=pw::BuildP2MRScriptPubKey(row.metadata.merkle_root);sqlite3_bind_text(q,1,row.metadata.derivation_path.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_blob(q,2,script.data(),script.size(),SQLITE_TRANSIENT);ASSERT_EQ(sqlite3_step(q),SQLITE_DONE);sqlite3_finalize(q);
     }
     ASSERT_NO_THROW(w.unlockWallet("password",100));const auto live=dinero::WalletUnlockOwnerTestAccess::State(w);
     pq_sql(db.get(),"UPDATE v7_p2mr_addresses SET seed_tag=zeroblob(16) WHERE created_at=78");
     EXPECT_THROW(w.unlockWallet("password",500),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),live);
     pq_sql(db.get(),"DELETE FROM v7_p2mr_addresses;INSERT INTO v7_p2mr_addresses SELECT * FROM saved");
     w.open("owner");
     modern_sql(w,"DELETE FROM settings WHERE key IN ('wallet_initial_owner_v1','v7_pq_master_key_encrypted')");
     const auto missing=dinero::WalletUnlockOwnerTestAccess::State(w);
     EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),missing);
     EXPECT_FALSE(w.GetV7PqMasterKey());
    }fs::remove_all(root);
}
TEST(WalletMainnetReadiness, PqInventoryReadAndSnapshot) {
    const auto root=make_temp_dir("din_pq_read_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password");pq_imports(w);w.lockWallet();
     auto writer=pq_db(w.GetV7P2MRStorePath());pq_sql(writer.get(),"PRAGMA journal_mode=WAL");
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);const auto owners=unlock_rows(w);
     PqTrace trace;
     {pw::V7P2MRStore reader;ASSERT_EQ(reader.OpenExistingReadOnly(w.GetV7P2MRStorePath()),pw::V7P2MRStore::OpenResult::Ok);
      ASSERT_TRUE(pq_reader);pq_sql(pq_reader,"BEGIN");EXPECT_THROW(reader.CaptureKeysByWallet(1),std::runtime_error);
      EXPECT_EQ(sqlite3_get_autocommit(pq_reader),0);pq_sql(pq_reader,"ROLLBACK");EXPECT_EQ(reader.CaptureKeysByWallet(1).size(),2u);}
     pq_deny=true;EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);pq_deny=false;
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(unlock_rows(w),owners);
     pq_interrupt=true;EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);pq_interrupt=false;
     EXPECT_EQ(dinero::WalletUnlockOwnerTestAccess::State(w),state);EXPECT_EQ(unlock_rows(w),owners);
     pq_writer=writer.get();pq_replace=true;pq_mutation=SQLITE_ERROR;
     ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_EQ(pq_mutation,SQLITE_OK);EXPECT_FALSE(pq_replace);
     w.lockWallet();EXPECT_THROW(w.unlockWallet("password"),std::runtime_error);EXPECT_TRUE(w.isLocked());
    }fs::remove_all(root);
}

TEST(WalletMainnetReadiness, TimeoutClearsPqAndPreservesDurableOwner) {
    const auto root=make_temp_dir("din_timeout_clear_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password",600);
     const auto pq=w.GetV7PqMasterKey();ASSERT_TRUE(pq);const auto rows=unlock_rows(w);
     dinero::WalletUnlockOwnerTestAccess::Expire(w);
     EXPECT_TRUE(w.isWalletLocked());EXPECT_FALSE(w.GetV7PqMasterKey());
     EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::Cleared(w));EXPECT_TRUE(unlock_rows(w)==rows);
     ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_TRUE(w.GetV7PqMasterKey()==pq);
     const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);
     EXPECT_FALSE(w.isWalletLocked());EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);
     w.lockWallet();EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::Cleared(w));
     ASSERT_NO_THROW(w.unlockWallet("password",600));dinero::WalletUnlockOwnerTestAccess::Expire(w);
     EXPECT_TRUE(w.isWalletLocked());EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::Cleared(w));EXPECT_TRUE(unlock_rows(w)==rows);
    }
    {dinero::WalletManager w(root/"node");w.open("owner");ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_TRUE(w.GetV7PqMasterKey());}
    fs::remove_all(root);
}
TEST(WalletMainnetReadiness, TimeoutHonorsExistingRecoveryPin) {
    const auto root=make_temp_dir("din_timeout_pin_");ScopedHomeEnv home(root/"home");
    {dinero::WalletManager w(root/"node");w.create("owner");w.encryptWallet("password");w.unlockWallet("password",600);
     const auto pq=w.GetV7PqMasterKey();ASSERT_TRUE(pq);const auto rows=unlock_rows(w);
     {auto lease=w.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(lease->Session());ASSERT_EQ(seed->Bytes().size(),64u);
      dinero::WalletUnlockOwnerTestAccess::Expire(w);const auto state=dinero::WalletUnlockOwnerTestAccess::State(w);
      EXPECT_THROW(w.isWalletLocked(),std::logic_error);EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::State(w)==state);
      EXPECT_TRUE(w.GetV7PqMasterKey()==pq);EXPECT_TRUE(unlock_rows(w)==rows);
     }
     EXPECT_TRUE(w.isWalletLocked());EXPECT_TRUE(dinero::WalletUnlockOwnerTestAccess::Cleared(w));EXPECT_TRUE(unlock_rows(w)==rows);
     ASSERT_NO_THROW(w.unlockWallet("password"));EXPECT_TRUE(w.GetV7PqMasterKey()==pq);
    }fs::remove_all(root);
}
