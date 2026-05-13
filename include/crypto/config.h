#pragma once

// Crypto configuration and guards
#if !defined(HAVE_SECP256K1)
#error "secp256k1 not found—install it; stubs are forbidden for crypto."
#endif

// Dinero network parameters
namespace dinero {
constexpr const char* HRP = "din";
constexpr uint16_t RPC_PORT = 20998;
constexpr uint16_t P2P_PORT = 20999;
} // namespace dinero
