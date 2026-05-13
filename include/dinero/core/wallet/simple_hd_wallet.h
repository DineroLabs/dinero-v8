#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SimpleHDWallet {
public:
  static std::unique_ptr<SimpleHDWallet> Open(const std::string& datadir, uint32_t coin_type);

  // Derive next bech32m P2TR address: m/86'/coin_type'/0'/0/i (i persisted)
  std::string DeriveNextAddress();     // returns din1p…
  uint32_t    CurrentIndex() const { return index_; }

private:
  SimpleHDWallet(std::string wdir, uint32_t coin_type);
  void LoadOrCreate();
  void Save() const;

  // BIP32 derivation from 64-byte seed (no mnemonics here; seed is random & stored)
  std::string DeriveAddressAt(uint32_t index) const;

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
  uint32_t    index_{0};
  // 64-byte seed, never logged
  std::vector<uint8_t> seed_;
};
