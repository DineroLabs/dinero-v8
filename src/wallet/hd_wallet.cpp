#include "wallet/hd_wallet.h"
#include "wallet/hd_paths.h"
#include "wallet/bip39.h"
#include "wallet/bip32_deriver.h"  // Consolidated BIP32 derivation (replaces inline lambdas)
#include "wallet/taproot_keys.h"   // Canonical TapTweak (ComputeTweakedPubkey)
#include "wallet/utxo_index.h"  // CRITICAL: Need full definition for RegisterAddress()
#include "consensus/chainparams.h"
#include "consensus/coin_type.h"              // BIP32 derivation constants
#include "consensus/coinbase_maturity.h"      // P1: Coinbase maturity rules
#include "storage/chain_height_provider.h"    // P1: Chain height access (clean interface)
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef FFI_WALLET_ONLY
// iOS: No filesystem header needed
#include <sys/stat.h>  // For mkdir/stat
#else
#include <filesystem>
#endif
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <ctime>

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_recovery.h>
#include <secp256k1_schnorrsig.h>  // For Taproot Schnorr signatures
#include <openssl/crypto.h>  // For OPENSSL_cleanse
#include <openssl/hmac.h>    // For HMAC-SHA256 (Phase 7: revocation secrets)
#include <openssl/sha.h>     // For SHA256
#include <sqlite3.h>         // For wallet state persistence (wallet_state.db)

// Use project crypto (Bitcoin-like API)
#include "crypto/dinero_crypto_minimal.h"
#include "crypto/ripemd160.h"
#include "crypto/hash160.h"
#include "crypto/hd_keychain.h"  // For GetAccountXpub()

// Bech32 for address encoding
#include "external/bech32/bech32.hpp"

// Wallet encryption (PBKDF2-HMAC-SHA512 + AES-256-GCM)
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <algorithm>

// No longer need chain_db.h - using clean ChainHeightProvider interface instead

// Minimal Bech32 encode (segwit v0) – self-contained to avoid API drift
namespace bech32_local {
static const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static uint32_t polymod(const std::vector<uint8_t>& v) {
  uint32_t c = 1;
  for (auto v_i : v) {
    uint8_t c0 = c >> 25;
    c = ( (c & 0x1ffffff) << 5 ) ^ v_i;
    if (c0 & 1) c ^= 0x3b6a57b2;
    if (c0 & 2) c ^= 0x26508e6d;
    if (c0 & 4) c ^= 0x1ea119fa;
    if (c0 & 8) c ^= 0x3d4233dd;
    if (c0 & 16) c ^= 0x2a1462b3;
  }
  return c;
}
static std::vector<uint8_t> hrpExpand(const std::string& hrp) {
  std::vector<uint8_t> ret; ret.reserve(hrp.size()*2+1);
  for (char c : hrp) ret.push_back((uint8_t)(c >> 5));
  ret.push_back(0);
  for (char c : hrp) ret.push_back((uint8_t)(c & 31));
  return ret;
}
static bool convertbits(std::vector<uint8_t>& out, const uint8_t* in, size_t len, int from, int to, bool pad) {
  uint32_t acc = 0; int bits = 0; const uint32_t maxv = (1u<<to)-1;
  for (size_t i=0;i<len;i++) {
    acc = (acc << from) | in[i];
    bits += from;
    while (bits >= to) { bits -= to; out.push_back((acc >> bits) & maxv); }
  }
  if (pad) { if (bits) out.push_back((acc << (to-bits)) & maxv); }
  else if (bits >= from || ((acc << (to-bits)) & maxv)) return false;
  return true;
}
static std::string EncodeSegwitV0(const std::string& hrp, const uint8_t prog[20]) {
  std::vector<uint8_t> data; data.reserve(1+32);
  data.push_back(0); // version 0
  std::vector<uint8_t> five;
  convertbits(five, prog, 20, 8, 5, true);
  data.insert(data.end(), five.begin(), five.end());
  std::vector<uint8_t> values = hrpExpand(hrp); values.insert(values.end(), data.begin(), data.end()); values.insert(values.end(), {0,0,0,0,0,0});
  uint32_t pm = polymod(values) ^ 1;
  std::string out = hrp; out.push_back('1');
  for (auto d : data) out.push_back(CHARSET[d]);
  for (int i=0;i<6;i++) out.push_back(CHARSET[(pm >> (5*(5-i))) & 31]);
  return out;
}
} // namespace

// REMOVED: using namespace std; (causes namespace pollution with RocksDB headers)
#ifndef FFI_WALLET_ONLY
namespace fs = std::filesystem;
#endif

static inline uint32_t U32BE(uint32_t x){ return ((x>>24)&0xff)|((x>>8)&0xff00)|((x&0xff00)<<8)|((x&0xff)<<24); }
static inline uint32_t hardened(uint32_t i){ return 0x80000000u | i; }

// Encryption constants/helpers shared by legacy HDWallet state handling.
static constexpr uint32_t PBKDF2_ITERATIONS = 600000;  // Bitcoin Core style: ~1 second
static constexpr size_t KEY_SIZE = 32;
static constexpr size_t SALT_SIZE = 32;
static constexpr size_t NONCE_SIZE = 12;
static constexpr size_t TAG_SIZE = 16;
static constexpr char PASSWORD_VERIFIER_DOMAIN[] = "Dinero HDWallet password verifier v2";

static bool DeriveKey(const std::string& password,
                      const std::vector<uint8_t>& salt,
                      uint8_t key_out[32]);
static bool MakePasswordVerifier(const uint8_t key[32],
                                 std::vector<uint8_t>& verifier_out);
static bool VerifyPasswordMaterial(const std::vector<uint8_t>& stored_verifier,
                                   const uint8_t key[32],
                                   bool* legacy_key_format = nullptr);
static bool AES_Encrypt(const uint8_t* plaintext, size_t plaintext_len,
                        const uint8_t key[32], const uint8_t nonce[12],
                        uint8_t* ciphertext_out, uint8_t tag_out[16]);
static bool AES_Decrypt(const uint8_t* ciphertext, size_t ciphertext_len,
                        const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t tag[16], uint8_t* plaintext_out);
static bool SealSeedForStorage(const std::vector<uint8_t>& seed,
                               const std::string& password,
                               std::vector<uint8_t>& salt_out,
                               std::vector<uint8_t>& password_hash_out,
                               std::vector<uint8_t>& encrypted_seed_out);
static bool UnsealSeedFromStorage(const std::vector<uint8_t>& salt,
                                  const std::vector<uint8_t>& password_hash,
                                  const std::vector<uint8_t>& encrypted_seed,
                                  const std::string& password,
                                  std::vector<uint8_t>& seed_out);

static void Hash160(const uint8_t* data, size_t len, uint8_t out20[20]) {
  HASH160(data, len, out20);
}

static void HMAC512(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out64[64]) {
  hmac_sha512(key, klen, msg, mlen, out64);
}

// =============================================================================
// Derivation Path String Helpers
// =============================================================================
// Rule: Use DINERO_COIN_TYPE constant, never hardcode "1448"

/**
 * @brief Generate a BIP84/86 derivation path string
 * @param purpose 84 (BIP84/P2WPKH) or 86 (BIP86/P2TR)
 * @param account Account index (typically 0)
 * @param chain 0=external/receive, 1=internal/change, 2=mining
 * @param index Address index
 * @return Path string like "m/86'/1448'/0'/0/0"
 */
static std::string MakeDerivationPath(uint32_t purpose, uint32_t account, uint32_t chain, uint32_t index) {
    return "m/" + std::to_string(purpose) + "'/" +
           std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/" +
           std::to_string(account) + "'/" +
           std::to_string(chain) + "/" +
           std::to_string(index);
}

/**
 * @brief Generate a BIP84/86 derivation path string without final index (for display)
 * @param purpose 84 or 86
 * @param account Account index
 * @param chain Chain index
 * @return Path string like "m/86'/1448'/0'/0/"
 */
static std::string MakeDerivationPathPrefix(uint32_t purpose, uint32_t account, uint32_t chain) {
    return "m/" + std::to_string(purpose) + "'/" +
           std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/" +
           std::to_string(account) + "'/" +
           std::to_string(chain) + "/";
}

static bool IsInternalChangePath(const std::string& path) {
    if (path.size() < 6 || path[0] != 'm' || path[1] != '/') {
        return false;
    }

    std::vector<std::string> segments;
    std::stringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        segments.push_back(segment);
    }

    if (segments.size() < 6) {
        return false;
    }

    return segments[4] == "1";
}

HDWallet::HDWallet(std::string wdir, uint32_t coin_type, bool enable_autolock)
  : wallet_dir_(std::move(wdir)), coin_type_(coin_type), autolock_running_(false), unlock_time_(0) {
#ifdef FFI_WALLET_ONLY
  wallet_file_ = wallet_dir_ + "/wallet_state.db";
#else
  wallet_file_ = (fs::path(wallet_dir_) / "wallet_state.db").string();
#endif

  // Start auto-lock thread only if enabled (disabled for tests to prevent hangs)
  if (enable_autolock) {
    autolock_running_ = true;
    autolock_thread_ = std::thread(&HDWallet::AutoLockThread, this);
  }
}

HDWallet::~HDWallet() {
  // Stop auto-lock thread
  autolock_running_ = false;
  if (autolock_thread_.joinable()) {
    autolock_thread_.join();
  }
  
  // Clear sensitive data from memory
  if (!seed_.empty()) {
    std::fill(seed_.begin(), seed_.end(), 0);
  }
}

std::unique_ptr<HDWallet> HDWallet::Open(const std::string& datadir, uint32_t coin_type, bool enable_autolock) {
#ifdef FFI_WALLET_ONLY
  // iOS: Create directory using mkdir
  struct stat info;
  if (stat(datadir.c_str(), &info) != 0) {
    mkdir(datadir.c_str(), 0755);
  }
#else
  fs::create_directories(datadir);
#endif
  auto w = std::unique_ptr<HDWallet>(new HDWallet(datadir, coin_type, enable_autolock));
  w->LoadOrCreate();
  return w;
}

HDWallet::LegacyMigrationResult HDWallet::MigrateLegacySidecarToStateDb(
    const std::string& datadir,
    uint32_t coin_type,
    bool create_backup,
    bool overwrite_existing) {
  LegacyMigrationResult result;

  auto fail = [&](const std::string& message) {
    result.success = false;
    result.message = message;
    return result;
  };

  auto join_path = [](const std::string& base, const std::string& leaf) -> std::string {
    if (base.empty()) return leaf;
    const char last = base.back();
    if (last == '/' || last == '\\') {
      return base + leaf;
    }
    return base + "/" + leaf;
  };

  auto file_exists = [](const std::string& path) -> bool {
#ifdef FFI_WALLET_ONLY
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
#else
    return fs::exists(path);
#endif
  };

  auto ensure_dir = [](const std::string& path) -> bool {
#ifdef FFI_WALLET_ONLY
    struct stat info {};
    if (stat(path.c_str(), &info) == 0) {
      return true;
    }
    return mkdir(path.c_str(), 0700) == 0;
#else
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec;
#endif
  };

  result.wallet_state_path = join_path(datadir, "wallet_state.db");
  result.legacy_wallet_conf_path = join_path(datadir, "wallet.conf");

  auto count_wallet_state_rows = [&](const std::string& state_db_path, std::string& error_out) -> int {
    if (!file_exists(state_db_path)) {
      return 0;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(state_db_path.c_str(), &db) != SQLITE_OK) {
      error_out = "Failed to open wallet_state.db: " + std::string(sqlite3_errmsg(db));
      if (db) sqlite3_close(db);
      return -1;
    }

    int rows = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM wallet_state";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      // No wallet_state table yet means migration has not been run.
      sqlite3_close(db);
      return 0;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
      rows = sqlite3_column_int(stmt, 0);
    } else {
      error_out = "Failed to read wallet_state row count: " + std::string(sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return -1;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
  };

  std::string row_count_error;
  const int existing_rows = count_wallet_state_rows(result.wallet_state_path, row_count_error);
  if (existing_rows < 0) {
    return fail(row_count_error);
  }

  const bool has_legacy_conf = file_exists(result.legacy_wallet_conf_path);
  if (!has_legacy_conf) {
    if (existing_rows > 0) {
      result.success = true;
      result.already_migrated = true;
      result.message = "wallet_state.db already populated; no legacy wallet.conf present";
      return result;
    }
    return fail("Legacy wallet.conf not found at: " + result.legacy_wallet_conf_path);
  }

  if (existing_rows > 0 && !overwrite_existing) {
    result.success = true;
    result.already_migrated = true;
    result.message = "wallet_state.db already populated; skipping legacy import";
    return result;
  }

  auto bytes = ReadFile(result.legacy_wallet_conf_path);
  if (bytes.empty()) {
    return fail("Legacy wallet.conf is empty or unreadable: " + result.legacy_wallet_conf_path);
  }

  HDWallet migrated_wallet(datadir, coin_type, false);

  // Reset to deterministic default state before parsing legacy sidecar.
  migrated_wallet.encrypted_ = false;
  migrated_wallet.locked_ = false;
  migrated_wallet.index_ = 0;
  migrated_wallet.change_index_ = 0;
  migrated_wallet.mining_index_ = 0;
  migrated_wallet.taproot_index_ = 0;
  migrated_wallet.taproot_change_index_ = 0;
  migrated_wallet.taproot_mining_index_ = 0;
  migrated_wallet.seed_.clear();
  migrated_wallet.mnemonic_.clear();
  migrated_wallet.password_salt_.clear();
  migrated_wallet.password_hash_.clear();
  migrated_wallet.encrypted_seed_.clear();

  auto parse_kv = [&](const std::string& key, const std::string& value) {
    if (key == "encrypted") {
      migrated_wallet.encrypted_ = (value == "1");
    } else if (key == "next_index") {
      migrated_wallet.index_ = static_cast<uint32_t>(std::stoul(value));
    } else if (key == "next_change_index") {
      migrated_wallet.change_index_ = static_cast<uint32_t>(std::stoul(value));
    } else if (key == "next_mining_index") {
      migrated_wallet.mining_index_ = static_cast<uint32_t>(std::stoul(value));
    } else if (key == "next_taproot_index") {
      migrated_wallet.taproot_index_ = static_cast<uint32_t>(std::stoul(value));
    } else if (key == "next_taproot_change_index") {
      migrated_wallet.taproot_change_index_ = static_cast<uint32_t>(std::stoul(value));
    } else if (key == "next_taproot_mining_index") {
      migrated_wallet.taproot_mining_index_ = static_cast<uint32_t>(std::stoul(value));
    } else if (key == "mnemonic") {
      migrated_wallet.mnemonic_ = value;
    } else if (key == "seed_hex") {
      FromHex(value, migrated_wallet.seed_);
    } else if (key == "password_salt") {
      FromHex(value, migrated_wallet.password_salt_);
    } else if (key == "password_hash") {
      FromHex(value, migrated_wallet.password_hash_);
    } else if (key == "encrypted_seed") {
      FromHex(value, migrated_wallet.encrypted_seed_);
    }
  };

  try {
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    std::string line;
    while (std::getline(in, line)) {
      // std::getline strips '\n' but leaves a trailing '\r' when the file
      // was written with CRLF line endings — typical for any wallet.conf
      // produced on Windows or edited in Notepad/Notepad++/VS Code with
      // default settings. Drop it so values like seed_hex parse cleanly.
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const auto p = line.find('=');
      if (p == std::string::npos) {
        continue;
      }
      parse_kv(line.substr(0, p), line.substr(p + 1));
    }
  } catch (const std::exception& e) {
    return fail("Failed to parse wallet.conf: " + std::string(e.what()));
  }

  if (migrated_wallet.encrypted_) {
    if (migrated_wallet.encrypted_seed_.empty() ||
        migrated_wallet.password_salt_.empty() ||
        migrated_wallet.password_hash_.empty()) {
      return fail("wallet.conf is corrupted (missing encrypted wallet fields)");
    }
    migrated_wallet.locked_ = true;
  } else {
    if (migrated_wallet.seed_.size() != 64) {
      return fail("wallet.conf is corrupted (seed_hex is invalid or wrong length)");
    }
    migrated_wallet.locked_ = false;
  }

  if (create_backup) {
    const std::string backup_dir = join_path(datadir, "backups");
    if (!ensure_dir(backup_dir)) {
      return fail("Failed to create backup directory: " + backup_dir);
    }

    std::time_t now = std::time(nullptr);
    std::tm tm_snapshot {};
#ifdef _WIN32
    localtime_s(&tm_snapshot, &now);
#else
    localtime_r(&now, &tm_snapshot);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_snapshot);
    result.backup_path = join_path(backup_dir, std::string("wallet.conf.") + ts + ".bak");
    WriteFile(result.backup_path, std::string(bytes.begin(), bytes.end()));
  }

  try {
    migrated_wallet.Save();
  } catch (const std::exception& e) {
    return fail("Failed to write wallet_state.db: " + std::string(e.what()));
  }

  result.success = true;
  result.migrated = true;
  result.message = "Legacy wallet.conf imported into wallet_state.db";
  return result;
}

std::unique_ptr<HDWallet> HDWallet::CreateNew(const std::string& datadir, uint32_t coin_type, std::string& mnemonic_out, bool enable_autolock) {
#ifdef FFI_WALLET_ONLY
  // iOS: Create directory using mkdir
  struct stat info;
  if (stat(datadir.c_str(), &info) != 0) {
    mkdir(datadir.c_str(), 0755);
  }
#else
  fs::create_directories(datadir);
#endif

  // Generate BIP39 mnemonic (12 words = 128 bits entropy)
  mnemonic_out = dinero::bip39::Generate(dinero::bip39::WordCount::Words12);
  if (mnemonic_out.empty()) {
    throw std::runtime_error("Failed to generate BIP39 mnemonic");
  }

  // Convert mnemonic to seed
  std::vector<uint8_t> seed;
  if (!dinero::bip39::MnemonicToSeed(mnemonic_out, "", seed)) {
    throw std::runtime_error("Failed to convert mnemonic to seed");
  }

  if (seed.size() != 64) {
    throw std::runtime_error("Invalid seed size");
  }

  // Create wallet
  auto w = std::unique_ptr<HDWallet>(new HDWallet(datadir, coin_type, enable_autolock));
  w->seed_ = seed;
  w->mnemonic_ = mnemonic_out;  // Store the mnemonic for backup
  w->index_ = 0;
  w->Save();

  return w;
}

std::unique_ptr<HDWallet> HDWallet::Restore(const std::string& datadir, uint32_t coin_type, const std::string& mnemonic, const std::string& passphrase, bool enable_autolock) {
  // Validate mnemonic
  if (!dinero::bip39::ValidateMnemonic(mnemonic)) {
    throw std::runtime_error("Invalid BIP39 mnemonic");
  }

  // Convert to seed
  std::vector<uint8_t> seed;
  if (!dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed)) {
    throw std::runtime_error("Failed to convert mnemonic to seed");
  }

  if (seed.size() != 64) {
    throw std::runtime_error("Invalid seed size");
  }

#ifdef FFI_WALLET_ONLY
  // iOS: Create directory using mkdir
  struct stat info;
  if (stat(datadir.c_str(), &info) != 0) {
    mkdir(datadir.c_str(), 0755);
  }
#else
  fs::create_directories(datadir);
#endif

  // Create or overwrite wallet
  auto w = std::unique_ptr<HDWallet>(new HDWallet(datadir, coin_type, enable_autolock));
  w->seed_ = seed;
  w->mnemonic_ = mnemonic;  // Store the mnemonic for backup
  w->index_ = 0;
  w->Save();

  return w;
}

void HDWallet::LoadOrCreate() {
  auto reset_state = [&]() {
    encrypted_ = false;
    locked_ = false;
    index_ = 0;
    change_index_ = 0;
    mining_index_ = 0;
    taproot_index_ = 0;
    taproot_change_index_ = 0;
    taproot_mining_index_ = 0;
    seed_.clear();
    mnemonic_.clear();
    password_salt_.clear();
    password_hash_.clear();
    encrypted_seed_.clear();
  };

  auto parse_kv = [&](const std::string& k, const std::string& v) {
    if (k == "encrypted") {
      encrypted_ = (v == "1");
    } else if (k == "next_index") {
      index_ = static_cast<uint32_t>(std::stoul(v));
    } else if (k == "next_change_index") {
      change_index_ = static_cast<uint32_t>(std::stoul(v));
    } else if (k == "next_mining_index") {
      mining_index_ = static_cast<uint32_t>(std::stoul(v));
    } else if (k == "next_taproot_index") {
      taproot_index_ = static_cast<uint32_t>(std::stoul(v));
    } else if (k == "next_taproot_change_index") {
      taproot_change_index_ = static_cast<uint32_t>(std::stoul(v));
    } else if (k == "next_taproot_mining_index") {
      taproot_mining_index_ = static_cast<uint32_t>(std::stoul(v));
    } else if (k == "coin_type") {
      /* ignore mismatch at runtime */
    } else if (k == "mnemonic") {
      mnemonic_ = v;
    } else if (k == "seed_hex") {
      // Plaintext seed (legacy unencrypted mode)
      FromHex(v, seed_);
    } else if (k == "password_salt") {
      FromHex(v, password_salt_);
    } else if (k == "password_hash") {
      FromHex(v, password_hash_);
    } else if (k == "encrypted_seed") {
      FromHex(v, encrypted_seed_);
    }
  };

  auto validate_loaded_state = [&]() {
    if (encrypted_) {
      if (encrypted_seed_.empty() || password_salt_.empty() || password_hash_.empty()) {
        throw std::runtime_error("wallet: corrupted encrypted wallet");
      }
      locked_ = true;  // Encrypted wallets start locked
      std::cout << "🔐 Encrypted wallet loaded (locked)" << std::endl;
    } else {
      // Legacy plaintext state (seed_hex) remains readable for compatibility.
      if (!seed_.empty()) {
        if (seed_.size() != 64) {
          throw std::runtime_error("wallet: bad seed");
        }
        locked_ = false;
        std::cout << "⚠️  Plaintext wallet loaded (unencrypted)" << std::endl;
        return;
      }

      // Preferred unencrypted-at-rest format: encrypted_seed + salt/hash with empty passphrase.
      if (!UnsealSeedFromStorage(password_salt_, password_hash_, encrypted_seed_, "", seed_)) {
        throw std::runtime_error("wallet: bad seed");
      }
      if (seed_.size() != 64) {
        throw std::runtime_error("wallet: bad seed length after decrypt");
      }

      locked_ = false;
      std::cout << "🔐 Unencrypted wallet loaded (seed encrypted-at-rest)" << std::endl;
    }
  };

  auto create_fresh_wallet = [&]() {
    mnemonic_ = dinero::bip39::Generate(dinero::bip39::WordCount::Words12);
    if (mnemonic_.empty()) {
      throw std::runtime_error("Failed to generate BIP39 mnemonic");
    }

    std::vector<uint8_t> seed;
    if (!dinero::bip39::MnemonicToSeed(mnemonic_, "", seed)) {
      throw std::runtime_error("Failed to convert mnemonic to seed");
    }
    if (seed.size() != 64) {
      throw std::runtime_error("Invalid seed size");
    }

    seed_ = seed;
    encrypted_ = false;
    locked_ = false;
    Save();
  };

  auto legacy_conf_exists = [&](const std::string& legacy_wallet_file) -> bool {
#ifdef FFI_WALLET_ONLY
    struct stat info;
    return stat(legacy_wallet_file.c_str(), &info) == 0;
#else
    return fs::exists(legacy_wallet_file);
#endif
  };

  reset_state();

  bool state_db_exists = false;
#ifdef FFI_WALLET_ONLY
  struct stat info;
  state_db_exists = (stat(wallet_file_.c_str(), &info) == 0);
#else
  state_db_exists = fs::exists(wallet_file_);
#endif

  if (!state_db_exists) {
#ifdef FFI_WALLET_ONLY
    const std::string legacy_wallet_file = wallet_dir_ + "/wallet.conf";
#else
    const std::string legacy_wallet_file = (fs::path(wallet_dir_) / "wallet.conf").string();
#endif
    if (legacy_conf_exists(legacy_wallet_file)) {
      throw std::runtime_error(
          "Legacy wallet.conf detected but runtime import is disabled. "
          "Migrate wallet.conf to wallet_state.db using an explicit migration flow.");
    }
    create_fresh_wallet();
    return;
  }

  sqlite3* db = nullptr;
  if (sqlite3_open(wallet_file_.c_str(), &db) != SQLITE_OK) {
    const std::string err = db ? sqlite3_errmsg(db) : "unknown sqlite open error";
    if (db) {
      sqlite3_close(db);
    }
    throw std::runtime_error("Failed to open wallet_state.db: " + err);
  }

  char* err_msg = nullptr;
  const char* create_sql =
      "CREATE TABLE IF NOT EXISTS wallet_state ("
      "key TEXT PRIMARY KEY, "
      "value TEXT NOT NULL"
      ");";
  if (sqlite3_exec(db, create_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
    const std::string err = err_msg ? err_msg : sqlite3_errmsg(db);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    throw std::runtime_error("Failed to initialize wallet_state table: " + err);
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT key, value FROM wallet_state;", -1, &stmt, nullptr) != SQLITE_OK) {
    const std::string err = sqlite3_errmsg(db);
    sqlite3_close(db);
    throw std::runtime_error("Failed to query wallet_state: " + err);
  }

  int row_count = 0;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!key || !value) {
      continue;
    }
    ++row_count;
    parse_kv(key, value);
  }

  if (rc != SQLITE_DONE) {
    const std::string err = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    throw std::runtime_error("Failed while reading wallet_state rows: " + err);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);

  if (row_count == 0) {
    create_fresh_wallet();
    return;
  }

  validate_loaded_state();
}

void HDWallet::Save() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(wallet_file_.c_str(), &db) != SQLITE_OK) {
    const std::string err = db ? sqlite3_errmsg(db) : "unknown sqlite open error";
    if (db) {
      sqlite3_close(db);
    }
    throw std::runtime_error("Failed to open wallet_state.db for save: " + err);
  }

  char* err_msg = nullptr;
  const char* create_sql =
      "CREATE TABLE IF NOT EXISTS wallet_state ("
      "key TEXT PRIMARY KEY, "
      "value TEXT NOT NULL"
      ");";
  if (sqlite3_exec(db, create_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
    const std::string err = err_msg ? err_msg : sqlite3_errmsg(db);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    throw std::runtime_error("Failed to initialize wallet_state table: " + err);
  }

  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    const std::string err = err_msg ? err_msg : sqlite3_errmsg(db);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    throw std::runtime_error("Failed to begin wallet_state transaction: " + err);
  }

  auto rollback_and_throw = [&](const std::string& msg) {
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    throw std::runtime_error(msg);
  };

  if (sqlite3_exec(db, "DELETE FROM wallet_state;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    const std::string err = err_msg ? err_msg : sqlite3_errmsg(db);
    sqlite3_free(err_msg);
    rollback_and_throw("Failed to clear wallet_state table: " + err);
  }

  auto insert_kv = [&](const std::string& key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "INSERT OR REPLACE INTO wallet_state(key, value) VALUES(?, ?);",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
      rollback_and_throw("Failed to prepare wallet_state insert: " + std::string(sqlite3_errmsg(db)));
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      rollback_and_throw("Failed to insert wallet_state key '" + key + "': " + std::string(sqlite3_errmsg(db)));
    }
    sqlite3_finalize(stmt);
  };

  insert_kv("encrypted", encrypted_ ? "1" : "0");
  insert_kv("coin_type", std::to_string(coin_type_));
  insert_kv("next_index", std::to_string(index_));
  insert_kv("next_change_index", std::to_string(change_index_));
  insert_kv("next_mining_index", std::to_string(mining_index_));
  insert_kv("next_taproot_index", std::to_string(taproot_index_));
  insert_kv("next_taproot_change_index", std::to_string(taproot_change_index_));
  insert_kv("next_taproot_mining_index", std::to_string(taproot_mining_index_));

  // Never persist plaintext seed or mnemonic. For unencrypted wallets, seal seed
  // with an empty passphrase so runtime semantics remain unchanged.
  std::vector<uint8_t> persist_salt;
  std::vector<uint8_t> persist_hash;
  std::vector<uint8_t> persist_encrypted_seed;

  if (encrypted_) {
    if (password_salt_.empty() || password_hash_.empty() || encrypted_seed_.empty()) {
      rollback_and_throw("Encrypted wallet missing encryption material");
    }
    persist_salt = password_salt_;
    persist_hash = password_hash_;
    persist_encrypted_seed = encrypted_seed_;
  } else {
    if (!SealSeedForStorage(seed_, "", persist_salt, persist_hash, persist_encrypted_seed)) {
      rollback_and_throw("Failed to seal unencrypted seed for wallet_state persistence");
    }
  }

  insert_kv("password_salt", ToHex(persist_salt.data(), persist_salt.size()));
  insert_kv("password_hash", ToHex(persist_hash.data(), persist_hash.size()));
  insert_kv("encrypted_seed", ToHex(persist_encrypted_seed.data(), persist_encrypted_seed.size()));

  if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    const std::string err = err_msg ? err_msg : sqlite3_errmsg(db);
    sqlite3_free(err_msg);
    rollback_and_throw("Failed to commit wallet_state transaction: " + err);
  }

  sqlite3_close(db);
}

std::string HDWallet::DeriveNextAddress() {
  // Default: BIP86 Taproot (P2TR) — delegates to DeriveNextTaprootAddress()
  // Legacy BIP84 P2WPKH addresses available via DeriveAddressAt() for old wallets
  return DeriveNextTaprootAddress();
}

std::string HDWallet::DeriveNextChangeAddress() {
  // Default: BIP86 Taproot (P2TR) — delegates to DeriveNextTaprootChangeAddress()
  // Legacy BIP84 P2WPKH change available via DeriveChangeAddressAt() for old wallets
  return DeriveNextTaprootChangeAddress();
}

std::string HDWallet::DeriveNextMiningAddress() {
  // Default: BIP86 Taproot (P2TR) — delegates to DeriveNextTaprootMiningAddress()
  // Legacy BIP84 P2WPKH mining available via DeriveMiningAddressAt() for old wallets
  return DeriveNextTaprootMiningAddress();
}

std::string HDWallet::DeriveAddressAt(uint32_t index) const {
  // BIP84: m/84'/coin_type'/0'/0/index (P2WPKH receive)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(84);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(0);      // Chain 0 (receive)
  deriver.deriveNormal(index);

  // Compute HASH160(compressed pubkey)
  auto pub = deriver.getCompressedPubkey();
  uint8_t prog20[20];
  Hash160(pub.data(), pub.size(), prog20);

  // Bech32 with correct HRP for active network
  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";
  }

  return bech32_local::EncodeSegwitV0(hrp, prog20);
}

std::string HDWallet::DeriveChangeAddressAt(uint32_t index) const {
  // BIP84: m/84'/coin_type'/0'/1/index (P2WPKH change)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(84);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(1);      // Chain 1 (change)
  deriver.deriveNormal(index);

  // Compute HASH160(compressed pubkey)
  auto pub = deriver.getCompressedPubkey();
  uint8_t prog20[20];
  Hash160(pub.data(), pub.size(), prog20);

  // Bech32 with correct HRP for active network
  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";
  }

  return bech32_local::EncodeSegwitV0(hrp, prog20);
}

std::string HDWallet::DeriveMiningAddressAt(uint32_t index) const {
  // BIP84: m/84'/coin_type'/0'/2/index (P2WPKH mining)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(84);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(2);      // Chain 2 (mining)
  deriver.deriveNormal(index);

  // Compute HASH160(compressed pubkey)
  auto pub = deriver.getCompressedPubkey();
  uint8_t prog20[20];
  Hash160(pub.data(), pub.size(), prog20);

  // Bech32 with correct HRP for active network
  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";
  }

  return bech32_local::EncodeSegwitV0(hrp, prog20);
}

// ===== file utils / hex / rng =====
std::vector<uint8_t> HDWallet::ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void HDWallet::WriteFile(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary|std::ios::trunc);
  f.write(content.data(), (std::streamsize)content.size());
}
std::string HDWallet::ToHex(const uint8_t* d, size_t n) {
  static const char* hexd="0123456789abcdef"; std::string out; out.resize(n*2);
  for (size_t i=0;i<n;i++){ out[2*i]=hexd[(d[i]>>4)&0xF]; out[2*i+1]=hexd[d[i]&0xF]; }
  return out;
}
bool HDWallet::FromHex(const std::string& hex, std::vector<uint8_t>& out) {
  if (hex.size()%2) return false; out.resize(hex.size()/2);
  auto val=[&](char c)->int{ if('0'<=c&&c<='9')return c-'0'; if('a'<=c&&c<='f')return c-'a'+10; if('A'<=c&&c<='F')return c-'A'+10; return -1; };
  for (size_t i=0;i<out.size();i++){ int hi=val(hex[2*i]), lo=val(hex[2*i+1]); if(hi<0||lo<0) return false; out[i]=(uint8_t)((hi<<4)|lo); }
  return true;
}
bool HDWallet::GetRandomBytes(uint8_t* out, size_t n) {
  FILE* f = fopen("/dev/urandom","rb");
  if (!f) return false;
  size_t r = fread(out,1,n,f); fclose(f);
  return r==n;
}

// ===== NEW: Blockchain integration =====
#include "wallet/utxo_index.h"
#include "wallet/coin_selection.h"
#include "wallet/bip143_signer.h"
#include "wallet/taproot_tx_signer.h"  // BIP341 Taproot signing
#include "bech32/bech32.hpp"

std::string HDWallet::GetAddressAt(uint32_t index) const {
  return DeriveAddressAt(index);
}

std::string HDWallet::GetChangeAddressAt(uint32_t index) const {
  return DeriveChangeAddressAt(index);
}

std::string HDWallet::GetMiningAddressAt(uint32_t index) {
  return DeriveMiningAddressAt(index);
}

std::vector<std::string> HDWallet::GetAllAddresses() {
  std::vector<std::string> addresses;
  for (uint32_t i = 0; i < index_; i++) {
    addresses.push_back(DeriveAddressAt(i));
  }
  return addresses;
}

// UTXOIndex connection methods
void HDWallet::ConnectUTXOIndex(dinero::UTXOIndex* utxo_index) {
  utxo_index_ = utxo_index;
  std::cout << "✅ HDWallet connected to UTXOIndex" << std::endl;
}

// P1: ChainHeightProvider connection for chain height access
void HDWallet::ConnectChainHeightProvider(dinero::ChainHeightProvider* provider) {
  chain_height_provider_ = provider;
  std::cout << "✅ HDWallet connected to ChainHeightProvider (for maturity checks)" << std::endl;
}

void HDWallet::RegisterAddresses() {
  if (!utxo_index_) {
    std::cerr << "❌ No UTXO index connected" << std::endl;
    return;
  }

  // ═══════════════════════════════════════════════════════════════════════════
  // LEGACY POOL: BIP84 P2WPKH addresses (for backward compatibility)
  // These addresses exist only for scanning historical wallets.
  // New addresses should NOT be added to this pool.
  // ═══════════════════════════════════════════════════════════════════════════

  // LEGACY: Register existing BIP84 P2WPKH receive addresses
  address_to_index_.clear();
  for (uint32_t i = 0; i < index_; i++) {
    std::string addr = DeriveAddressAt(i);
    address_to_index_[addr] = i;
    auto script = AddressToScriptPubKey(addr);
    std::string derivation_path = MakeDerivationPath(84, 0, 0, i);
    utxo_index_->RegisterAddress(script, derivation_path);
  }

  // LEGACY: Register existing BIP84 P2WPKH change addresses
  change_address_to_index_.clear();
  for (uint32_t i = 0; i < change_index_; i++) {
    std::string addr = DeriveChangeAddressAt(i);
    change_address_to_index_[addr] = i;
    auto script = AddressToScriptPubKey(addr);
    std::string derivation_path = MakeDerivationPath(84, 0, 1, i);
    utxo_index_->RegisterAddress(script, derivation_path);
  }

  // ═══════════════════════════════════════════════════════════════════════════
  // PRIMARY POOL: BIP86 Taproot addresses (CONSTITUTIONAL)
  // These are the authoritative wallet addresses.
  // All new wallets should use only this pool.
  // ═══════════════════════════════════════════════════════════════════════════

  // PRIMARY: Register BIP86 Taproot receive addresses
  for (uint32_t i = 0; i < taproot_index_; i++) {
    std::string addr = GetTaprootAddressAt(i);
    auto script = AddressToScriptPubKey(addr);
    std::string derivation_path = MakeDerivationPath(86, 0, 0, i);
    utxo_index_->RegisterAddress(script, derivation_path);
  }

  // PRIMARY: Register BIP86 Taproot change addresses
  for (uint32_t i = 0; i < taproot_change_index_; i++) {
    std::string addr = GetTaprootChangeAddressAt(i);
    auto script = AddressToScriptPubKey(addr);
    std::string derivation_path = MakeDerivationPath(86, 0, 1, i);
    utxo_index_->RegisterAddress(script, derivation_path);
  }

  std::cout << "✅ Registered " << address_to_index_.size() << " P2WPKH (legacy) + "
            << change_address_to_index_.size() << " P2WPKH change (legacy) + "
            << taproot_index_ << " Taproot (primary) + "
            << taproot_change_index_ << " Taproot change (primary) addresses" << std::endl;
}

// Real implementations using UTXOIndex with P1 coinbase maturity checks
WalletBalance HDWallet::GetBalance() const {
  WalletBalance balance;

  if (!utxo_index_) {
    return balance;  // Returns all zeros
  }

  // Get current chain height for maturity calculations
  uint32_t current_height = 0;
  if (chain_height_provider_ && chain_height_provider_->IsAvailable()) {
    current_height = chain_height_provider_->GetBestHeight();
  }

  auto utxos = utxo_index_->GetUnspentUTXOs();
  for (const auto& utxo : utxos) {
    // Phase M.6.2: Extract raw value for balance accumulation (WalletBalance still uses uint64_t)
    if (utxo.is_coinbase) {
      // Check coinbase maturity (needs 100 confirmations)
      if (current_height == 0) {
        // No chain height available, treat as immature
        balance.immature += utxo.value.GetUna();
      } else if (dinero::CoinbaseMaturity::isCoinbaseMature(static_cast<uint32_t>(utxo.height), current_height)) {
        // Coinbase is mature, add to confirmed balance
        balance.confirmed += utxo.value.GetUna();
      } else {
        // Coinbase not yet mature
        balance.immature += utxo.value.GetUna();
      }
    } else {
      // Regular (non-coinbase) UTXO - always confirmed if in UTXO set
      balance.confirmed += utxo.value.GetUna();
    }
  }

  balance.total = balance.confirmed + balance.immature;
  return balance;
}

// ═══════════════════════════════════════════════════════════════════════════
// COMPILE-TIME BOUNDARY GUARD: ListUTXOs() and CreateTransaction()
// ═══════════════════════════════════════════════════════════════════════════
// Each method MUST have exactly ONE implementation in this file.
// If you see duplicate definition errors, a second implementation was added.
//
// ListUTXOs: Keep blockchain_ version (filters with IsOurScript())
// CreateTransaction: Keep the implementation at line ~1079 below
//
// DELETED: First CreateTransaction (VIOLATED Phase M.3 lock)
//   - Used utxo.address (doesn't exist in CanonicalWalletUTXO)
//   - Stored derived fields instead of computing at boundaries
//
// Canonical implementations below use CanonicalWalletUTXO (Phase M.3 lock).
// ═══════════════════════════════════════════════════════════════════════════

#if 0 // LEGACY: SimpleBlockchain-dependent methods disabled after SimpleBlockchain removal
void HDWallet::ConnectBlockchain(SimpleBlockchain* blockchain) {
  blockchain_ = blockchain;
  std::cout << "✅ HDWallet connected to blockchain" << std::endl;
}

void HDWallet::ScanBlockchain() {
  if (!blockchain_) {
    std::cerr << "❌ No blockchain connected" << std::endl;
    return;
  }
  
  auto* utxo_set = blockchain_->get_utxo_set();
  if (!utxo_set) {
    std::cerr << "❌ No UTXO set available" << std::endl;
    return;
  }
  
  // Build address cache and register with UTXO index
  address_to_index_.clear();
  for (uint32_t i = 0; i < index_; i++) {
    std::string addr = DeriveAddressAt(i);
    address_to_index_[addr] = i;
    
    // CRITICAL: Register scriptPubKey with UTXO index
    auto script = AddressToScriptPubKey(addr);
    std::string derivation_path = MakeDerivationPath(84, 0, 0, i);
    utxo_set->RegisterAddress(script, derivation_path);
  }
  
  std::cout << "✅ Scanned & registered " << address_to_index_.size() << " addresses with UTXO index" << std::endl;
}

WalletBalance HDWallet::GetBalance() const {
  WalletBalance balance;
  
  if (!blockchain_) {
    return balance;
  }
  
  auto* utxo_set = blockchain_->get_utxo_set();
  if (!utxo_set) {
    return balance;
  }
  
  uint32_t current_height = blockchain_->get_height();
  
  // CRITICAL FIX: Only sum UTXOs that belong to our wallet addresses
  auto all_utxos = utxo_set->GetUnspentUTXOs();
  
  for (const auto& utxo : all_utxos) {
    // Check if this UTXO belongs to one of our addresses
    // by checking if the scriptPubKey matches any of our addresses
    auto opt_path = utxo_set->IsOurScript(utxo.spk);
    
    if (opt_path.has_value()) {
      // This UTXO belongs to us!
      uint32_t confirmations = 0;
      if (utxo.height > 0 && current_height >= static_cast<uint32_t>(utxo.height)) {
        confirmations = current_height - utxo.height + 1;
      }
      
      // Detect if coinbase (would need proper detection)
      // For now, assume height 0 or height 1 are coinbase
      bool is_coinbase = (utxo.height <= 1);
      
      if (confirmations == 0) {
        // Unconfirmed
        balance.unconfirmed += utxo.value;
      } else if (is_coinbase && confirmations < 100) {
        // Immature coinbase
        balance.immature += utxo.value;
      } else {
        // Confirmed and spendable
        balance.confirmed += utxo.value;
      }
    }
  }
  
  balance.total = balance.confirmed + balance.unconfirmed + balance.immature;

  return balance;
}
#endif // End of GetBalance (part of legacy SimpleBlockchain methods)

// ═══════════════════════════════════════════════════════════════════════════
// ACTIVE METHOD: ListUTXOs (used by CreatePSBT and wallet balance queries)
// Wallet → UTXOIndex architecture (NOT Wallet → Blockchain → UTXOSet)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<dinero::CanonicalWalletUTXO> HDWallet::ListUTXOs(uint32_t min_confirmations, bool allow_unconfirmed_change) const {
  std::vector<dinero::CanonicalWalletUTXO> result;

  if (!utxo_index_) {
    return result; // No UTXO index connected
  }

  // Get current chain tip for confirmation calculations
  uint32_t current_height = 0;
  if (chain_height_provider_) {
    current_height = chain_height_provider_->GetBestHeight();
  }

  // Query wallet's UTXO index (wallet-owned, not consensus)
  auto wallet_utxos = utxo_index_->GetUnspentUTXOs();

  for (const auto& utxo : wallet_utxos) {
    // Calculate confirmations (not stored, computed at boundary)
    uint32_t confirmations = 0;
    if (utxo.height > 0 && current_height >= static_cast<uint32_t>(utxo.height)) {
      confirmations = current_height - static_cast<uint32_t>(utxo.height) + 1;
    }

    // Filter by minimum confirmations
    const bool is_unconfirmed_change =
        allow_unconfirmed_change &&
        confirmations == 0 &&
        !utxo.is_coinbase &&
        IsInternalChangePath(utxo.path);

    if (confirmations < min_confirmations && !is_unconfirmed_change) {
      continue;
    }

    // Apply coinbase maturity rule (100 confirmations)
    if (utxo.is_coinbase && confirmations < 100) {
      continue;
    }

    // CT outputs must be spent via ring transactions, not transparent sends.
    // Exclude them here so the transparent coin selector never touches them.
    if (utxo.is_confidential) {
      continue;
    }

    // Convert WalletUTXO → CanonicalWalletUTXO (types are nearly identical)
    dinero::CanonicalWalletUTXO canonical;
    canonical.txid = utxo.txid.AsUint256();  // Phase M.4: Unwrap TxId to uint256
    canonical.vout = utxo.vout;
    // Phase M.6.2: Both are now AmountUna, direct assignment
    canonical.value = utxo.value;
    canonical.spk = utxo.spk;
    canonical.height = static_cast<uint32_t>(utxo.height); // int → uint32_t
    canonical.is_coinbase = utxo.is_coinbase;
    canonical.path = utxo.path;
    canonical.is_confidential = utxo.is_confidential;
    canonical.commitment = utxo.commitment;

    result.push_back(canonical);
  }

  return result;
}

bool HDWallet::CreateTransaction(const std::vector<TxOutput>& outputs, uint64_t fee_rate, std::string& tx_hex_out, std::string& error_out) {
  if (!utxo_index_) {
    error_out = "No UTXO index connected";
    return false;
  }

  // Reset auto-lock timer (wallet operations require unlocked wallet)
  ResetAutoLockTimer();
  
  // Calculate total output amount
  uint64_t total_output = 0;
  for (const auto& out : outputs) {
    total_output += out.value;
  }
  
  // Get available UTXOs
  auto available_utxos = ListUTXOs(1, true);
  if (available_utxos.empty()) {
    error_out = "No spendable UTXOs available";
    return false;
  }
  
  // Use coin selection to pick inputs
  auto coin_result = dinero::CoinSelector::SelectCoins(
    available_utxos,
    total_output,
    fee_rate,
    outputs.size()
  );
  
  if (!coin_result.success) {
    error_out = coin_result.error;
    return false;
  }
  
  std::cout << "INFO: Selected " << coin_result.selected_coins.size() 
            << " inputs, total value: " << coin_result.total_value 
            << ", fee: " << coin_result.fee 
            << ", change: " << coin_result.change_amount << std::endl;
  
  // Build transaction
  dinero::Transaction tx;
  tx.version = 2;
  tx.witness_version = 1;  // Taproot - Dinero is Taproot from genesis
  tx.lockTime = 0;
  
  // Add inputs
  for (const auto& utxo : coin_result.selected_coins) {
    dinero::TxInput input;
    input.prevout.txid = dinero::TxId(utxo.txid);  // Phase M.4: Wrap uint256 in TxId with namespace
    input.prevout.vout = utxo.vout;
    input.scriptSig.clear();  // Empty for SegWit
    input.sequence = 0xfffffffe;  // RBF-enabled
    tx.vin.push_back(input);
  }
  
  // Add recipient outputs
  for (const auto& out : outputs) {
    auto script = AddressToScriptPubKey(out.address);
    // Phase M.6.2: Wrap raw value in AmountUna for TxOutput constructor
    dinero::TxOutput output(dinero::AmountUna::Una(out.value), script);
    tx.vout.push_back(output);
  }

  // Add change output if needed (using BIP84 change chain)
  if (coin_result.change_amount > dinero::CoinSelector::DUST_THRESHOLD) {
    std::string change_addr = DeriveNextChangeAddress();  // BIP84 m/84'/1448'/0'/1/index
    auto change_script = AddressToScriptPubKey(change_addr);
    // Phase M.6.2: Wrap raw value in AmountUna for TxOutput constructor
    dinero::TxOutput change_output(dinero::AmountUna::Una(coin_result.change_amount), change_script);
    tx.vout.push_back(change_output);
    std::cout << "INFO: Added change output: " << coin_result.change_amount
              << " to " << change_addr << " (change chain)" << std::endl;
  }
  
  // Get private keys for signing
  std::vector<std::vector<uint8_t>> private_keys;
  for (const auto& utxo : coin_result.selected_coins) {
    // Extract index from BIP32 path (e.g., "m/84'/1448'/0'/0/12" → 12)
    // CanonicalWalletUTXO stores path, not address (Phase M.3)
    uint32_t addr_index = 0;
    size_t last_slash = utxo.path.rfind('/');
    if (last_slash != std::string::npos) {
      try {
        addr_index = std::stoul(utxo.path.substr(last_slash + 1));
      } catch (...) {
        error_out = "Failed to parse address index from path: " + utxo.path;
        return false;
      }
    } else {
      error_out = "Invalid derivation path in UTXO: " + utxo.path;
      return false;
    }

    auto privkey = GetPrivateKeyAt(addr_index);
    if (privkey.empty()) {
      error_out = "Failed to derive private key for signing";
      return false;
    }
    private_keys.push_back(privkey);
  }
  
  // Sign the transaction with BIP143
  if (!dinero::BIP143Signer::SignTransaction(tx, coin_result.selected_coins, private_keys)) {
    error_out = "Failed to sign transaction";
    return false;
  }
  
  // Serialize to hex
  tx_hex_out = tx.SerializeHex(true);  // Include witness data
  
  std::cout << "✅ Transaction created and signed successfully!" << std::endl;
  // Phase M.4.3-B Step 1: Unwrap TxId for hex conversion
  std::cout << "   Txid: " << tx.GetTxid().AsUint256().GetHex() << std::endl;
  std::cout << "   Size: " << tx.GetSize() << " bytes" << std::endl;
  std::cout << "   Virtual size: " << tx.GetVirtualSize() << " vbytes" << std::endl;

  return true;
}
// End of CreateTransaction - used by CreatePSBT

std::vector<uint8_t> HDWallet::GetPrivateKeyAt(uint32_t index) const {
  // BIP84: m/84'/coin_type'/0'/0/index (P2WPKH receive private key)
  if (index >= 0x80000000) {
    throw std::runtime_error("Invalid non-hardened index: index must be < 2^31");
  }

  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(84);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(0);      // Chain 0 (receive)
  deriver.deriveNormal(index);

  auto k = deriver.getPrivateKey();
  return std::vector<uint8_t>(k.begin(), k.end());
}

std::vector<uint8_t> HDWallet::GetChangePrivateKeyAt(uint32_t index) const {
  // BIP84: m/84'/coin_type'/0'/1/index (P2WPKH change private key)
  if (index >= 0x80000000) {
    throw std::runtime_error("Invalid non-hardened index: index must be < 2^31");
  }

  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(84);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(1);      // Chain 1 (change)
  deriver.deriveNormal(index);

  auto k = deriver.getPrivateKey();
  return std::vector<uint8_t>(k.begin(), k.end());
}

std::vector<uint8_t> HDWallet::GetMiningPrivateKeyAt(uint32_t index) const {
  // BIP84: m/84'/coin_type'/0'/2/index (P2WPKH mining private key)
  if (index >= 0x80000000) {
    throw std::runtime_error("Invalid non-hardened index: index must be < 2^31");
  }

  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(84);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(2);      // Chain 2 (mining)
  deriver.deriveNormal(index);

  auto k = deriver.getPrivateKey();
  return std::vector<uint8_t>(k.begin(), k.end());
}

std::string HDWallet::GetMnemonic() const {
  if (mnemonic_.empty()) {
    throw std::runtime_error("No mnemonic available - wallet was created without BIP39 mnemonic");
  }
  return mnemonic_;
}

std::vector<uint8_t> HDWallet::AddressToScriptPubKey(const std::string& address) const {
  // PROPER IMPLEMENTATION: Decode bech32 address to get witness program
  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";  // fallback
  }

  // Decode the bech32 address
  auto decode_result = bech32::Decode(hrp, address);
  if (!decode_result.has_value()) {
    std::cerr << "❌ Failed to decode bech32 address: " << address << std::endl;
    return {};
  }

  // Build scriptPubKey: OP_<version> <program>
  std::vector<uint8_t> script;
  script.push_back(decode_result->witver);  // Witness version (0 for P2WPKH/P2WSH, 1 for Taproot)
  script.push_back(static_cast<uint8_t>(decode_result->program.size()));  // Program length
  script.insert(script.end(), decode_result->program.begin(), decode_result->program.end());

  return script;
}

// ========================================================================
// BIP86 TAPROOT ADDRESS GENERATION (m/86'/1448'/0')
// ========================================================================

std::string HDWallet::DeriveNextTaprootAddress() {
  // Bounds check: only non-hardened indices allowed for addresses
  if (taproot_index_ >= 0x80000000) {
    throw std::runtime_error("Taproot address index overflow: maximum 2^31-1 addresses");
  }

  // Reset auto-lock timer (wallet operations require unlocked wallet)
  ResetAutoLockTimer();

  std::string addr = GetTaprootAddressAt(taproot_index_);

  // CRITICAL FIX: Register scriptPubKey with UTXOIndex immediately after generation
  // This ensures the wallet can detect UTXOs sent to this Taproot address
  if (utxo_index_) {
    auto script = AddressToScriptPubKey(addr);
    std::string derivation_path = MakeDerivationPath(86, 0, 0, taproot_index_);
    utxo_index_->RegisterAddress(script, derivation_path);
    std::cout << "✅ Registered Taproot address with UTXOIndex: " << addr << std::endl;
  }

  taproot_index_++;
  Save();
  return addr;
}

std::string HDWallet::DeriveNextTaprootChangeAddress() {
  // Bounds check: only non-hardened indices allowed for change addresses
  if (taproot_change_index_ >= 0x80000000) {
    throw std::runtime_error("Taproot change address index overflow: maximum 2^31-1 addresses");
  }

  std::string addr = GetTaprootChangeAddressAt(taproot_change_index_);
  taproot_change_index_++;
  Save();
  return addr;
}

std::string HDWallet::GetTaprootAddressAt(uint32_t index) const {
  // BIP86: m/86'/coin_type'/0'/0/index (key-spend only Taproot)

  // BIP32 derivation using consolidated deriver (replaces inline lambdas)
  // Memory zeroization handled by BIP32Deriver RAII destructor
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(0);      // Chain 0 (receive)
  deriver.deriveNormal(index);

  // Get x-only pubkey and compute BIP341 tweaked output key via canonical function
  auto xonly_bytes = deriver.getXOnlyPubkey();
  std::array<uint8_t, 32> output_key_arr{};
  if (!dinero::TaprootKeys::ComputeTweakedPubkey(xonly_bytes, output_key_arr)) {
    throw std::runtime_error("Failed to compute Taproot output key");
  }

  // Encode as Bech32m (witness version 1)
  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";
  }

  std::vector<uint8_t> program(output_key_arr.begin(), output_key_arr.end());
  std::string address = bech32::Encode(hrp, 1, program, bech32::Encoding::BECH32M);

  return address;
}

std::string HDWallet::GetTaprootChangeAddressAt(uint32_t index) const {
  // BIP86: m/86'/coin_type'/0'/1/index (change chain)

  // BIP32 derivation using consolidated deriver (replaces inline lambdas)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(1);      // Chain 1 (change)
  deriver.deriveNormal(index);

  // Get x-only pubkey and compute BIP341 tweaked output key via canonical function
  auto xonly_bytes = deriver.getXOnlyPubkey();
  std::array<uint8_t, 32> output_key_arr{};
  if (!dinero::TaprootKeys::ComputeTweakedPubkey(xonly_bytes, output_key_arr)) {
    throw std::runtime_error("Failed to compute Taproot output key");
  }

  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";
  }

  std::vector<uint8_t> program(output_key_arr.begin(), output_key_arr.end());
  std::string address = bech32::Encode(hrp, 1, program, bech32::Encoding::BECH32M);

  return address;
}

std::vector<uint8_t> HDWallet::GetTaprootPrivateKeyAt(uint32_t index) const {
  // Derive private key using BIP86 path: m/86'/coin_type'/0'/0/index

  if (index >= 0x80000000) {
    throw std::runtime_error("Invalid non-hardened index: index must be < 2^31");
  }

  // BIP32 derivation using consolidated deriver (replaces inline lambdas)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(0);      // Chain 0 (receive)
  deriver.deriveNormal(index);

  auto k = deriver.getPrivateKey();
  auto xonly_bytes = deriver.getXOnlyPubkey();

  // BIP341 TapTweak via canonical implementation
  std::array<uint8_t, 32> priv_arr, xonly_arr, tweaked_arr;
  std::copy(k.begin(), k.end(), priv_arr.begin());
  std::copy(xonly_bytes.begin(), xonly_bytes.end(), xonly_arr.begin());
  if (!dinero::TaprootKeys::ComputeTweakedPrivkey(priv_arr, xonly_arr, tweaked_arr)) {
    throw std::runtime_error("Failed to compute tweaked Taproot private key");
  }
  return std::vector<uint8_t>(tweaked_arr.begin(), tweaked_arr.end());
}

// HDWallet encryption methods implementation
// This will be merged into hd_wallet.cpp

// ========================================================================
// ENCRYPTION METHODS (PBKDF2-HMAC-SHA512 + AES-256-GCM)
// ========================================================================

// Helper: Derive key from password using PBKDF2-HMAC-SHA512 (Bitcoin Core style)
static bool DeriveKey(const std::string& password,
                      const std::vector<uint8_t>& salt,
                      uint8_t key_out[32]) {
  if (salt.size() != SALT_SIZE) return false;
  
  // Use OpenSSL's PBKDF2 - works everywhere, no external dependencies
  int result = PKCS5_PBKDF2_HMAC(
    password.c_str(),           // password
    password.size(),            // password length
    salt.data(),                // salt
    salt.size(),                // salt length
    PBKDF2_ITERATIONS,          // iterations (600,000)
    EVP_sha512(),               // hash function (SHA-512)
    KEY_SIZE,                   // output key length (32 bytes)
    key_out                     // output buffer
  );
  
  return result == 1;
}

static bool MakePasswordVerifier(const uint8_t key[32],
                                 std::vector<uint8_t>& verifier_out) {
  unsigned int digest_len = 0;
  uint8_t digest[EVP_MAX_MD_SIZE];
  const unsigned char* domain =
      reinterpret_cast<const unsigned char*>(PASSWORD_VERIFIER_DOMAIN);

  if (HMAC(EVP_sha256(), key, KEY_SIZE, domain,
           std::strlen(PASSWORD_VERIFIER_DOMAIN), digest, &digest_len) == nullptr ||
      digest_len != KEY_SIZE) {
    OPENSSL_cleanse(digest, sizeof(digest));
    return false;
  }

  verifier_out.assign(digest, digest + KEY_SIZE);
  OPENSSL_cleanse(digest, sizeof(digest));
  return true;
}

static bool VerifyPasswordMaterial(const std::vector<uint8_t>& stored_verifier,
                                   const uint8_t key[32],
                                   bool* legacy_key_format) {
  if (legacy_key_format) {
    *legacy_key_format = false;
  }
  if (stored_verifier.size() != KEY_SIZE) {
    return false;
  }

  std::vector<uint8_t> expected;
  if (!MakePasswordVerifier(key, expected)) {
    return false;
  }
  const bool verifier_match =
      CRYPTO_memcmp(expected.data(), stored_verifier.data(), KEY_SIZE) == 0;
  OPENSSL_cleanse(expected.data(), expected.size());
  if (verifier_match) {
    return true;
  }

  // Legacy wallet_state persisted the raw derived AES key in password_hash_.
  const bool raw_key_match = CRYPTO_memcmp(key, stored_verifier.data(), KEY_SIZE) == 0;
  if (raw_key_match && legacy_key_format) {
    *legacy_key_format = true;
  }
  return raw_key_match;
}

// Helper: AES-256-GCM encrypt
static bool AES_Encrypt(const uint8_t* plaintext, size_t plaintext_len,
                        const uint8_t key[32], const uint8_t nonce[12],
                        uint8_t* ciphertext_out, uint8_t tag_out[16]) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  
  int len = 0;
  int ciphertext_len = 0;
  
  // Initialize encryption
  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, nonce) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  
  // Encrypt
  if (EVP_EncryptUpdate(ctx, ciphertext_out, &len, plaintext, (int)plaintext_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  ciphertext_len = len;
  
  // Finalize
  if (EVP_EncryptFinal_ex(ctx, ciphertext_out + len, &len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  ciphertext_len += len;
  
  // Get tag
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag_out) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  
  EVP_CIPHER_CTX_free(ctx);
  return true;
}

static bool SealSeedForStorage(const std::vector<uint8_t>& seed,
                               const std::string& password,
                               std::vector<uint8_t>& salt_out,
                               std::vector<uint8_t>& password_hash_out,
                               std::vector<uint8_t>& encrypted_seed_out) {
  if (seed.empty()) return false;

  salt_out.resize(SALT_SIZE);
  if (RAND_bytes(salt_out.data(), SALT_SIZE) != 1) {
    return false;
  }

  uint8_t key[KEY_SIZE];
  if (!DeriveKey(password, salt_out, key)) {
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }

  if (!MakePasswordVerifier(key, password_hash_out)) {
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }

  std::vector<uint8_t> nonce(NONCE_SIZE);
  if (RAND_bytes(nonce.data(), NONCE_SIZE) != 1) {
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }

  encrypted_seed_out.resize(NONCE_SIZE + seed.size() + TAG_SIZE);
  std::memcpy(encrypted_seed_out.data(), nonce.data(), NONCE_SIZE);
  uint8_t* ciphertext = encrypted_seed_out.data() + NONCE_SIZE;
  uint8_t* tag = encrypted_seed_out.data() + NONCE_SIZE + seed.size();

  const bool ok = AES_Encrypt(seed.data(), seed.size(), key, nonce.data(), ciphertext, tag);
  OPENSSL_cleanse(key, KEY_SIZE);
  if (!ok) {
    encrypted_seed_out.clear();
    return false;
  }

  return true;
}

static bool UnsealSeedFromStorage(const std::vector<uint8_t>& salt,
                                  const std::vector<uint8_t>& password_hash,
                                  const std::vector<uint8_t>& encrypted_seed,
                                  const std::string& password,
                                  std::vector<uint8_t>& seed_out) {
  if (salt.size() != SALT_SIZE || password_hash.size() != KEY_SIZE) return false;
  if (encrypted_seed.size() < (NONCE_SIZE + TAG_SIZE)) return false;

  uint8_t key[KEY_SIZE];
  if (!DeriveKey(password, salt, key)) {
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }

  if (!VerifyPasswordMaterial(password_hash, key)) {
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }

  const uint8_t* nonce = encrypted_seed.data();
  const uint8_t* ciphertext = encrypted_seed.data() + NONCE_SIZE;
  const size_t ciphertext_len = encrypted_seed.size() - NONCE_SIZE - TAG_SIZE;
  const uint8_t* tag = encrypted_seed.data() + NONCE_SIZE + ciphertext_len;

  seed_out.assign(ciphertext_len, 0);
  const bool ok = AES_Decrypt(ciphertext, ciphertext_len, key, nonce, tag, seed_out.data());
  OPENSSL_cleanse(key, KEY_SIZE);
  if (!ok) {
    seed_out.clear();
    return false;
  }
  return true;
}

// Helper: AES-256-GCM decrypt
static bool AES_Decrypt(const uint8_t* ciphertext, size_t ciphertext_len,
                        const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t tag[16], uint8_t* plaintext_out) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  
  int len = 0;
  int plaintext_len = 0;
  
  // Initialize decryption
  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, nonce) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  
  // Decrypt
  if (EVP_DecryptUpdate(ctx, plaintext_out, &len, ciphertext, (int)ciphertext_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  plaintext_len = len;
  
  // Set expected tag
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, const_cast<uint8_t*>(tag)) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  
  // Finalize and verify tag
  if (EVP_DecryptFinal_ex(ctx, plaintext_out + len, &len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return false;  // Tag verification failed!
  }
  plaintext_len += len;
  
  EVP_CIPHER_CTX_free(ctx);
  return true;
}

bool HDWallet::EncryptWallet(const std::string& password) {
  if (encrypted_) {
    std::cerr << "Wallet is already encrypted" << std::endl;
    return false;
  }
  
  if (password.length() < 8) {
    std::cerr << "Password must be at least 8 characters" << std::endl;
    return false;
  }
  
  // Generate random salt for password
  password_salt_.resize(SALT_SIZE);
  if (RAND_bytes(password_salt_.data(), SALT_SIZE) != 1) {
    std::cerr << "Failed to generate salt" << std::endl;
    return false;
  }
  
  // Derive key from password using PBKDF2-HMAC-SHA512
  uint8_t key[KEY_SIZE];
  if (!DeriveKey(password, password_salt_, key)) {
    std::cerr << "Failed to derive key" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Store a verifier, never the raw AES key.
  if (!MakePasswordVerifier(key, password_hash_)) {
    std::cerr << "Failed to create password verifier" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Generate random nonce
  std::vector<uint8_t> nonce(NONCE_SIZE);
  if (RAND_bytes(nonce.data(), NONCE_SIZE) != 1) {
    std::cerr << "Failed to generate nonce" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Encrypt seed: nonce(12) + ciphertext(64) + tag(16)
  encrypted_seed_.resize(NONCE_SIZE + seed_.size() + TAG_SIZE);
  std::memcpy(encrypted_seed_.data(), nonce.data(), NONCE_SIZE);
  
  uint8_t* ciphertext = encrypted_seed_.data() + NONCE_SIZE;
  uint8_t* tag = encrypted_seed_.data() + NONCE_SIZE + seed_.size();
  
  if (!AES_Encrypt(seed_.data(), seed_.size(), key, nonce.data(), ciphertext, tag)) {
    std::cerr << "Failed to encrypt seed" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    encrypted_seed_.clear();
    password_salt_.clear();
    password_hash_.clear();
    return false;
  }
  OPENSSL_cleanse(key, KEY_SIZE);
  
  // Clear plaintext seed from memory
  std::fill(seed_.begin(), seed_.end(), 0);
  seed_.clear();

  // CRITICAL: Also clear plaintext mnemonic from memory
  std::fill(mnemonic_.begin(), mnemonic_.end(), '\0');
  mnemonic_.clear();

  encrypted_ = true;
  locked_ = true;

  // Save encrypted wallet (mnemonic will NOT be saved due to fix in Save())
  Save();

  std::cout << "✅ Wallet encrypted successfully" << std::endl;
  std::cout << "⚠️  Wallet is now locked. Use Unlock() to access." << std::endl;
  std::cout << "🔒 Plaintext mnemonic cleared from memory for security" << std::endl;

  return true;
}

bool HDWallet::Lock() {
  if (!encrypted_) {
    std::cerr << "Wallet is not encrypted" << std::endl;
    return false;
  }
  
  if (locked_) {
    return true;  // Already locked
  }
  
  // Clear plaintext seed from memory
  std::fill(seed_.begin(), seed_.end(), 0);
  seed_.clear();
  
  locked_ = true;
  
  // Reset unlock time when locked
  unlock_time_ = 0;
  
  std::cout << "🔒 Wallet locked" << std::endl;
  
  return true;
}

bool HDWallet::Unlock(const std::string& password) {
  if (!encrypted_) {
    std::cerr << "Wallet is not encrypted" << std::endl;
    return false;
  }
  
  // ALWAYS verify password, even if already unlocked
  // (Don't skip password check just because wallet is unlocked!)
  
  // Derive key from password
  uint8_t key[KEY_SIZE];
  if (!DeriveKey(password, password_salt_, key)) {
    std::cerr << "Failed to derive key" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Verify password without storing the raw AES key in wallet_state.
  bool legacy_key_format = false;
  if (!VerifyPasswordMaterial(password_hash_, key, &legacy_key_format)) {
    std::cerr << "❌ Incorrect password" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Password is correct - check if already unlocked
  if (!locked_) {
    if (legacy_key_format && MakePasswordVerifier(key, password_hash_)) {
      Save();
    }
    OPENSSL_cleanse(key, KEY_SIZE);
    std::cout << "✅ Password verified (wallet already unlocked)" << std::endl;
    return true;  // Already unlocked, but password was verified
  }
  
  // Extract nonce, ciphertext, tag
  const uint8_t* nonce = encrypted_seed_.data();
  const uint8_t* ciphertext = encrypted_seed_.data() + NONCE_SIZE;
  size_t ciphertext_len = encrypted_seed_.size() - NONCE_SIZE - TAG_SIZE;
  const uint8_t* tag = encrypted_seed_.data() + NONCE_SIZE + ciphertext_len;
  
  // Decrypt seed
  seed_.resize(ciphertext_len);
  if (!AES_Decrypt(ciphertext, ciphertext_len, key, nonce, tag, seed_.data())) {
    std::cerr << "❌ Failed to decrypt seed (corrupted wallet?)" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    seed_.clear();
    return false;
  }
  
  locked_ = false;
  if (legacy_key_format && MakePasswordVerifier(key, password_hash_)) {
    Save();
  }
  OPENSSL_cleanse(key, KEY_SIZE);
  
  // Reset auto-lock timer
  ResetAutoLockTimer();
  
  std::cout << "🔓 Wallet unlocked" << std::endl;
  
  return true;
}

bool HDWallet::ChangePassword(const std::string& old_password, const std::string& new_password) {
  if (!encrypted_) {
    std::cerr << "Wallet is not encrypted" << std::endl;
    return false;
  }
  
  if (new_password.length() < 8) {
    std::cerr << "New password must be at least 8 characters" << std::endl;
    return false;
  }
  
  // Unlock with old password
  bool was_locked = locked_;
  if (!Unlock(old_password)) {
    return false;
  }
  
  // Generate new salt
  password_salt_.resize(SALT_SIZE);
  if (RAND_bytes(password_salt_.data(), SALT_SIZE) != 1) {
    std::cerr << "Failed to generate new salt" << std::endl;
    return false;
  }
  
  // Derive new key
  uint8_t key[KEY_SIZE];
  if (!DeriveKey(new_password, password_salt_, key)) {
    std::cerr << "Failed to derive new key" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Store new password verifier
  if (!MakePasswordVerifier(key, password_hash_)) {
    std::cerr << "Failed to create password verifier" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Generate new nonce
  std::vector<uint8_t> nonce(NONCE_SIZE);
  if (RAND_bytes(nonce.data(), NONCE_SIZE) != 1) {
    std::cerr << "Failed to generate nonce" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  
  // Re-encrypt seed with new password
  encrypted_seed_.resize(NONCE_SIZE + seed_.size() + TAG_SIZE);
  std::memcpy(encrypted_seed_.data(), nonce.data(), NONCE_SIZE);
  
  uint8_t* ciphertext = encrypted_seed_.data() + NONCE_SIZE;
  uint8_t* tag = encrypted_seed_.data() + NONCE_SIZE + seed_.size();
  
  if (!AES_Encrypt(seed_.data(), seed_.size(), key, nonce.data(), ciphertext, tag)) {
    std::cerr << "Failed to encrypt seed with new password" << std::endl;
    OPENSSL_cleanse(key, KEY_SIZE);
    return false;
  }
  OPENSSL_cleanse(key, KEY_SIZE);
  
  // Save wallet with new encryption
  Save();
  
  // Re-lock if it was locked before
  if (was_locked) {
    Lock();
  }
  
  std::cout << "✅ Password changed successfully" << std::endl;
  
  return true;
}

// Auto-lock timeout implementation
void HDWallet::AutoLockThread() {
  while (autolock_running_) {
    std::this_thread::sleep_for(std::chrono::seconds(10));  // Check every 10 seconds
    
    // Only auto-lock if wallet is encrypted and unlocked
    if (encrypted_ && !locked_) {
      time_t now = std::time(nullptr);
      time_t unlock_time = unlock_time_.load();
      
      // Check if timeout has elapsed
      if (unlock_time > 0 && (now - unlock_time) >= autolock_seconds_) {
        std::lock_guard<std::mutex> lock(autolock_mutex_);
        
        // Double-check wallet is still unlocked (may have been locked manually)
        if (!locked_) {
          std::cout << "🔒 Auto-locking wallet after " << autolock_seconds_ 
                    << " seconds of inactivity" << std::endl;
          Lock();
        }
      }
    }
  }
}

void HDWallet::ResetAutoLockTimer() {
  if (encrypted_ && !locked_) {
    unlock_time_ = std::time(nullptr);
  }
}

void HDWallet::SetAutoLockTimeout(int seconds) {
  if (seconds < 60) {
    std::cerr << "⚠️  Auto-lock timeout must be at least 60 seconds" << std::endl;
    autolock_seconds_ = 60;
  } else if (seconds > 86400) {
    std::cerr << "⚠️  Auto-lock timeout capped at 86400 seconds (24 hours)" << std::endl;
    autolock_seconds_ = 86400;
  } else {
    autolock_seconds_ = seconds;
  }

  // Reset timer when timeout is changed
  ResetAutoLockTimer();
}

// ═══════════════════════════════════════════════════════════════════════════
// BIP32 MASTER KEY FINGERPRINT CALCULATION
// ═══════════════════════════════════════════════════════════════════════════

uint32_t HDWallet::CalculateMasterFingerprint() const {
  // BIP32 master fingerprint = first 4 bytes of HASH160(master_pubkey)

  // Derive BIP32 root from seed
  uint8_t I[64];
  const uint8_t* seed = seed_.data();
  HMAC512((const uint8_t*)"Bitcoin seed", 12, seed, 64, I);

  // Master private key is first 32 bytes
  uint8_t master_privkey[32];
  memcpy(master_privkey, I, 32);

  // Derive master public key
  secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
  secp256k1_pubkey master_pubkey_obj;
  if (!secp256k1_ec_pubkey_create(ctx, &master_pubkey_obj, master_privkey)) {
    secp256k1_context_destroy(ctx);
    throw std::runtime_error("Failed to derive master public key");
  }

  // Serialize master pubkey (33-byte compressed)
  uint8_t master_pubkey[33];
  size_t pubkey_len = 33;
  secp256k1_ec_pubkey_serialize(ctx, master_pubkey, &pubkey_len, &master_pubkey_obj, SECP256K1_EC_COMPRESSED);
  secp256k1_context_destroy(ctx);

  // Calculate HASH160 of master pubkey
  uint8_t hash160[20];
  HASH160(master_pubkey, 33, hash160);

  // Fingerprint is first 4 bytes, big-endian
  uint32_t fingerprint = ((uint32_t)hash160[0] << 24) |
                         ((uint32_t)hash160[1] << 16) |
                         ((uint32_t)hash160[2] << 8) |
                         ((uint32_t)hash160[3]);

  // Clear sensitive data
  OPENSSL_cleanse(master_privkey, 32);
  OPENSSL_cleanse(I, 64);

  return fingerprint;
}

std::string HDWallet::GetMasterFingerprintHex() const {
  uint32_t fingerprint = CalculateMasterFingerprint();

  // Convert to 8-character lowercase hex string
  char hex[9];
  snprintf(hex, sizeof(hex), "%08x", fingerprint);

  return std::string(hex);
}

std::string HDWallet::GetAccountXpub(uint32_t account) const {
  // Use HDKeychain to derive account extended public key
  auto master_key = dinero::crypto::HDKeychain::fromSeed(seed_);
  auto account_key = dinero::crypto::HDKeychain::getBIP84Account(master_key, coin_type_, account);

  // Serialize to xpub format (mainnet=true for now, can be parameterized later)
  return account_key.serialize(true);
}

// ═══════════════════════════════════════════════════════════════════════════
// PSBT CREATION WITH PROPER BIP32 METADATA (Hardware Wallet Support)
// ═══════════════════════════════════════════════════════════════════════════

#include "wallet/psbt.h"

bool HDWallet::CreatePSBT(const std::vector<TxOutput>& outputs, uint64_t fee_rate,
                          dinero::PSBT& psbt_out, std::string& error_out) {
    if (!utxo_index_) {
        error_out = "No UTXO index connected";
        return false;
    }

    // Reset auto-lock timer
    ResetAutoLockTimer();

    // Calculate total output amount
    uint64_t total_output = 0;
    for (const auto& out : outputs) {
        total_output += out.value;
    }

    // Get available UTXOs
    auto available_utxos = ListUTXOs(1, true);
    if (available_utxos.empty()) {
        error_out = "No spendable UTXOs available";
        return false;
    }

    // Use coin selection
    auto coin_result = dinero::CoinSelector::SelectCoins(
        available_utxos,
        total_output,
        fee_rate,
        outputs.size()
    );

    if (!coin_result.success) {
        error_out = coin_result.error;
        return false;
    }

    // Build unsigned transaction
    dinero::Transaction tx;
    tx.version = 2;
    tx.witness_version = 1;  // Support both SegWit v0 and Taproot v1
    tx.lockTime = 0;

    // Add inputs (unsigned)
    for (const auto& utxo : coin_result.selected_coins) {
        dinero::TxInput input;
        input.prevout.txid = dinero::TxId(utxo.txid);  // Phase M.4: Wrap uint256 in TxId with namespace
        input.prevout.vout = utxo.vout;
        input.scriptSig.clear();  // Empty for PSBT
        input.sequence = 0xfffffffe;  // RBF-enabled
        tx.vin.push_back(input);
    }

    // Add recipient outputs
    for (const auto& out : outputs) {
        auto script = AddressToScriptPubKey(out.address);
        // Phase M.6.2: Wrap raw value in AmountUna for TxOutput constructor
        dinero::TxOutput output(dinero::AmountUna::Una(out.value), script);
        tx.vout.push_back(output);
    }

    // Add change output if needed (using BIP86 Taproot change chain)
    if (coin_result.change_amount > dinero::CoinSelector::DUST_THRESHOLD) {
        std::string change_addr = DeriveNextTaprootChangeAddress();  // BIP86 m/86'/1448'/0'/1/index (Taproot everywhere!)
        auto change_script = AddressToScriptPubKey(change_addr);
        // Phase M.6.2: Wrap raw value in AmountUna for TxOutput constructor
        dinero::TxOutput change_output(dinero::AmountUna::Una(coin_result.change_amount), change_script);
        tx.vout.push_back(change_output);
    }

    // Initialize PSBT
    psbt_out.tx = tx;
    psbt_out.inputs.resize(tx.vin.size());
    psbt_out.outputs.resize(tx.vout.size());

    // Add witness UTXO data and BIP32 derivation for each input
    for (size_t i = 0; i < coin_result.selected_coins.size(); i++) {
        const auto& utxo = coin_result.selected_coins[i];
        auto& psbt_input = psbt_out.inputs[i];

        // Add witness UTXO (value + scriptPubKey)
        // Phase M.6.2: Extract raw value for PSBT (witness_utxo_amount is uint64_t)
        psbt_input.witness_utxo_amount = utxo.value.GetUna();
        psbt_input.witness_utxo_script = utxo.spk;  // Phase M.3: spk not scriptPubKey

        // Add BIP32 derivation info from path (Phase M.3: path is authoritative)
        // TODO: Implement derivation from path when PSBT is re-enabled
        // For now, path is stored in utxo.path but derivation helpers need updating
    }

    // Add BIP32 derivation for change outputs (if any)
    for (size_t i = 0; i < psbt_out.outputs.size(); i++) {
        auto& output = psbt_out.outputs[i];
        // Check if this is our last output (change output)
        if (i == psbt_out.outputs.size() - 1 && coin_result.change_amount > 0) {
            // This is the change output - add derivation info
            uint32_t change_idx = change_index_ - 1;  // We just derived it
            std::string change_addr = GetChangeAddressAt(change_idx);
            DerivationInfo info;
            if (GetDerivationInfo(change_addr, info)) {
                output.bip32_derivation[info.pubkey] = {info.master_fingerprint, info.path};
            }
        }
    }

    std::cout << "✅ PSBT created successfully!" << std::endl;
    std::cout << "   Inputs: " << psbt_out.inputs.size() << std::endl;
    std::cout << "   Outputs: " << psbt_out.outputs.size() << std::endl;
    std::cout << "   Ready for hardware wallet signing" << std::endl;

    return true;
}

void HDWallet::FillPSBT(dinero::PSBT& psbt) {
    // For each input, add BIP32 derivation info
    for (size_t i = 0; i < psbt.inputs.size(); i++) {
        auto& input = psbt.inputs[i];
        auto& txin = psbt.tx.vin[i];

        // Get the scriptPubKey from UTXO (needed to find which address spent)
        // For now, we'll iterate through our derived addresses to find matches
        // TODO: Optimize with address->index cache

        // Try receive addresses (m/84'/1448'/0'/0/index)
        for (uint32_t idx = 0; idx < index_; idx++) {
            std::string addr = GetAddressAt(idx);
            DerivationInfo info;
            if (GetDerivationInfo(addr, info)) {
                // Add BIP32 derivation: pubkey -> (fingerprint, path)
                input.bip32_derivation[info.pubkey] = {info.master_fingerprint, info.path};
            }
        }

        // Try change addresses (m/84'/1448'/0'/1/index)
        for (uint32_t idx = 0; idx < change_index_; idx++) {
            std::string addr = GetChangeAddressAt(idx);
            DerivationInfo info;
            if (GetDerivationInfo(addr, info)) {
                input.bip32_derivation[info.pubkey] = {info.master_fingerprint, info.path};
            }
        }
    }

    // For each output, add BIP32 derivation for change outputs
    for (size_t i = 0; i < psbt.outputs.size(); i++) {
        auto& output = psbt.outputs[i];
        auto& txout = psbt.tx.vout[i];

        // Check if this is a change output (belongs to our wallet)
        // Change outputs are from m/84'/1448'/0'/1/index
        for (uint32_t idx = 0; idx < change_index_; idx++) {
            std::string addr = GetChangeAddressAt(idx);
            DerivationInfo info;
            if (GetDerivationInfo(addr, info)) {
                output.bip32_derivation[info.pubkey] = {info.master_fingerprint, info.path};
            }
        }
    }
}

bool HDWallet::GetDerivationInfo(const std::string& address, DerivationInfo& info_out) const {
    // Use address cache for O(1) lookup instead of O(n) iteration
    const uint32_t HARDENED = 0x80000000;
    uint32_t idx = 0;
    std::vector<uint8_t> privkey;
    uint32_t chain = 0;  // 0 = receive, 1 = change, 2 = mining
    uint32_t purpose = 84;  // BIP84 (SegWit) or BIP86 (Taproot)
    bool is_taproot = false;

    // Check SegWit receive addresses (BIP84 m/84'/1448'/0'/0/index)
    auto it = address_to_index_.find(address);
    if (it != address_to_index_.end()) {
        idx = it->second;
        chain = 0;
        purpose = 84;
        privkey = GetPrivateKeyAt(idx);
    }
    // Check SegWit change addresses (BIP84 m/84'/1448'/0'/1/index)
    else if (auto it_change = change_address_to_index_.find(address); it_change != change_address_to_index_.end()) {
        idx = it_change->second;
        chain = 1;
        purpose = 84;
        privkey = GetChangePrivateKeyAt(idx);
    }
    // Check Taproot receive addresses (BIP86 m/86'/1448'/0'/0/index)
    else if (auto it_taproot = taproot_address_to_index_.find(address); it_taproot != taproot_address_to_index_.end()) {
        idx = it_taproot->second;
        chain = 0;
        purpose = 86;
        is_taproot = true;
        privkey = GetTaprootPrivateKeyAt(idx);
    }
    // Check Taproot change addresses (BIP86 m/86'/1448'/0'/1/index)
    else if (auto it_taproot_change = taproot_change_to_index_.find(address); it_taproot_change != taproot_change_to_index_.end()) {
        idx = it_taproot_change->second;
        chain = 1;
        purpose = 86;
        is_taproot = true;
        privkey = GetTaprootChangePrivateKeyAt(idx);
    }
    // Check Taproot mining addresses (BIP86 m/86'/1448'/0'/2/index)
    else if (auto it_taproot_mining = taproot_mining_to_index_.find(address); it_taproot_mining != taproot_mining_to_index_.end()) {
        idx = it_taproot_mining->second;
        chain = 2;
        purpose = 86;
        is_taproot = true;
        privkey = GetTaprootMiningPrivateKeyAt(idx);
    }
    else {
        return false;  // Address not found in wallet
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    if (is_taproot) {
        // For Taproot: use x-only pubkey (32 bytes)
        secp256k1_pubkey pubkey_obj;
        if (!secp256k1_ec_pubkey_create(ctx, &pubkey_obj, privkey.data())) {
            secp256k1_context_destroy(ctx);
            return false;
        }

        secp256k1_xonly_pubkey xonly_pubkey;
        int parity;
        if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_pubkey, &parity, &pubkey_obj)) {
            secp256k1_context_destroy(ctx);
            return false;
        }

        std::vector<uint8_t> pubkey(32);
        if (!secp256k1_xonly_pubkey_serialize(ctx, pubkey.data(), &xonly_pubkey)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        info_out.pubkey = pubkey;
    } else {
        // For SegWit: use compressed pubkey (33 bytes)
        secp256k1_pubkey pubkey_obj;
        if (!secp256k1_ec_pubkey_create(ctx, &pubkey_obj, privkey.data())) {
            secp256k1_context_destroy(ctx);
            return false;
        }

        std::vector<uint8_t> pubkey(33);
        size_t pubkey_len = 33;
        secp256k1_ec_pubkey_serialize(ctx, pubkey.data(), &pubkey_len, &pubkey_obj, SECP256K1_EC_COMPRESSED);
        info_out.pubkey = pubkey;
    }

    secp256k1_context_destroy(ctx);

    // Calculate real BIP32 master key fingerprint
    info_out.master_fingerprint = CalculateMasterFingerprint();

    // Build BIP32 path: m/purpose'/coin_type'/0'/chain/index
    info_out.path = {
        HARDENED | purpose,      // 84' (BIP84 SegWit) or 86' (BIP86 Taproot)
        HARDENED | coin_type_,   // coin_type' (SLIP-44 Dinero coin type)
        HARDENED | 0,            // 0' (account)
        chain,                   // 0 (receive), 1 (change), or 2 (mining)
        idx                      // address index
    };

    return true;
}

std::vector<uint8_t> HDWallet::GetPublicKey(const std::vector<uint8_t>& private_key) const {
  if (private_key.size() != 32) {
    throw std::invalid_argument("Private key must be 32 bytes");
  }

  secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

  secp256k1_pubkey pubkey_obj;
  if (!secp256k1_ec_pubkey_create(ctx, &pubkey_obj, private_key.data())) {
    secp256k1_context_destroy(ctx);
    throw std::runtime_error("Failed to create public key");
  }

  std::vector<uint8_t> pubkey(33);
  size_t pubkey_len = 33;
  secp256k1_ec_pubkey_serialize(ctx, pubkey.data(), &pubkey_len, &pubkey_obj, SECP256K1_EC_COMPRESSED);

  secp256k1_context_destroy(ctx);
  return pubkey;
}

// ============================================================================
// BIP86 Taproot Mining Address Functions (m/86'/1448'/0'/2/index)
// ============================================================================

std::string HDWallet::DeriveNextTaprootMiningAddress() {
  std::string addr = GetTaprootMiningAddressAt(taproot_mining_index_);
  taproot_mining_to_index_[addr] = taproot_mining_index_;
  taproot_mining_index_++;
  Save();
  return addr;
}

std::string HDWallet::GetTaprootMiningAddressAt(uint32_t index) const {
  // BIP86: m/86'/coin_type'/0'/2/index (mining chain)

  // BIP32 derivation using consolidated deriver (replaces inline lambdas)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(2);      // Chain 2 (mining)
  deriver.deriveNormal(index);

  // Get x-only pubkey and compute BIP341 tweaked output key via canonical function
  auto xonly_bytes = deriver.getXOnlyPubkey();
  std::array<uint8_t, 32> output_key_arr{};
  if (!dinero::TaprootKeys::ComputeTweakedPubkey(xonly_bytes, output_key_arr)) {
    throw std::runtime_error("Failed to compute Taproot output key");
  }

  // Encode as Bech32m (witness version 1)
  std::string hrp = dinero::Params().hrp;
  if (hrp.empty()) {
    hrp = "din";
  }

  std::vector<uint8_t> program(output_key_arr.begin(), output_key_arr.end());
  std::string address = bech32::Encode(hrp, 1, program, bech32::Encoding::BECH32M);

  return address;
}

std::vector<uint8_t> HDWallet::GetTaprootMiningPrivateKeyAt(uint32_t index) const {
  // BIP86: m/86'/coin_type'/0'/2/index (mining chain)

  // BIP32 derivation using consolidated deriver (replaces inline lambdas)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(2);      // Chain 2 (mining)
  deriver.deriveNormal(index);

  auto k = deriver.getPrivateKey();
  auto xonly_bytes = deriver.getXOnlyPubkey();

  // BIP341 TapTweak via canonical implementation
  std::array<uint8_t, 32> priv_arr, xonly_arr, tweaked_arr;
  std::copy(k.begin(), k.end(), priv_arr.begin());
  std::copy(xonly_bytes.begin(), xonly_bytes.end(), xonly_arr.begin());
  if (!dinero::TaprootKeys::ComputeTweakedPrivkey(priv_arr, xonly_arr, tweaked_arr)) {
    throw std::runtime_error("Failed to compute tweaked Taproot mining private key");
  }
  return std::vector<uint8_t>(tweaked_arr.begin(), tweaked_arr.end());
}

std::vector<uint8_t> HDWallet::GetTaprootChangePrivateKeyAt(uint32_t index) const {
  // BIP86: m/86'/coin_type'/0'/1/index (change chain)

  // BIP32 derivation using consolidated deriver (replaces inline lambdas)
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveNormal(1);      // Chain 1 (change)
  deriver.deriveNormal(index);

  auto k = deriver.getPrivateKey();
  auto xonly_bytes = deriver.getXOnlyPubkey();

  // BIP341 TapTweak via canonical implementation
  std::array<uint8_t, 32> priv_arr, xonly_arr, tweaked_arr;
  std::copy(k.begin(), k.end(), priv_arr.begin());
  std::copy(xonly_bytes.begin(), xonly_bytes.end(), xonly_arr.begin());
  if (!dinero::TaprootKeys::ComputeTweakedPrivkey(priv_arr, xonly_arr, tweaked_arr)) {
    throw std::runtime_error("Failed to compute tweaked Taproot change private key");
  }
  return std::vector<uint8_t>(tweaked_arr.begin(), tweaked_arr.end());
}


// ============================================================================
// B4: Panic & Recovery Key Derivation (Safety Profiles)
// ============================================================================

std::vector<uint8_t> HDWallet::GetPanicPrivateKeyAt(uint32_t index) const {
  // m/86'/1448'/0'/100'/index — panic chain (hardened for isolation)
  // Raw (untweaked) key for script-path spending
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveHardened(100);    // Panic chain (hardened)
  deriver.deriveNormal(index);

  auto priv = deriver.getPrivateKey();
  return std::vector<uint8_t>(priv.begin(), priv.end());
}

std::vector<uint8_t> HDWallet::GetPanicPublicKeyAt(uint32_t index) const {
  // m/86'/1448'/0'/100'/index — x-only pubkey for panic leaf
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveHardened(100);
  deriver.deriveNormal(index);

  auto pub = deriver.getXOnlyPubkey();
  return std::vector<uint8_t>(pub.begin(), pub.end());
}

std::vector<uint8_t> HDWallet::GetRecoveryPrivateKeyAt(uint32_t index) const {
  // m/86'/1448'/0'/101'/index — recovery chain (hardened for isolation)
  // Raw (untweaked) key for script-path spending
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveHardened(101);    // Recovery chain (hardened)
  deriver.deriveNormal(index);

  auto priv = deriver.getPrivateKey();
  return std::vector<uint8_t>(priv.begin(), priv.end());
}

std::vector<uint8_t> HDWallet::GetRecoveryPublicKeyAt(uint32_t index) const {
  // m/86'/1448'/0'/101'/index — x-only pubkey for recovery leaf
  dinero::BIP32Deriver deriver(seed_.data(), 64);
  deriver.deriveHardened(86);
  deriver.deriveHardened(coin_type_);
  deriver.deriveHardened(0);
  deriver.deriveHardened(101);
  deriver.deriveNormal(index);

  auto pub = deriver.getXOnlyPubkey();
  return std::vector<uint8_t>(pub.begin(), pub.end());
}
