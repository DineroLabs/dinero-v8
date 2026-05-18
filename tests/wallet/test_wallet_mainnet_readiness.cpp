#include <gtest/gtest.h>

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

#include <openssl/sha.h>
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

namespace fs = std::filesystem;

namespace dinero {
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
                              const std::string& expected_first_address = "") {
    din::Json params = din::arr();
    params.append(wallet_name);
    params.append(mnemonic);
    params.append(bip39_passphrase);
    params.append(encryption_password);
    params.append(policy);
    if (!expected_first_address.empty()) {
        params.append(expected_first_address);
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
        SHA256_CTX sha_ctx;
        SHA256_Init(&sha_ctx);
        SHA256_Update(&sha_ctx, tag_hash, sizeof(tag_hash));
        SHA256_Update(&sha_ctx, tag_hash, sizeof(tag_hash));
        SHA256_Update(&sha_ctx, internal_xonly.data(), internal_xonly.size());
        SHA256_Final(tweak, &sha_ctx);

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

TEST(WalletMainnetReadiness, RestoreResetsLegacyEncryptionStateBeforeReEncrypt) {
    const fs::path root = make_temp_dir("din_wallet_restore_reencrypt_");
    const fs::path home = root / "home";
    const fs::path data = root / "node";
    fs::create_directories(data);
    ScopedHomeEnv scoped_home(home);

    const fs::path db = data / "wallets" / "wallet_default.db";
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

    dinero::WalletManager wallet(data);

    // Seed an existing encrypted wallet with an old passphrase.
    din::Json created = dinero::rpc::RpcCreateHDWallet(
        make_create_params("default", 12, "", "", "bip86"), &wallet);
    assert_rpc_success(created);
    wallet.encryptWallet("old-passphrase");
    EXPECT_TRUE(wallet.isWalletEncrypted());
    EXPECT_TRUE(wallet.isWalletLocked());
    EXPECT_EQ(query_encryption_flag(db), 1);

    // Restore over the same wallet using GUI-like flow (no password in restore RPC).
    din::Json restored = dinero::rpc::RpcRestoreWallet(
        make_restore_params("default", mnemonic, "", "", "bip86"), &wallet);
    assert_rpc_success(restored);
    EXPECT_FALSE(restored.get("encrypted", false).asBool());
    EXPECT_EQ(query_encryption_flag(db), 0);

    // Completion step in GUI calls wallet.encrypt afterwards with a fresh passphrase.
    ASSERT_NO_THROW(wallet.encryptWallet("new-passphrase"));
    EXPECT_TRUE(wallet.isWalletEncrypted());
    EXPECT_TRUE(wallet.isWalletLocked());
    EXPECT_EQ(query_encryption_flag(db), 1);

    // Old passphrase must no longer unlock. New passphrase must work.
    EXPECT_THROW(wallet.unlockWallet("old-passphrase"), std::runtime_error);
    EXPECT_NO_THROW(wallet.unlockWallet("new-passphrase"));
    EXPECT_FALSE(wallet.isWalletLocked());

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
