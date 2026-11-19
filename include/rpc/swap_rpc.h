#pragma once
#include <string>

struct ExecutionContext;

namespace dinero {
// The DIN payout address a swap offer/accept uses: a new wallet address (on a
// native wallet enrolled on a wallet-index-v3 node, the next sealed pooled
// address). Throws with the wallet's own refusal text when none is available.
std::string SwapWalletPayoutAddress(const ExecutionContext& ctx);
}  // namespace dinero
