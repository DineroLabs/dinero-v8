#include "privacy/silent_sender.h"
#include "privacy/silent_payments.h"
#include "crypto/evp_secp256k1.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <array>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>

#include "common/sha256d.h"

namespace din::sp {

// --- small helpers ---

static void tagged_hash(const char* tag, const uint8_t* msg, size_t msglen, uint8_t out32[32]) {
  uint8_t th[32];
  Dinero::Common::sha256 hasher;
  hasher.update(reinterpret_cast<const uint8_t*>(tag), std::strlen(tag));
  auto th_result = hasher.finalize();
  std::copy(th_result.begin(), th_result.end(), th);
  
  // H(tag || tag || msg)
  std::vector<uint8_t> buf; buf.reserve(64 + msglen);
  buf.insert(buf.end(), th, th+32);
  buf.insert(buf.end(), th, th+32);
  buf.insert(buf.end(), msg, msg+msglen);
  
  Dinero::Common::sha256 final_hasher;
  final_hasher.update(buf.data(), buf.size());
  auto final_result = final_hasher.finalize();
  std::copy(final_result.begin(), final_result.end(), out32);
}

static int sec1_from_xonly_even(const std::array<uint8_t,32>& x, std::array<uint8_t,33>& out33) {
  out33[0] = 0x02; // even-Y
  std::memcpy(out33.data()+1, x.data(), 32);
  return 1;
}

static void scalar_mod_n(uint8_t s[32]) {
  // libsecp will validate scalars; no explicit mod here, rely on API returns.
  // (We still keep bytes as-is; rejects zero/overflow later.)
}

// Concatenate helper
static std::vector<uint8_t> concat(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
  std::vector<uint8_t> r; r.reserve(a.size()+b.size()); r.insert(r.end(), a.begin(), a.end()); r.insert(r.end(), b.begin(), b.end()); return r;
}

// Serialize 32-bit little-endian
static void put_le32(uint32_t v, uint8_t out[4]) {
  out[0] = uint8_t(v & 0xFF);
  out[1] = uint8_t((v >> 8) & 0xFF);
  out[2] = uint8_t((v >> 16) & 0xFF);
  out[3] = uint8_t((v >> 24) & 0xFF);
}

std::array<uint8_t,32> derive_sp_taproot_key(const DeriveParams& P) {
  // Pre: each input contains the private key (even-Y normalized for tap keys)
  // Note: You MUST provide correct parity-normalized priv for tap inputs; if not, normalize here.

  // 0) Init secp
  secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  if (!ctx) throw std::runtime_error("secp ctx null");

  // 1) Build A (sum of public keys) and 'a' (sum scalar) from inputs
  std::vector<secp256k1_pubkey> pubs;
  pubs.reserve(P.vin.size());

  // track scalar a = sum(priv_i) mod n
  // libsecp wants scalars as 32 bytes
  unsigned char a32[32] = {0};

  for (const auto& in : P.vin) {
    // Parse priv_i → add to a32
    if (!secp256k1_ec_seckey_verify(ctx, in.priv.data())) {
      throw std::runtime_error("invalid input privkey");
    }
    // a32 = a32 + in.priv (mod n)
    unsigned char tmp[32]; std::memcpy(tmp, in.priv.data(), 32);
    if (!secp256k1_ec_seckey_tweak_add(ctx, a32, tmp)) {
      // if a32 is zero initially, tweak_add with tmp is equivalent to set=a+tmp; ensure a32 ≠ 0 after.
      // Workaround: if a32==0, copy tmp into a32; else tweak_add works.
      std::memcpy(a32, in.priv.data(), 32);
    }

    // Parse public key for combine (SEC1 33 for non-tap; or from xonly for tap)
    std::array<uint8_t,33> sec1{};
    if (in.is_tap) {
      if (!sec1_from_xonly_even(in.tap_xonly, sec1)) throw std::runtime_error("xonly->sec1 failed");
    } else {
      sec1 = in.sec1;
    }
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_parse(ctx, &pk, sec1.data(), sec1.size()))
      throw std::runtime_error("parse pubkey failed");
    pubs.push_back(pk);
  }

  if (pubs.empty()) throw std::runtime_error("no inputs");

  secp256k1_pubkey A;
  const secp256k1_pubkey* arr = pubs.data();
  if (!secp256k1_ec_pubkey_combine(ctx, &A, &arr, pubs.size()))
    throw std::runtime_error("pubkey combine failed");

  // A_compressed (33B) for hashing
  unsigned char A33[33]; size_t A33len=33;
  if (!secp256k1_ec_pubkey_serialize(ctx, A33, &A33len, &A, SECP256K1_EC_COMPRESSED))
    throw std::runtime_error("serialize A failed");

  // 2) input_hash = H_tag("din-sp-input", outpoint_L || serP(A))
  std::vector<uint8_t> ih_msg;
  ih_msg.reserve(36 + 33);
  ih_msg.insert(ih_msg.end(), P.outpoint_L_le.begin(), P.outpoint_L_le.end()); // 36 bytes
  ih_msg.insert(ih_msg.end(), A33, A33+33);
  uint8_t input_hash[32];
  tagged_hash("din-sp-input", ih_msg.data(), ih_msg.size(), input_hash);

  // s = input_hash * a (mod n)
  unsigned char s32[32]; std::memcpy(s32, input_hash, 32); scalar_mod_n(s32);
  if (!secp256k1_ec_seckey_verify(ctx, s32)) throw std::runtime_error("bad input_hash scalar");
  if (!secp256k1_ec_seckey_tweak_mul(ctx, s32, a32)) throw std::runtime_error("mul failed"); // s = s * a

  // 3) ecdh = s * B_scan   (multiply receiver scan pubkey by scalar s)
  secp256k1_pubkey Bscan;
  if (!secp256k1_ec_pubkey_parse(ctx, &Bscan, P.receiver.scan_pub.data(), P.receiver.scan_pub.size()))
    throw std::runtime_error("parse scan pub failed");
  if (!secp256k1_ec_pubkey_tweak_mul(ctx, &Bscan, s32))
    throw std::runtime_error("ecdh mul failed");

  unsigned char ecdh33[33]; size_t ecdh33len=33;
  if (!secp256k1_ec_pubkey_serialize(ctx, ecdh33, &ecdh33len, &Bscan, SECP256K1_EC_COMPRESSED))
    throw std::runtime_error("serialize ecdh failed");

  // 4) t_k = H_tag("din-sp-tweak", serP(ecdh) || ser32(k_index))
  uint8_t idx4[4]; put_le32(P.k_index, idx4);
  std::vector<uint8_t> tmsg = concat(std::vector<uint8_t>(ecdh33, ecdh33+33), std::vector<uint8_t>(idx4, idx4+4));
  unsigned char tweak32[32];
  tagged_hash("din-sp-tweak", tmsg.data(), tmsg.size(), tweak32);
  if (!secp256k1_ec_seckey_verify(ctx, tweak32))
    throw std::runtime_error("tweak zero/overflow");

  // 5) P_k = B_spend + t_k * G  → x-only for taproot program
  secp256k1_pubkey Bspend;
  if (!secp256k1_ec_pubkey_parse(ctx, &Bspend, P.receiver.spend_pub.data(), P.receiver.spend_pub.size()))
    throw std::runtime_error("parse spend pub failed");
  if (!secp256k1_ec_pubkey_tweak_add(ctx, &Bspend, tweak32))
    throw std::runtime_error("tweak add failed");

  secp256k1_xonly_pubkey X;
  int pk_parity = 0;
  if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &X, &pk_parity, &Bspend))
    throw std::runtime_error("xonly from pub failed");

  std::array<uint8_t,32> out{};
  if (!secp256k1_xonly_pubkey_serialize(ctx, out.data(), &X))
    throw std::runtime_error("xonly serialize failed");
  return out;
}

} // namespace din::sp
