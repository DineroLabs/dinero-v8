#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>
#include "primitives/uint256.h"
#include "wallet/canonical_wallet_utxo.h"  // Phase M.3: THE canonical UTXO type

// Forward declarations
namespace dinero {
  class UTXOIndex;
  class PSBT;
  class ChainHeightProvider;  // P1: For chain height access (abstraction layer)
}

// ═══════════════════════════════════════════════════════════════════════════
// ENFORCEMENT: Phase M.3 Lock (Dec 27, 2025)
// ═══════════════════════════════════════════════════════════════════════════
// HDWalletUTXO DELETED - was <24h temporary artifact, superseded same day
//
// THE ONLY wallet UTXO type: dinero::CanonicalWalletUTXO
//
// Deleted fields (now computed at boundaries):
//   - address (derived from spk)
//   - confirmations (computed from height)
//   - scriptPubKey (renamed to spk)
//
// Bitcoin-style invariant: script + height authoritative, rest derived.
// ═══════════════════════════════════════════════════════════════════════════

struct WalletBalance {
  uint64_t confirmed = 0;      // spendable, mature coins
  uint64_t unconfirmed = 0;    // 0-conf transactions
  uint64_t immature = 0;       // coinbase < 100 confirmations
  uint64_t total = 0;          // confirmed + unconfirmed + immature
};

class HDWallet {
public:
  struct LegacyMigrationResult {
    bool success = false;
    bool migrated = false;
    bool already_migrated = false;
    std::string message;
    std::string wallet_state_path;
    std::string legacy_wallet_conf_path;
    std::string backup_path;
  };

  // Open existing wallet (loads from disk)
  // enable_autolock: false for tests to prevent background thread from blocking
  static std::unique_ptr<HDWallet> Open(const std::string& datadir, uint32_t coin_type, bool enable_autolock = true);

  // Create new wallet with BIP39 mnemonic generation
  static std::unique_ptr<HDWallet> CreateNew(const std::string& datadir, uint32_t coin_type, std::string& mnemonic_out, bool enable_autolock = true);

  // Restore wallet from BIP39 mnemonic
  static std::unique_ptr<HDWallet> Restore(const std::string& datadir, uint32_t coin_type, const std::string& mnemonic, const std::string& passphrase = "", bool enable_autolock = true);

  // Explicit one-time legacy sidecar migration: wallet.conf -> wallet_state.db
  static LegacyMigrationResult MigrateLegacySidecarToStateDb(
      const std::string& datadir,
      uint32_t coin_type,
      bool create_backup = true,
      bool overwrite_existing = false);

  // Destructor - stops auto-lock thread
  ~HDWallet();

  // ═══════════════════════════════════════════════════════════════════════════
  // LEGACY BIP84 (P2WPKH) - For backward compatibility only
  // ═══════════════════════════════════════════════════════════════════════════
  // WARNING: These methods are LEGACY. For new addresses, use BIP86 Taproot methods.
  // BIP84 is kept only for:
  //   - Importing old wallets
  //   - Scanning historical chains
  //   - Interoperability with older tooling
  //   - Legacy Lightning internals
  // DO NOT use for new user-facing wallet addresses.
  // ═══════════════════════════════════════════════════════════════════════════

  // LEGACY: BIP84 receive (m/84'/1448'/0'/0/index) - prefer DeriveNextTaprootAddress()
  std::string DeriveNextAddress();     // returns din1… (P2WPKH)
  uint32_t    CurrentIndex() const { return index_; }
  std::string GetAddressAt(uint32_t index) const;
  std::vector<std::string> GetAllAddresses();

  // LEGACY: BIP84 change (m/84'/1448'/0'/1/index) - prefer DeriveNextTaprootChangeAddress()
  std::string DeriveNextChangeAddress();
  uint32_t    CurrentChangeIndex() const { return change_index_; }
  std::string GetChangeAddressAt(uint32_t index) const;

  // LEGACY: BIP84 mining (m/84'/1448'/0'/2/index) - prefer DeriveNextTaprootMiningAddress()
  std::string DeriveNextMiningAddress();
  uint32_t    CurrentMiningIndex() const { return mining_index_; }
  std::string GetMiningAddressAt(uint32_t index);
  std::vector<uint8_t> GetMiningPrivateKeyAt(uint32_t index) const;
  
  // UTXO tracking connection (replacement for SimpleBlockchain)
  void ConnectUTXOIndex(class dinero::UTXOIndex* utxo_index);
  void ConnectChainHeightProvider(class dinero::ChainHeightProvider* provider);  // P1: Wire chain height for maturity checks
  void RegisterAddresses();  // Register all derived addresses with UTXO index

  // Balance queries (CRITICAL)
  WalletBalance GetBalance() const;

  // ═══════════════════════════════════════════════════════════════════════════
  // Phase M.3: CANONICAL UTXO QUERY
  // ═══════════════════════════════════════════════════════════════════════════
  // Returns wallet's UTXOs using CanonicalWalletUTXO (THE standard type).
  // Implementation MUST use blockchain_->get_utxo_set()->IsOurScript() to filter.
  // See compile-time guard in hd_wallet.cpp line ~768.
  std::vector<dinero::CanonicalWalletUTXO> ListUTXOs(uint32_t min_confirmations = 1, bool allow_unconfirmed_change = false) const;
  
  // Transaction creation (CRITICAL)
  struct TxOutput {
    std::string address;
    uint64_t value;
  };
  bool CreateTransaction(const std::vector<TxOutput>& outputs, uint64_t fee_rate, std::string& tx_hex_out, std::string& error_out);

  // PSBT creation with proper BIP32 metadata (for hardware wallet signing)
  bool CreatePSBT(const std::vector<TxOutput>& outputs, uint64_t fee_rate, class dinero::PSBT& psbt_out, std::string& error_out);

  // Fill PSBT with BIP32 derivation paths and wallet metadata
  void FillPSBT(class dinero::PSBT& psbt);

  // Get BIP32 derivation info for an address (for PSBT metadata)
  struct DerivationInfo {
    std::vector<uint8_t> pubkey;       // 33-byte compressed pubkey
    uint32_t master_fingerprint;        // 4-byte master key fingerprint
    std::vector<uint32_t> path;         // BIP32 path (e.g., [84'|0x80000054, 1448'|0x800005A8, 0'|0x80000000, 0, index])
  };
  bool GetDerivationInfo(const std::string& address, DerivationInfo& info_out) const;

  // Private key access (for signing)
  std::vector<uint8_t> GetPrivateKeyAt(uint32_t index) const;
  std::vector<uint8_t> GetChangePrivateKeyAt(uint32_t index) const;  // Get private key for change address

  // Seed access (for Lightning key derivation via LightningKeyDeriver)
  // SECURITY: Caller must zeroize after use
  std::vector<uint8_t> GetSeed() const { return seed_; }

  // ═══════════════════════════════════════════════════════════════════════════
  // PRIMARY: BIP86 Taproot Address Generation (m/86'/1448'/0')
  // ═══════════════════════════════════════════════════════════════════════════
  // These are the CONSTITUTIONAL wallet methods. Use these for:
  //   - All new wallets
  //   - All user-facing addresses
  //   - Descriptor exports
  //   - Cross-language consistency (vector-tested with Rust/Swift/C++)
  // ═══════════════════════════════════════════════════════════════════════════

  /**
   * @brief Derive next Taproot address (BIP86: m/86'/1448'/0'/0/index)
   * @return Bech32m encoded P2TR address
   * @note PRIMARY method for new receive addresses
   */
  std::string DeriveNextTaprootAddress();

  /**
   * @brief Get current Taproot address index
   * @return Current taproot_index_
   */
  uint32_t CurrentTaprootIndex() const { return taproot_index_; }

  /**
   * @brief Derive Taproot address at specific index (BIP86: m/86'/1448'/0'/0/index)
   * @param index Address index (non-hardened)
   * @return Bech32m encoded P2TR address
   */
  std::string GetTaprootAddressAt(uint32_t index) const;

  /**
   * @brief Derive Taproot change address at specific index (BIP86: m/86'/1448'/0'/1/index)
   * @param index Address index (non-hardened)
   * @return Bech32m encoded P2TR change address
   */
  std::string GetTaprootChangeAddressAt(uint32_t index) const;

  /**
   * @brief Derive next Taproot change address
   * @return Bech32m encoded P2TR change address
   */
  std::string DeriveNextTaprootChangeAddress();

  /**
   * @brief Get Taproot private key at specific index (for signing)
   * @param index Address index
   * @return 32-byte Schnorr private key (tweaked)
   */
  std::vector<uint8_t> GetTaprootPrivateKeyAt(uint32_t index) const;

  /**
   * @brief Get Taproot change private key at specific index (for signing)
   * @param index Change address index
   * @return 32-byte Schnorr private key (tweaked)
   */
  std::vector<uint8_t> GetTaprootChangePrivateKeyAt(uint32_t index) const;

  // ===== BIP86 Taproot Mining Address Generation (m/86'/1448'/0'/2/index) =====

  /**
   * @brief Derive next Taproot mining address (BIP86: m/86'/1448'/0'/2/index)
   *
   * Mining addresses use Taproot for consistency with regular receives.
   * This ensures all addresses use the same modern standard.
   *
   * @return Bech32m encoded P2TR mining address (din1p...)
   */
  std::string DeriveNextTaprootMiningAddress();

  /**
   * @brief Get current Taproot mining address index
   * @return Current taproot_mining_index_
   */
  uint32_t CurrentTaprootMiningIndex() const { return taproot_mining_index_; }

  /**
   * @brief Get Taproot mining address at specific index (BIP86: m/86'/1448'/0'/2/index)
   * @param index Mining address index (non-hardened)
   * @return Bech32m encoded P2TR mining address
   */
  std::string GetTaprootMiningAddressAt(uint32_t index) const;

  /**
   * @brief Get Taproot mining private key at specific index (for signing coinbase)
   * @param index Mining address index
   * @return 32-byte Schnorr private key (tweaked)
   */
  std::vector<uint8_t> GetTaprootMiningPrivateKeyAt(uint32_t index) const;

  /**
   * @brief Get public key from private key (compressed, 33 bytes)
   * @param private_key 32-byte private key
   * @return 33-byte compressed public key
   */
  std::vector<uint8_t> GetPublicKey(const std::vector<uint8_t>& private_key) const;

  // ===== B4: Panic & Recovery Key Derivation (Safety Profiles) =====

  /**
   * @brief Get panic private key at index (m/86'/1448'/0'/100'/index)
   * Used for panic-cancel spending via script-path.
   * Chain 100 is hardened for isolation from regular spending keys.
   * @param index Key index (non-hardened final step)
   * @return 32-byte private key (caller must zeroize after use)
   */
  std::vector<uint8_t> GetPanicPrivateKeyAt(uint32_t index) const;

  /**
   * @brief Get panic x-only public key at index
   * @param index Key index
   * @return 32-byte x-only public key
   */
  std::vector<uint8_t> GetPanicPublicKeyAt(uint32_t index) const;

  /**
   * @brief Get recovery private key at index (m/86'/1448'/0'/101'/index)
   * Used for disaster recovery spending via script-path.
   * Chain 101 is hardened for isolation.
   * @param index Key index (non-hardened final step)
   * @return 32-byte private key (caller must zeroize after use)
   */
  std::vector<uint8_t> GetRecoveryPrivateKeyAt(uint32_t index) const;

  /**
   * @brief Get recovery x-only public key at index
   * @param index Key index
   * @return 32-byte x-only public key
   */
  std::vector<uint8_t> GetRecoveryPublicKeyAt(uint32_t index) const;

  // Mnemonic access (for backup)
  std::string GetMnemonic() const;
  
  // Encryption methods (Argon2id + AES-256-GCM)
  bool EncryptWallet(const std::string& password);
  bool Lock();
  bool Unlock(const std::string& password);
  bool ChangePassword(const std::string& old_password, const std::string& new_password);
  bool IsEncrypted() const { return encrypted_; }
  bool IsLocked() const { return locked_; }
  
  // Auto-lock timeout (security feature)
  void ResetAutoLockTimer();  // Call after any RPC activity that requires unlocked wallet
  void SetAutoLockTimeout(int seconds);  // Set auto-lock timeout (default: 900 = 15 min)
  int GetAutoLockTimeout() const { return autolock_seconds_; }

  // Calculate BIP32 master key fingerprint (first 4 bytes of HASH160 of master pubkey)
  // This is public to allow RPC handlers to get fingerprint for wallet metadata
  uint32_t CalculateMasterFingerprint() const;

  /**
   * @brief Get master fingerprint as 8-character hex string (for descriptors)
   * @return Master fingerprint in lowercase hex (e.g., "8a2b3c4d")
   */
  std::string GetMasterFingerprintHex() const;

  /**
   * @brief Get account extended public key (xpub) for descriptors
   *
   * Derives the BIP32 account-level extended public key at path m/84'/coin_type'/account'.
   * This xpub can be used in descriptor strings for deterministic address derivation.
   *
   * @param account Account index (default: 0)
   * @return Base58-encoded xpub string
   */
  std::string GetAccountXpub(uint32_t account = 0) const;

  // Accessors
  const std::string& GetWalletDir() const { return wallet_dir_; }

private:
  HDWallet(std::string wdir, uint32_t coin_type, bool enable_autolock = true);
  void LoadOrCreate();
  void Save() const;

  // BIP32 derivation from 64-byte seed (no mnemonics here; seed is random & stored)
  std::string DeriveAddressAt(uint32_t index) const;
  std::string DeriveChangeAddressAt(uint32_t index) const;
  std::string DeriveMiningAddressAt(uint32_t index) const;  // Mining chain derivation

  // Helpers
  static std::vector<uint8_t> ReadFile(const std::string& path);
  static void WriteFile(const std::string& path, const std::string& content);
  static std::string ToHex(const uint8_t* d, size_t n);
  static bool FromHex(const std::string& hex, std::vector<uint8_t>& out);
  static bool GetRandomBytes(uint8_t* out, size_t n);

private:
  std::string wallet_dir_;
  std::string wallet_file_;
  uint32_t    coin_type_;
  uint32_t    index_{0};               // Receive address index (m/84'/1448'/0'/0/index)
  uint32_t    change_index_{0};        // Change address index (m/84'/1448'/0'/1/index)
  uint32_t    mining_index_{0};        // Mining address index (m/84'/1448'/0'/2/index) [LEGACY SegWit]
  uint32_t    taproot_index_{0};       // Taproot receive index (m/86'/1448'/0'/0/index)
  uint32_t    taproot_change_index_{0}; // Taproot change index (m/86'/1448'/0'/1/index)
  uint32_t    taproot_mining_index_{0}; // Taproot mining index (m/86'/1448'/0'/2/index) [RECOMMENDED]
  // 64-byte seed, never logged
  std::vector<uint8_t> seed_;
  // BIP39 mnemonic phrase (for backup/recovery)
  std::string mnemonic_;
  
  // UTXO tracking connection (replacement for SimpleBlockchain)
  class dinero::UTXOIndex* utxo_index_ = nullptr;

  // Chain height provider (P1: for chain height and maturity checks)
  class dinero::ChainHeightProvider* chain_height_provider_ = nullptr;

  // Encryption state
  bool encrypted_ = false;
  bool locked_ = false;
  std::vector<uint8_t> password_salt_;      // Salt for password verification
  std::vector<uint8_t> password_hash_;      // Argon2id hash of password
  std::vector<uint8_t> encrypted_seed_;     // AES-256-GCM encrypted seed
  
  // Auto-lock timeout (security feature)
  std::thread autolock_thread_;             // Background thread for auto-lock
  std::atomic<bool> autolock_running_;      // Flag to control auto-lock thread
  std::atomic<time_t> unlock_time_;         // Timestamp when wallet was unlocked
  int autolock_seconds_ = 900;              // Auto-lock timeout in seconds (default: 15 min)
  std::mutex autolock_mutex_;                // Mutex for thread safety
  
  void AutoLockThread();  // Background thread function
  
  // Address cache (address -> index)
  std::map<std::string, uint32_t> address_to_index_;         // Receive addresses (chain 0)
  std::map<std::string, uint32_t> change_address_to_index_;  // Change addresses (chain 1)
  std::map<std::string, uint32_t> taproot_address_to_index_; // Taproot receive addresses
  std::map<std::string, uint32_t> taproot_change_to_index_;  // Taproot change addresses
  std::map<std::string, uint32_t> taproot_mining_to_index_;  // Taproot mining addresses

  // Helper: Convert address to scriptPubKey
  std::vector<uint8_t> AddressToScriptPubKey(const std::string& address) const;
};
