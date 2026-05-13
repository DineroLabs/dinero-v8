#pragma once
#include <cstdint>
#include "consensus/coin_type.h"

namespace dinero {

// Official SLIP-44 coin type for Dinero
constexpr uint32_t kSlip44CoinType = dinero::consensus::DINERO_COIN_TYPE;

inline constexpr uint32_t CoinType() { return kSlip44CoinType; }

} // namespace dinero

