#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace dinero {

/**
 * Structured recipient descriptor — the result of decoding any payment address.
 *
 * Address decoding is deterministic.
 * Transaction construction (blinding, commitments) is randomized.
 * These two concerns are separated by this type.
 */
struct RecipientDescriptor {
    enum class Type {
        Transparent,    // din1.../rdin1.../tdin1... → P2TR/P2WPKH
        SilentPayment,  // Future: BIP352-style
        Unknown
    };

    Type type = Type::Unknown;

    // Decoded script for supported transparent destination types
    std::vector<uint8_t> script_pubkey;

    // Original address string (for logging/display)
    std::string original_address;

    // Network validation
    bool is_mainnet = false;
    bool is_regtest = false;
    bool is_testnet = false;

    bool isValid() const { return type != Type::Unknown && !script_pubkey.empty(); }
    bool isTransparent() const { return type == Type::Transparent; }
};

/**
 * Decode any Dinero payment address into a RecipientDescriptor.
 *
 * Deterministic: same address always produces the same descriptor.
 *
 * Supported formats:
 *   din1...  / rdin1...  / tdin1...   → Transparent (Taproot P2TR)
 *
 * @param address The address string to decode
 * @return RecipientDescriptor (check isValid())
 */
RecipientDescriptor DecodePaymentTarget(const std::string& address);

} // namespace dinero
