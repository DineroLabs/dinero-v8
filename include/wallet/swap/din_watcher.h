#pragma once
// Dinero watcher for the swap engine (milestone 4c of
// docs/design/din-btc-atomic-swaps-v1-plan.md): turns a Dinero node's view of
// one DIN HTLC into the engine's HtlcObservation.
//
// It scans blocks from the swap's start height, each block once: the funding
// is the output paying the HTLC script with exactly the swap amount (and, once
// known, from the lock's own transaction), so payments to the same address
// cannot stand in for it; the spend is the transaction that spends that
// output, classified by its (committed) leaf script. No address index is
// used. A reorg anywhere in the scanned range rescans from the start.
//
// Confirmed spends only (mempool spends of DIN are not visible here). Bob
// learns the secret from Bitcoin, whose watcher sees the mempool.

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

struct DinWatchTarget {
    DinHtlcTerms terms;
    uint32_t scan_from_height{0};      // first block that could hold the funding
    uint64_t expected_amount_una{0};   // the swap amount: other payments to the script are decoys (0 = any)
    std::string expected_funding_txid; // the lock's own transaction once known ("" = any)
};

struct DinWatchReport {
    bool ok{false};  // false: node unreachable or inconsistent; do not act
    HtlcObservation htlc;
    uint32_t mtp_unix{0};
    std::optional<FundingOutput> funding;
    std::optional<FundingOutput> claim_output;  // output 0 of the mined claim (Bob's sweep parent)
};

class DinWatcher {
public:
    DinWatcher(DinRpc rpc, DinWatchTarget target);
    DinWatcher(DinRpc rpc, DinHtlcTerms terms, std::string hrp);  // any amount, scan from genesis
    DinWatchReport Observe();

private:
    DinRpc rpc_;
    DinWatchTarget target_;
    DinHtlcOutput htlc_;
    struct FoundSpend {
        uint32_t height{};
        bool by_claim{};
        std::optional<Bytes32> preimage;
        FundingOutput output;  // the spending transaction's output 0
    };
    std::optional<FundingOutput> funding_;
    uint32_t funding_height_{0};
    std::optional<FoundSpend> spend_;
    uint32_t next_height_{0};
    std::map<uint32_t, std::string> recent_;  // hashes of the last scanned blocks (the reorg anchor)
};

}  // namespace dinero::swap
