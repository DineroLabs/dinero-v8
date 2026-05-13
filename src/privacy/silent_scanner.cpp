#include "privacy/silent_scanner.h"
#include "crypto/evp_secp256k1.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdexcept>
#include <vector>
#include <cstring>

#include "common/sha256d.h"

namespace din::sp {

static void tagged_hash(const char* tag, const uint8_t* msg, size_t msglen, uint8_t out32[32]) {
  uint8_t th[32];
  Dinero::Common::sha256 hasher;
  hasher.update(reinterpret_cast<const uint8_t*>(tag), std::strlen(tag));
  auto th_result = hasher.finalize();
  std::copy(th_result.begin(), th_result.end(), th);
  
  std::vector<uint8_t> buf; buf.reserve(64 + msglen);
  buf.insert(buf.end(), th, th+32);
  buf.insert(buf.end(), th, th+32);
  buf.insert(buf.end(), msg, msg+msglen);
  
  Dinero::Common::sha256 final_hasher;
  final_hasher.update(buf.data(), buf.size());
  auto final_result = final_hasher.finalize();
  std::copy(final_result.begin(), final_result.end(), out32);
}

static void put_le32(uint32_t v, uint8_t out[4]) {
  out[0]=uint8_t(v); out[1]=uint8_t(v>>8); out[2]=uint8_t(v>>16); out[3]=uint8_t(v>>24);
}

Scanner::Scanner(const std::array<uint8_t,32>& scan_priv,
                 const std::array<uint8_t,33>& spend_pub)
: b_scan_(scan_priv), B_spend_(spend_pub) {}

std::vector<Detection> Scanner::scan_tx(const TxView& tx) {
  std::vector<Detection> hits;
  if (tx.tap_outputs_xonly.empty()) return hits;

  secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  if (!ctx) throw std::runtime_error("secp ctx null");

  // 1) Build A = sum of eligible input pubkeys (tap xonly → sec1 even, plus legacy sec1)
  std::vector<secp256k1_pubkey> pubs;
  for (const auto& s33 : tx.input_pubkeys_sec1) {
    secp256k1_pubkey pk;
    if (secp256k1_ec_pubkey_parse(ctx, &pk, s33.data(), s33.size()))
      pubs.push_back(pk);
  }
  for (const auto& xonly : tx.input_tap_xonly) {
    uint8_t sec1[33]; sec1[0]=0x02; std::memcpy(sec1+1, xonly.data(), 32); // even-Y
    secp256k1_pubkey pk;
    if (secp256k1_ec_pubkey_parse(ctx, &pk, sec1, 33))
      pubs.push_back(pk);
  }
  if (pubs.empty()) return hits; // not eligible

  secp256k1_pubkey A;
  const secp256k1_pubkey* arr = pubs.data();
  if (!secp256k1_ec_pubkey_combine(ctx, &A, &arr, pubs.size()))
    return hits;

  unsigned char A33[33]; size_t A33len=33;
  if (!secp256k1_ec_pubkey_serialize(ctx, A33, &A33len, &A, SECP256K1_EC_COMPRESSED))
    return hits;

  // 2) input_hash = H_tag("din-sp-input", outpoint_L || serP(A))
  std::vector<uint8_t> ih; ih.reserve(36+33);
  ih.insert(ih.end(), tx.outpoint_L_le.begin(), tx.outpoint_L_le.end());
  ih.insert(ih.end(), A33, A33+33);
  uint8_t input_hash[32]; tagged_hash("din-sp-input", ih.data(), ih.size(), input_hash);

  // s = input_hash * b_scan (scalar)
  unsigned char s32[32]; std::memcpy(s32, input_hash, 32);
  if (!secp256k1_ec_seckey_verify(ctx, s32)) return hits;
  if (!secp256k1_ec_seckey_tweak_mul(ctx, s32, b_scan_.data())) return hits;

  // E = s * A (multiply A by scalar s)
  if (!secp256k1_ec_pubkey_tweak_mul(ctx, &A, s32)) return hits;
  unsigned char E33[33]; size_t E33len=33;
  if (!secp256k1_ec_pubkey_serialize(ctx, E33, &E33len, &A, SECP256K1_EC_COMPRESSED)) return hits;

  // For each k index we care about (usually 0), compute P_k and match outputs
  uint8_t idx4[4]; put_le32(0, idx4);
  uint8_t tk[32];
  {
    std::vector<uint8_t> tmsg; tmsg.reserve(33+4);
    tmsg.insert(tmsg.end(), E33, E33+33);
    tmsg.insert(tmsg.end(), idx4, idx4+4);
    tagged_hash("din-sp-tweak", tmsg.data(), tmsg.size(), tk);
    if (!secp256k1_ec_seckey_verify(ctx, tk)) return hits;
  }

  // P_k = B_spend + tk*G
  secp256k1_pubkey Bsp;
  if (!secp256k1_ec_pubkey_parse(ctx, &Bsp, B_spend_.data(), B_spend_.size())) return hits;
  if (!secp256k1_ec_pubkey_tweak_add(ctx, &Bsp, tk)) return hits;

  secp256k1_xonly_pubkey X;
  int parity=0;
  if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &X, &parity, &Bsp)) return hits;

  unsigned char X32[32];
  if (!secp256k1_xonly_pubkey_serialize(ctx, X32, &X)) return hits;

  // Compare against tap outputs in tx
  for (size_t i=0;i<tx.tap_outputs_xonly.size();++i) {
    if (std::memcmp(tx.tap_outputs_xonly[i].data(), X32, 32)==0) {
      Detection d; d.output_index=i; d.tap_output_xonly = tx.tap_outputs_xonly[i];
      // Optionally fill spend_priv_candidate if wallet has a derivation; leave zeroed otherwise.
      hits.push_back(d);
    }
  }
  return hits;
}

} // namespace din::sp
