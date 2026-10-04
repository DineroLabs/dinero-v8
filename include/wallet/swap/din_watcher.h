#pragma once
// Dinero watcher for the swap engine (milestone 4c of
// docs/design/din-btc-atomic-swaps-v1-plan.md): turns a Dinero node's view of
// one DIN HTLC into the engine's HtlcObservation.
//
// Funding: the node's address index (getaddresshistory) lists the transaction
// paying the HTLC address; its block is fetched raw (getblock <hash> 0) and
// parsed with Dinero's own transaction deserializer.
// Spend: the address index does NOT record a script-path spend from the
// address, so when gettxout reports the funding output spent, blocks from the
// funding height are scanned (each block once; a reorg below the scanned tip
// restarts the scan) for the spending transaction; the spend's witness
// tells claim (4 items, claim leaf) from refund (3 items, refund leaf). The
// spend's block hash is cached and re-checked every tick, and the history is
// re-read every tick, so a reorg that drops either transaction is noticed.
//
// Confirmed spends only: the node's address index does not attribute a mempool
// spend of a confirmed output to the address. That is enough for the engine —
// Bob learns the secret from Bitcoin (whose watcher does see the mempool), and
// DIN claim/refund re-broadcasts are idempotent while confirmation is pending.

#include "wallet/swap/engine.h"

#include <json/json.h>

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace dinero::swap {

// Calls a Dinero node RPC and returns the "result" member; std::nullopt on error.
using DinRpc = std::function<std::optional<Json::Value>(const std::string& method,
                                                        const Json::Value& params)>;

// bech32m P2TR address of the HTLC output ("din"/"tdin"/"rdin" HRP).
std::string DinHtlcAddress(const DinHtlcOutput& htlc, const std::string& hrp);

struct DinWatchReport {
    bool ok{false};  // false: node unreachable or inconsistent; do not act
    HtlcObservation htlc;
    uint32_t mtp_unix{0};
    std::optional<FundingOutput> funding;
};

class DinWatcher {
public:
    DinWatcher(DinRpc rpc, DinHtlcTerms terms, std::string hrp);
    DinWatchReport Observe();

private:
    DinRpc rpc_;
    DinHtlcTerms terms_;
    DinHtlcOutput htlc_;
    std::string address_;
    std::map<std::string, std::vector<uint8_t>> block_cache_;  // block hash -> raw block
    struct FoundSpend {
        uint32_t height{};
        std::string block_hash;
        bool by_claim{};
        std::optional<Bytes32> preimage;
    };
    std::optional<FoundSpend> spend_;
    // Incremental spend scan: blocks up to scanned_height_ (whose hash was
    // scanned_hash_) were searched and hold NO spend of scan_funding_; the
    // spend's own block is never marked, so it is re-found after a reorg.
    // Public nodes rate-limit RPC, so each block is fetched once, not per tick.
    std::string scan_funding_;
    uint32_t scanned_height_{0};
    std::string scanned_hash_;
};

}  // namespace dinero::swap
