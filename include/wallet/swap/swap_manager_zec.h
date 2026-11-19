#pragma once
// Zcash support for the swap manager (docs/design/din-zec-atomic-swaps.md):
// what SwapManager::SetZecSupport needs, over a Zebra node. Compiled into
// dinerod only with DINERO_SWAP_ZEC (it links rust/zcash_swap), so the default
// build has no Zcash code at all.

#include "wallet/swap/swap_manager.h"
#include "wallet/swap/zec_tx.h"
#include "wallet/swap/zec_watcher.h"

#include <functional>
#include <string>

namespace dinero::swap {

struct ZecDaemonConfig {
    DinRpc din;
    ZecRpc zec;
    // sendrawtransaction to Zebra: returns the txid, throws with Zebra's message.
    std::function<std::string(const std::string& raw_hex)> broadcast;
    SwapNetwork network{SwapNetwork::Testnet};
    std::string expected_chain{"test"};  // Zebra's name: "main", or "test" (testnet and regtest)
    std::function<uint32_t()> clock;     // wall clock (default: time())
};

ZecSupport MakeZecSupport(ZecDaemonConfig config);

}  // namespace dinero::swap
