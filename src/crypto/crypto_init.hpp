#pragma once
#include <cstddef>
#include <cstdint>

namespace CryptoInit {

// Call once at process start. Throws std::runtime_error on failure.
void init();

// Optional: runs a cheap self-test; returns true on success.
bool self_test() noexcept;

// Secure RNG (OS-backed). Throws on failure.
void secure_random(uint8_t* out, std::size_t len);

// Accessors (if you need the global secp ctx)
struct SecpCtx;
const SecpCtx* secp();  // non-owning

// Graceful shutdown (optional)
void cleanup();

} // namespace CryptoInit
