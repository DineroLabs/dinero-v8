#include "privacy/silent_payments.h"
#include "external/bech32/bech32.hpp"
#include "common/sha256d.h"
#include "dinero/core/crypto/ripemd160.h"
#include <stdexcept>
#include <sstream>

namespace din::sp {

// Bech32m encoding for Silent Payment addresses
std::string encode_bech32m(const Address& a, Net n) {
  std::string hrp;
  switch (n) {
    case Net::Main:    hrp = "dsp"; break;
    case Net::Test:    hrp = "tdsp"; break;
    case Net::Regtest: hrp = "rdsp"; break;
    default: throw std::runtime_error("unknown network");
  }
  
  // Payload: scan_pub (33) + spend_pub (33) = 66 bytes
  std::vector<uint8_t> payload;
  payload.insert(payload.end(), a.scan_pub.begin(), a.scan_pub.end());
  payload.insert(payload.end(), a.spend_pub.begin(), a.spend_pub.end());
  
  // Use external bech32 library
  return bech32::Encode(hrp, 0, payload);
}

Address decode_bech32m(const std::string& s, Net expected) {
  std::string expected_hrp;
  switch (expected) {
    case Net::Main:    expected_hrp = "dsp"; break;
    case Net::Test:    expected_hrp = "tdsp"; break;
    case Net::Regtest: expected_hrp = "rdsp"; break;
    default: throw std::runtime_error("unknown network");
  }
  
  Address addr;
  
  // Parse HRP and payload
  auto at_pos = s.find('1');
  if (at_pos == std::string::npos) throw std::runtime_error("invalid bech32m format");
  
  std::string hrp = s.substr(0, at_pos);
  if (hrp != expected_hrp) throw std::runtime_error("wrong HRP");
  
  // Decode payload (should be 66 bytes: 33 + 33)
  auto decode_result = bech32::Decode(expected_hrp, s);
  if (!decode_result) {
    throw std::runtime_error("bech32 decode failed");
  }
  
  std::vector<uint8_t> payload = decode_result->program;
  if (payload.size() != 66) {
    throw std::runtime_error("invalid payload size");
  }
  
  // Extract scan and spend pubkeys
  std::copy(payload.begin(), payload.begin() + 33, addr.scan_pub.begin());
  std::copy(payload.begin() + 33, payload.end(), addr.spend_pub.begin());
  
  return addr;
}

// Derive labeled spend pubkey: B_m = B_spend + H(label)·G
std::array<uint8_t,33> label_spend_pub(const std::array<uint8_t,33>& spend_pub,
                                       const std::array<uint8_t,32>& scan_priv,
                                       uint32_t label) {
  // TODO: Implement labeled spend pubkey derivation
  // This requires secp256k1 point operations
  // For now, return the original spend pubkey
  return spend_pub;
}

} // namespace din::sp
