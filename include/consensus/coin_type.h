#pragma once
#include <cstdint>

namespace dinero {
namespace consensus {

/**
 * Dinero Coin Type Definition (SLIP-44)
 * 
 * SLIP-44 defines the coin type used in BIP44/BIP84 derivation paths:
 * m/purpose'/coin_type'/account'/change/address_index
 * 
 * IMPORTANT: This coin type must be consistent across all Dinero implementations
 * (daemon, GUI, mobile wallets, hardware wallets, etc.)
 * 
 * Current Status: TEMPORARY - Pending official SLIP-44 registration
 * Official Registration: https://github.com/unalabs/slips/blob/master/slip-0044.md
 */

// Dinero coin type — 1448 is the only coin type. v7 chain is fresh; no legacy scan path.
constexpr uint32_t DINERO_COIN_TYPE = 1448;

// Purpose codes — one per cryptographic scheme.
constexpr uint32_t PURPOSE_TAPROOT  = 86;   ///< BIP-86: secp256k1 Schnorr (P2TR)
constexpr uint32_t PURPOSE_P2MR     = 88;   ///< Dinero v7: ML-DSA-65 post-quantum (P2MR)
constexpr uint32_t PURPOSE_SHIELDED = 77;   ///< Dinero shielded ZK nullifier model

/**
 * BIP84 purpose constant (Native SegWit / P2WPKH).
 * v7 wallets do not derive new addresses under this purpose; kept only because
 * Lightning protocol keys are conventionally derived under m/84'/coin'/9735'/...
 */
constexpr uint32_t BIP84_PURPOSE = 84;
constexpr uint32_t DEFAULT_ACCOUNT = 0;
constexpr uint32_t EXTERNAL_CHAIN = 0;  // Receive addresses
constexpr uint32_t INTERNAL_CHAIN = 1;  // Change addresses

/**
 * Hardened derivation marker
 */
constexpr uint32_t HARDENED = 0x80000000u;

/**
 * Get the full BIP84 derivation path as a string.
 * v7 uses BIP84 only for Lightning protocol-key derivation (m/84'/1448'/9735'/...),
 * not for new on-chain receive addresses (those use Taproot or P2MR).
 */
inline const char* getBip84DerivationPath() {
    static const char* path = "m/84'/1448'/0'";
    return path;
}

/**
 * Get the coin type for the current network
 * This function ensures consistent coin type usage across the entire codebase
 */
inline uint32_t getCoinType() {
    return DINERO_COIN_TYPE;
}

/**
 * Validation: only the canonical v7 coin type is accepted.
 */
inline bool isValidCoinType(uint32_t coin_type) {
    return coin_type == DINERO_COIN_TYPE;
}

} // namespace consensus
} // namespace dinero

/**
 * SLIP-44 Registration Checklist:
 * 
 * 1. ✅ Define consistent coin type across codebase
 * 2. ⏳ Create project website (dinero.org)
 * 3. ⏳ Submit SLIP-44 pull request to https://github.com/unalabs/slips
 * 4. ⏳ Submit SLIP-173 pull request (Bech32 HRP 'din')
 * 5. ⏳ Wait for UnaLabs review and assignment
 * 6. ⏳ Update DINERO_COIN_TYPE_OFFICIAL with assigned number
 * 7. ⏳ Switch DINERO_COIN_TYPE to use official number
 * 8. ⏳ Update all documentation and examples
 * 9. ⏳ Coordinate with hardware wallet manufacturers
 * 
 * Registration Requirements:
 * - Unique coin name: "Dinero"
 * - Symbol: "DIN"
 * - Website: https://dinero-coin.com (or GitHub)
 * - Source code: https://github.com/dinero-project/dinero
 * - Derivation path: m/86'/1448'/0' (Taproot) + m/88'/1448'/0' (P2MR, v7)
 * - Bech32 HRP: 'din' (via SLIP-173)
 *
 * Coin type: 1448 only. v7 chain is fresh; no legacy scan path.
 */
