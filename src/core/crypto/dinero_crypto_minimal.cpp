// dinero_crypto_minimal.cpp
// One-file amalgamation: RNG + sha256 + ripemd160 + hmac_sha512-SHA512 + Bech32 + libsecp256k1 facade.
// Build (example):
//   clang++ -std=c++17 dinero_crypto_minimal.cpp -o crypto_demo -DDINERO_CRYPTO_DEMO \
//           -I/path/to/secp256k1/include -L/path/to/secp256k1/lib -lsecp256k1
//
// In your daemon, just copy the functions under "PUBLIC API (call these)" and remove the demo main().
//

#include "dinero/core/crypto/ripemd160.h"  // Include our new RIPEMD-160 implementation
#include <algorithm>  // For std::reverse
#include <vector>  // For std::vector in HMAC-SHA512
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <cassert>
#include "crypto/evp_secp256k1.h"

// ============================== OS RNG ==============================
#if defined(__APPLE__)
  #include <Security/Security.h>
#elif defined(_WIN32)
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
#else
  #include <sys/random.h>
  #include <unistd.h>
  #include <cstdio>
#endif

static bool crypto_rng(void* out, size_t len) {
#if defined(__APPLE__)
  return SecRandomCopyBytes(kSecRandomDefault, len, (uint8_t*)out) == errSecSuccess;
#elif defined(_WIN32)
  return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  ssize_t r = getrandom(out, len, 0);
  if (r == (ssize_t)len) return true;
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f) return false;
  size_t n = fread(out, 1, len, f);
  fclose(f);
  return n == len;
#endif
}

// ============================== SHA-256 ==============================
void sha256(const uint8_t* data, size_t len, uint8_t out32[32]) {
  struct Ctx { uint32_t h[8]; uint64_t bits; uint8_t buf[64]; size_t idx; } c;
  auto ror = [](uint32_t x, uint32_t n){ return (x>>n)|(x<<(32-n)); };
  auto Ch  = [&](uint32_t x,uint32_t y,uint32_t z){ return (x & y) ^ (~x & z); };
  auto Maj = [&](uint32_t x,uint32_t y,uint32_t z){ return (x & y) ^ (x & z) ^ (y & z); };
  auto S0  = [&](uint32_t x){ return ror(x,2) ^ ror(x,13) ^ ror(x,22); };
  auto S1  = [&](uint32_t x){ return ror(x,6) ^ ror(x,11) ^ ror(x,25); };
  auto s0  = [&](uint32_t x){ return ror(x,7) ^ ror(x,18) ^ (x>>3); };
  auto s1  = [&](uint32_t x){ return ror(x,17) ^ ror(x,19) ^ (x>>10); };
  auto be32= [&](const uint8_t* p){ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | (uint32_t)p[3]; };
  auto be32enc=[&](uint8_t* p, uint32_t v){ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; };

  static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
  };

  auto init=[&](){
    c.h[0]=0x6a09e667u; c.h[1]=0xbb67ae85u; c.h[2]=0x3c6ef372u; c.h[3]=0xa54ff53au;
    c.h[4]=0x510e527fu; c.h[5]=0x9b05688cu; c.h[6]=0x1f83d9abu; c.h[7]=0x5be0cd19u;
    c.bits=0; c.idx=0;
  };
  auto compress=[&](const uint8_t blk[64]){
    uint32_t W[64];
    for(int i=0;i<16;i++) W[i]=be32(blk+4*i);
    for(int i=16;i<64;i++) W[i]=s1(W[i-2])+W[i-7]+s0(W[i-15])+W[i-16];
    uint32_t a=c.h[0],b=c.h[1],cc=c.h[2],d=c.h[3],e=c.h[4],f=c.h[5],g=c.h[6],h=c.h[7];
    for(int i=0;i<64;i++){
      uint32_t T1=h+S1(e)+Ch(e,f,g)+K[i]+W[i];
      uint32_t T2=S0(a)+Maj(a,b,cc);
      h=g; g=f; f=e; e=d+T1; d=cc; cc=b; b=a; a=T1+T2;
    }
    c.h[0]+=a; c.h[1]+=b; c.h[2]+=cc; c.h[3]+=d; c.h[4]+=e; c.h[5]+=f; c.h[6]+=g; c.h[7]+=h;
  };
  auto update=[&](const uint8_t* p, size_t n){
    c.bits += (uint64_t)n*8;
    size_t i=0;
    if(c.idx){
      size_t t = 64 - c.idx; if(t>n) t=n;
      memcpy(c.buf+c.idx, p, t); c.idx+=t; i+=t;
      if(c.idx==64){ compress(c.buf); c.idx=0; }
    }
    for(; i+64<=n; i+=64) compress(p+i);
    size_t rem=n-i; if(rem){ memcpy(c.buf, p+i, rem); c.idx=rem; }
  };
  auto final=[&](uint8_t out[32]){
    uint8_t tmp[8]; be32enc(tmp, (uint32_t)(c.bits>>32)); be32enc(tmp+4, (uint32_t)c.bits);
    update((uint8_t*)"\x80", 1);
    while(c.idx != 56) update((uint8_t*)"\x00", 1);
    update(tmp, 8);
    for(int i=0;i<8;i++) be32enc(out+4*i, c.h[i]);
  };
  init(); update(data, len); final(out32);
}

// ============================== ripemd160 ==============================
void ripemd160(const uint8_t* data, size_t len, uint8_t out20[20]) {
  // Use our new, correct RIPEMD-160 implementation
  auto result = dinero::RIPEMD160(data, len);
  std::copy(result.begin(), result.end(), out20);
}

// ============================== HMAC-SHA512 ==============================
// Canonical HMAC-SHA512 using OpenSSL on ALL platforms.
// Previous non-macOS fallback was broken (used SHA256, not SHA512).
void hmac_sha512(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t out64[64]) {
  unsigned int len = 0;
  unsigned char* r = HMAC(EVP_sha512(), key, static_cast<int>(keylen),
                          data, datalen, out64, &len);
  // Hard fail: crypto primitive must never silently degrade
  if (!r || len != 64) {
    std::abort();
  }
}

// ============================== Bech32 (BIP-173) ==============================
// Minimal encoder for witness v0 P2WPKH (20-byte program).
namespace bech32 {
static const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static uint32_t polymod(const std::vector<uint8_t>& v) {
  uint32_t chk = 1;
  for (auto x : v) {
    uint32_t b = chk >> 25;
    chk = (chk & 0x1ffffff) << 5 ^ x;
    if (b & 1) chk ^= 0x3b6a57b2;
    if (b & 2) chk ^= 0x26508e6d;
    if (b & 4) chk ^= 0x1ea119fa;
    if (b & 8) chk ^= 0x3d4233dd;
    if (b & 16) chk ^= 0x2a1462b3;
  }
  return chk;
}
static std::vector<uint8_t> hrpExpand(const std::string& hrp) {
  std::vector<uint8_t> ret; ret.reserve(hrp.size()*2+1);
  for (unsigned char c: hrp) ret.push_back(c >> 5);
  ret.push_back(0);
  for (unsigned char c: hrp) ret.push_back(c & 31);
  return ret;
}
static std::string encode(const std::string& hrp, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> values = hrpExpand(hrp);
  values.insert(values.end(), data.begin(), data.end());
  values.insert(values.end(), {0,0,0,0,0,0}); // 6 checksum 5-bit words
  uint32_t pm = polymod(values) ^ 1;
  std::string ret = hrp + '1';
  for (auto v : data) ret += CHARSET[v];
  for (int i=0;i<6;i++) ret += CHARSET[(pm >> (5*(5-i))) & 31];
  return ret;
}
// Convert 8-bit bytes to 5-bit groups (no padding), for witness program
static bool convertBits(const uint8_t* in, size_t inlen, int from, int to, bool pad, std::vector<uint8_t>& out) {
  uint32_t acc = 0; int bits = 0; uint32_t maxv = (1u<<to)-1;
  for (size_t i=0;i<inlen;i++) {
    uint8_t value = in[i];
    if (value >> from) return false;
    acc = (acc << from) | value;
    bits += from;
    while (bits >= to) {
      bits -= to;
      out.push_back((acc >> bits) & maxv);
    }
  }
  if (pad) {
    if (bits) out.push_back((acc << (to - bits)) & maxv);
  } else if (bits >= from || ((acc << (to - bits)) & maxv)) {
    return false;
  }
  return true;
}
static std::string encodeWitnessV0(const std::string& hrp, const uint8_t prog[], size_t progLen) {
  // data = [witver=0] + convertBits(prog, 8->5)
  std::vector<uint8_t> data; data.reserve(1 + (progLen*8+4)/5);
  data.push_back(0); // version 0
  if (!convertBits(prog, progLen, 8, 5, true, data)) return std::string();
  return encode(hrp, data);
}
} // namespace bech32

// ============================== libsecp256k1 facade ==============================
#include <secp256k1.h>

bool CF_Init() {
  return dinero::crypto::GetSecp256k1ContextSignVerify() != nullptr;
}
void CF_Shutdown() {
  // Shared secp contexts are process-lifetime singletons owned by crypto/evp_secp256k1.cpp.
}
bool CF_GeneratePrivKey(unsigned char out32[32]) {
  return dinero::crypto::GenerateSecp256k1PrivateKey(out32);
}

bool CF_GenerateRandomBytes(unsigned char* out, size_t len) {
  return crypto_rng(out, len);
}
bool CF_GetCompressedPubkey(const unsigned char seckey[32], unsigned char out33[33]) {
  if (!seckey || !out33) return false;
  auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_create(ctx, &pub, seckey)) return false;
  size_t len=33;
  return secp256k1_ec_pubkey_serialize(ctx, out33, &len, &pub, SECP256K1_EC_COMPRESSED)==1 && len==33;
}
void HASH160(const uint8_t* data, size_t len, uint8_t out20[20]) {
  uint8_t tmp[32]; sha256(data, len, tmp); ripemd160(tmp, sizeof(tmp), out20);
}
void DoubleSHA256(const uint8_t* data, size_t len, uint8_t out32[32]) {
  uint8_t tmp[32]; sha256(data, len, tmp); sha256(tmp, sizeof(tmp), out32);
}
bool CF_SignDER(const unsigned char seckey[32], const unsigned char msg32[32],
                unsigned char* out_der, size_t& out_len, size_t cap) {
  auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  secp256k1_ecdsa_signature sig;
  if (!secp256k1_ecdsa_sign(ctx, &sig, msg32, seckey, secp256k1_nonce_function_rfc6979, nullptr)) return false;
  size_t len=cap;
  if (!secp256k1_ecdsa_signature_serialize_der(ctx, out_der, &len, &sig)) return false;
  out_len = len; return true;
}
bool CF_VerifyDER(const unsigned char pub33[33], const unsigned char msg32[32],
                  const unsigned char* der, size_t der_len) {
  auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_parse(ctx, &pub, pub33, 33)) return false;
  secp256k1_ecdsa_signature sig;
  if (!secp256k1_ecdsa_signature_parse_der(ctx, &sig, der, der_len)) return false;
  secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);
  return secp256k1_ecdsa_verify(ctx, &sig, msg32, &pub) == 1;
}

// ============================== PUBLIC API (call these) ==============================
// Generate Dinero mainnet P2WPKH Bech32 address from a fresh keypair.
// hrp should be "din" (mainnet) or "rdin" (regtest) etc.
bool GenerateBech32Address(const std::string& hrp,
                           std::array<uint8_t,32>& out_seckey,
                           std::array<uint8_t,33>& out_pubkey,
                           std::string& out_address)
{
  if (!CF_Init()) return false;
  if (!CF_GeneratePrivKey(out_seckey.data())) return false;
  if (!CF_GetCompressedPubkey(out_seckey.data(), out_pubkey.data())) return false;
  uint8_t h20[20]; HASH160(out_pubkey.data(), out_pubkey.size(), h20);
  out_address = bech32::encodeWitnessV0(hrp, h20, sizeof(h20)); // witver=0, 20-byte prog
  return !out_address.empty();
}

// WIF (optional): Base58Check of 0x80 || seckey || 0x01 (compressed)
static const char* B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
static void sha256_once(const uint8_t* in, size_t n, uint8_t out32[32]) { sha256(in,n,out32); }
static void double_sha256(const uint8_t* in, size_t n, uint8_t out32[32]) { uint8_t t[32]; sha256_once(in,n,t); sha256_once(t,32,out32); }
static std::string Base58Check(const uint8_t* payload, size_t payload_len) {
  uint8_t chk[32]; double_sha256(payload, payload_len, chk);
  std::vector<uint8_t> buf(payload, payload+payload_len); buf.insert(buf.end(), chk, chk+4);
  // Count leading zeros
  size_t zeros=0; while(zeros<buf.size() && buf[zeros]==0) zeros++;
  // Big integer base58
  std::vector<uint8_t> tmp(buf.begin(), buf.end());
  std::string out;
  size_t start=zeros;
  while(start < tmp.size()) {
    int carry=0;
    for(size_t i=start;i<tmp.size();i++){
      int val = (int)tmp[i] + carry*256;
      tmp[i] = (uint8_t)(val/58);
      carry  = val % 58;
    }
    out.push_back(B58[carry]);
    while(start<tmp.size() && tmp[start]==0) start++;
  }
  for(size_t i=0;i<zeros;i++) out.push_back('1');
  std::reverse(out.begin(), out.end());
  return out;
}
std::string WIF_Compressed(const std::array<uint8_t,32>& seckey, bool mainnet) {
  std::vector<uint8_t> p; p.reserve(1+32+1);
  p.push_back(mainnet ? 0x80 : 0xEF);
  p.insert(p.end(), seckey.begin(), seckey.end());
  p.push_back(0x01); // compressed flag
  return Base58Check(p.data(), p.size());
}

// ============================== DEMO MAIN (optional) ==============================
#ifdef DINERO_CRYPTO_DEMO
int main(){
  if (!CF_Init()) { std::fprintf(stderr,"secp256k1 init failed\n"); return 1; }
  std::array<uint8_t,32> sk{};
  std::array<uint8_t,33> pk{};
  std::string addr;
  if (!GenerateBech32Address("din", sk, pk, addr)) {
    std::fprintf(stderr,"address generation failed\n");
    return 2;
  }
  std::printf("Private key (hex): ");
  for (auto b: sk) std::printf("%02x", b); std::printf("\n");
  std::printf("Pubkey (33B hex): ");
  for (auto b: pk) std::printf("%02x", b); std::printf("\n");
  std::printf("Bech32 P2WPKH: %s\n", addr.c_str());
  std::string wif = WIF_Compressed(sk, /*mainnet=*/true);
  std::printf("WIF (compressed): %s\n", wif.c_str());
  CF_Shutdown();
  return 0;
}
#endif
