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

// Coinbase maturity (blocks before coinbase outputs can be spent)
const uint32_t COINBASE_MATURITY = 100;

// Economics constants
namespace Economics {
    const uint64_t UNA_PER_DIN = 100000000ULL;  // 1 DIN = 100,000,000 una (8 decimals like Bitcoin)
    const uint64_t MAX_MONEY = 97850000ULL * UNA_PER_DIN;  // 97.85M DIN max supply
    const uint64_t INITIAL_BLOCK_REWARD = 100 * UNA_PER_DIN;  // 100 DIN Phase 1 reward
    const uint32_t HALVING_INTERVAL = 800000;  // 800k blocks (~15.2 years)
    const uint64_t MIN_RELAY_FEE = 1000;  // 1000 una minimum relay fee
    const uint64_t DUST_THRESHOLD = 546;  // 546 una dust threshold
}

// Network/economics constants removed — use consensus headers:
//   include/consensus/limits.h         — MAX_BLOCK_SIZE, MAX_TX_SIZE
//   include/consensus/subsidy.h        — INITIAL_SUBSIDY, HALVING_INTERVAL
//   include/consensus/tx_validation.h  — MAX_MONEY, COINBASE_MATURITY

} // namespace BIP32
} // namespace dinero
