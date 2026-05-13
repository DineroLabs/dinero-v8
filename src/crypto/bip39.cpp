#include "bip39.hpp"
#include "crypto/pbkdf2.h"
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include "third_party/bip39/english_words.hpp"

// Forward declarations for existing crypto functions
extern void sha256(const uint8_t* data, size_t len, uint8_t out32[32]);

namespace dinero::bip39 {

// --- tiny utils ---
std::vector<std::string> split(const std::string& s){
  std::istringstream iss(s); std::vector<std::string> out; std::string w;
  while (iss >> w) out.push_back(w); return out;
}
std::string join(const std::vector<std::string>& v, const char* sep){
  std::ostringstream oss; for(size_t i=0;i<v.size();++i){ if(i) oss<<sep; oss<<v[i]; } return oss.str();
}

static void bits_append(std::vector<bool>& bits, uint8_t byte) {
  for (int i=7;i>=0;--i) bits.push_back((byte>>i)&1);
}

static uint8_t checksum_bits(const uint8_t* entropy, size_t len) {
  uint8_t h[32]; sha256(entropy, len, h);
  // CS = ENT/32 (bits) -> return the first CS bits of sha256(entropy)
  return h[0]; // caller will mask appropriately
}

std::string mnemonic_from_entropy(const uint8_t* ent, size_t ent_len) {
  const size_t ENT = ent_len*8;
  if (ENT < 128 || ENT > 256 || (ENT % 32)!=0) throw std::invalid_argument("entropy size");

  std::vector<bool> bits; bits.reserve(ENT + ENT/32);
  for (size_t i=0;i<ent_len;++i) bits_append(bits, ent[i]);

  uint8_t cs_first = checksum_bits(ent, ent_len);
  int CS = ENT / 32;
  for (int i=0;i<CS; ++i) bits.push_back( (cs_first >> (7-i)) & 1 );

  // Split into 11-bit words
  std::vector<std::string> words; words.reserve((ENT+CS)/11);
  for (size_t i=0; i<bits.size(); i+=11) {
    unsigned idx = 0;
    for (size_t j=0;j<11;j++) { idx = (idx<<1) | (i+j<bits.size()? bits[i+j]:0); }
    words.emplace_back(kBip39English[idx]);
  }
  return join(words);
}

void mnemonic_to_seed(const std::string& mnemonic, const std::string& passphrase, uint8_t out64[64]) {
  // Redirect to canonical PBKDF2 — no hand-rolled PBKDF2 here.
  // Previous implementation had its own PBKDF2 loop using hmac_sha512 directly.
  // Now uses the single canonical PBKDF2_HMAC_SHA512 from pbkdf2.cpp.
  std::string salt = std::string("mnemonic") + passphrase;
  dinero::crypto::PBKDF2_HMAC_SHA512(
      reinterpret_cast<const uint8_t*>(mnemonic.data()), mnemonic.size(),
      reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
      2048, out64, 64);
}

} // namespace dinero::bip39
