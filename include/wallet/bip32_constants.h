#pragma once
#include <cstdint>
#include "consensus/coin_type.h"

namespace dinero {
namespace BIP32 {

// Dinero BIP32 Extended Key Version Bytes
// These are unique to Dinero and will render as dpub/dprv instead of xpub/xprv
namespace ExtendedKeyVersions {
    // Mainnet
    const uint32_t EXT_PUBLIC_KEY  = 0x03D1F1A1;  // dpub prefix
    const uint32_t EXT_SECRET_KEY  = 0x03D1F1A2;  // dprv prefix

    // Testnet
    const uint32_t EXT_PUBLIC_KEY_TESTNET  = 0x03D1F1B1;  // tpub prefix
    const uint32_t EXT_SECRET_KEY_TESTNET  = 0x03D1F1B2;  // tprv prefix
}

// Dinero BIP84 Coin Type (official SLIP-44 registration)
// Derivation path: m/84'/1448'/account'/change/address_index
const uint32_t COIN_TYPE = dinero::consensus::DINERO_COIN_TYPE;

// Wallet magic bytes for header separation
// This ensures Dinero wallets are unique and incompatible with other cryptocurrencies
const uint32_t WALLET_MAGIC = 0x44494E52;  // "DINR" in ASCII

// Economics, block limits, coinbase maturity, etc. live in consensus headers:
//   include/consensus/subsidy.h        — INITIAL_SUBSIDY, HALVING_INTERVAL, UNA_PER_DIN
//   include/consensus/tx_validation.h  — MAX_MONEY, COINBASE_MATURITY
//   include/consensus/limits.h         — MAX_BLOCK_SIZE, MAX_TX_SIZE

} // namespace BIP32
} // namespace dinero
