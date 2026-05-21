#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>

// ============================== PUBLIC API ==============================

// Initialize the crypto system (call once at startup)
bool CF_Init();

// Cleanup (call at shutdown)
void CF_Shutdown();

// Generate a new private key
bool CF_GeneratePrivKey(unsigned char out32[32]);

// Generate random bytes (for salts, nonces, etc.)
bool CF_GenerateRandomBytes(unsigned char* out, size_t len);

// Derive compressed public key from private key
bool CF_GetCompressedPubkey(const unsigned char seckey[32], unsigned char out33[33]);

// Generate Bech32 address from keypair
bool GenerateBech32Address(const std::string& hrp,
                           std::array<uint8_t,32>& out_seckey,
                           std::array<uint8_t,33>& out_pubkey,
                           std::string& out_address);

// Hash functions
void sha256(const uint8_t* data, size_t len, uint8_t out32[32]);
void ripemd160(const uint8_t* data, size_t len, uint8_t out20[20]);
void HASH160(const uint8_t* data, size_t len, uint8_t out20[20]);
void DoubleSHA256(const uint8_t* data, size_t len, uint8_t out32[32]);

// hmac_sha512 functions
void hmac_sha512(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t out64[64]);

// ECDSA signing and verification
bool CF_SignDER(const unsigned char seckey[32], const unsigned char msg32[32],
                unsigned char* out_der, size_t& out_len, size_t cap);

bool CF_VerifyDER(const unsigned char pub33[33], const unsigned char msg32[32],
                  const unsigned char* der, size_t der_len);

// WIF export (optional)
std::string WIF_Compressed(const std::array<uint8_t,32>& seckey, bool mainnet=true);
