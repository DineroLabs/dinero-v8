// secp256k1_utils.h - ECDSA signing and verification for Lightning Network
// Provides: Signature creation, verification, and recovery for BOLT 11 invoices

#ifndef DINERO_LIGHTNING_SECP256K1_UTILS_H
#define DINERO_LIGHTNING_SECP256K1_UTILS_H

#include <array>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>

// Forward declare secp256k1 types to avoid exposing them in the header
typedef struct secp256k1_context_struct secp256k1_context;

namespace dinero {
namespace lightning {
namespace secp256k1_utils {

// Initialize secp256k1 library (call once at startup)
bool init_secp256k1();

// Cleanup secp256k1 library (call at shutdown)
void cleanup_secp256k1();

// Check if secp256k1 is initialized
bool is_secp256k1_initialized();

// Generate a random private key (32 bytes)
// Returns: 32-byte private key valid for secp256k1
std::array<uint8_t, 32> generate_private_key();

// Derive public key from private key
// Input: 32-byte private key
// Returns: 33-byte compressed public key
std::array<uint8_t, 33> get_public_key(const std::array<uint8_t, 32>& private_key);

// Sign a 32-byte message hash with private key (ECDSA with recovery)
// Input: 32-byte message hash, 32-byte private key
// Returns: 65-byte recoverable signature (recovery_id || r || s)
std::array<uint8_t, 65> sign_recoverable(
    const std::array<uint8_t, 32>& message_hash,
    const std::array<uint8_t, 32>& private_key
);

// Verify ECDSA signature
// Input: 32-byte message hash, 64-byte signature (r || s), 33-byte public key
// Returns: true if signature is valid
bool verify_signature(
    const std::array<uint8_t, 32>& message_hash,
    const std::array<uint8_t, 64>& signature,
    const std::array<uint8_t, 33>& public_key
);

// Recover public key from recoverable signature
// Input: 32-byte message hash, 65-byte recoverable signature
// Returns: 33-byte compressed public key
std::array<uint8_t, 33> recover_public_key(
    const std::array<uint8_t, 32>& message_hash,
    const std::array<uint8_t, 65>& recoverable_signature
);

// Verify recoverable signature and recover public key
// Input: 32-byte message hash, 65-byte recoverable signature
// Returns: pair<is_valid, public_key>
std::pair<bool, std::array<uint8_t, 33>> verify_and_recover(
    const std::array<uint8_t, 32>& message_hash,
    const std::array<uint8_t, 65>& recoverable_signature
);

// Convert 65-byte recoverable signature to 64-byte compact signature
// Input: 65-byte recoverable signature (recovery_id || r || s)
// Returns: 64-byte compact signature (r || s)
std::array<uint8_t, 64> recoverable_to_compact(
    const std::array<uint8_t, 65>& recoverable_signature
);

// Lightning-specific: Sign invoice data
// Input: invoice data bytes, node private key
// Returns: 65-byte recoverable signature for BOLT 11
std::array<uint8_t, 65> sign_invoice(
    const std::vector<uint8_t>& invoice_data,
    const std::array<uint8_t, 32>& node_private_key
);

// Lightning-specific: Verify invoice signature
// Input: invoice data bytes, 65-byte signature, 33-byte node public key
// Returns: true if invoice signature is valid
bool verify_invoice_signature(
    const std::vector<uint8_t>& invoice_data,
    const std::array<uint8_t, 65>& signature,
    const std::array<uint8_t, 33>& node_public_key
);

// Validate private key (check if it's in valid range for secp256k1)
bool validate_private_key(const std::array<uint8_t, 32>& private_key);

// Validate public key (check if it's a valid point on the curve)
bool validate_public_key(const std::array<uint8_t, 33>& public_key);

} // namespace secp256k1_utils
} // namespace lightning
} // namespace dinero

#endif // DINERO_LIGHTNING_SECP256K1_UTILS_H
