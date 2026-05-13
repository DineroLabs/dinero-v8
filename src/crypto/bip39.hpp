#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::bip39 {

// Return space-separated mnemonic from raw entropy (128..256 bits, multiple of 32)
std::string mnemonic_from_entropy(const uint8_t* entropy, size_t len);

// PBKDF2-HMAC-SHA512(mnemonic, "mnemonic"+passphrase, 2048) -> 64-byte seed
void mnemonic_to_seed(const std::string& mnemonic,
                      const std::string& passphrase,
                      uint8_t out64[64]);

// Utility: split/join
std::vector<std::string> split(const std::string& s);
std::string join(const std::vector<std::string>& v, const char* sep = " ");

} // namespace dinero::bip39
