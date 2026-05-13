#include "crypto_init.hpp"
#include "crypto/evp_secp256k1.h"
#include <stdexcept>
#include <mutex>
#include <array>
#include <cstring>
#include <algorithm>

#if defined(__APPLE__)
  #include <Security/Security.h>
#elif defined(__linux__)
  #include <sys/random.h>
  #include <errno.h>
#endif

// ---- secp256k1-zkp ----
// NOTE: This is NOT vanilla libsecp256k1 from bitcoin-core/secp256k1
// This IS secp256k1-zkp from ElementsProject with ZK extensions:
//   - Pedersen commitments
//   - Range proofs (Bulletproofs)
//   - Generators for confidential transactions
#include <secp256k1.h>
#include <secp256k1_generator.h>

namespace {
std::once_flag g_once;

void os_secure_random(uint8_t* out, std::size_t len) {
#if defined(__APPLE__)
  if (SecRandomCopyBytes(kSecRandomDefault, len, out) != errSecSuccess)
    throw std::runtime_error("SecRandomCopyBytes failed");
#elif defined(__linux__)
  ssize_t n = 0;
  while (n < (ssize_t)len) {
    ssize_t r = getrandom(out + n, len - n, 0);
    if (r < 0) { if (errno == EINTR) continue; throw std::runtime_error("getrandom failed"); }
    n += r;
  }
#else
  #error "No CSPRNG available — Dinero requires macOS (SecRandomCopyBytes) or Linux (getrandom)"
#endif
}

void init_secp() {
  if (!dinero::crypto::GetSecp256k1ContextSignVerify()) {
    throw std::runtime_error("secp256k1 context unavailable");
  }
}

bool self_test_impl() noexcept {
  auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  // Minimal deterministic sign/verify self-test
  // priv = 32 bytes of 0x01 (valid key); msg = 32 bytes 0x02
  std::array<uint8_t, 32> priv{}; std::fill(priv.begin(), priv.end(), 0x01);
  std::array<uint8_t, 32> msg{};  std::fill(msg.begin(),  msg.end(),  0x02);

  secp256k1_pubkey pk{};
  secp256k1_ecdsa_signature sig{};

  if (secp256k1_ec_pubkey_create(ctx, &pk, priv.data()) != 1) return false;
  if (secp256k1_ecdsa_sign(ctx, &sig, msg.data(), priv.data(), nullptr, nullptr) != 1) return false;
  if (secp256k1_ecdsa_verify(ctx, &sig, msg.data(), &pk) != 1) return false;
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ZKP CAPABILITY ASSERTION
// ═══════════════════════════════════════════════════════════════════════════════
// Verifies that we're linked against secp256k1-zkp (not vanilla secp256k1)
// by testing ZKP-specific functionality: generator creation.
//
// This catches linking errors at startup rather than at first CT use,
// making deployment issues immediately visible.
// ═══════════════════════════════════════════════════════════════════════════════
bool zkp_capability_check() noexcept {
  auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
  // Test 1: Verify secp256k1_generator_h constant is accessible
  // This is a ZKP-only symbol that doesn't exist in vanilla secp256k1
  if (secp256k1_generator_h == nullptr) {
    return false;  // Should never happen if properly linked
  }

  // Test 2: Generate a blinded generator (ZKP-specific operation)
  // Uses deterministic seed for reproducibility
  std::array<uint8_t, 32> seed{};
  std::fill(seed.begin(), seed.end(), 0x42);

  secp256k1_generator gen{};
  if (secp256k1_generator_generate_blinded(ctx, &gen, seed.data(), seed.data()) != 1) {
    return false;
  }

  // Test 3: Serialize and verify the generator is valid
  std::array<uint8_t, 33> serialized{};
  if (secp256k1_generator_serialize(ctx, serialized.data(), &gen) != 1) {
    return false;
  }

  // Verify first byte is valid generator prefix (0x0a or 0x0b)
  if (serialized[0] != 0x0a && serialized[0] != 0x0b) {
    return false;
  }

  return true;
}

} // namespace

namespace CryptoInit {

struct SecpCtx { secp256k1_context* p; };
static SecpCtx g_wrap;

void init() {
  std::call_once(g_once, []{
    init_secp();
    g_wrap = SecpCtx{dinero::crypto::GetSecp256k1ContextSignVerify()};

    // Basic ECDSA self-test
    if (!self_test_impl())
      throw std::runtime_error("Crypto self-test failed (secp256k1)");

    // ZKP capability assertion - ensures we have secp256k1-zkp, not vanilla
    if (!zkp_capability_check())
      throw std::runtime_error(
        "ZKP capability check failed: secp256k1-zkp required for confidential transactions. "
        "Ensure you are linked against ElementsProject/secp256k1-zkp, not bitcoin-core/secp256k1.");
  });
}

bool self_test() noexcept {
  try { return self_test_impl(); } catch (...) { return false; }
}

void secure_random(uint8_t* out, std::size_t len) {
  os_secure_random(out, len);
}

const SecpCtx* secp() { return &g_wrap; }

void cleanup() {
  // Shared secp contexts are process-lifetime singletons owned by crypto/evp_secp256k1.cpp.
}

} // namespace CryptoInit
