#pragma once
// Bitcoin watcher for the swap engine (milestone 4b of
// docs/design/din-btc-atomic-swaps-v1-plan.md): turns a Bitcoin Core node's
// view of one BTC HTLC into the engine's HtlcObservation.
//
// Talks to the user's own Bitcoin Core over JSON-RPC (no Dinero server in the
// trust path). Uses only non-wallet calls: getblockchaininfo, getblockhash,
// getblock (verbosity 2), gettxspendingprevout and getrawtransaction.
//
// - Funding: blocks from `scan_from_height` are scanned for an output paying
//   the HTLC's P2WSH script; the block hash is remembered so a reorg that drops
//   the funding block is detected and the output is searched for again.
// - Spends: a confirmed spend is found in a block (depth computed); an
//   unconfirmed one through gettxspendingprevout, so Bob sees Alice's secret
//   the moment her claim reaches the mempool.
// - Claim vs refund is read from the spend's witness; the secret is extracted
//   only if it hashes to the payment hash.

#include "wallet/swap/btc_tx.h"
#include "wallet/swap/engine.h"

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>

namespace dinero::swap {

// Calls a Bitcoin Core RPC; returns std::nullopt on any error.
using BtcRpc = std::function<std::optional<Json::Value>(const std::string& method,
                                                        const Json::Value& params)>;

struct BtcWatchTarget {
    BtcHtlcTerms terms;
    uint32_t scan_from_height{0};  // first block that could contain the funding
};

struct BtcWatchReport {
    bool ok{false};                // false: node unreachable or inconsistent; do not act
    HtlcObservation htlc;
    uint32_t mtp_unix{0};
    std::optional<BtcFunding> funding;  // the HTLC output, once seen
};

class BtcWatcher {
public:
    BtcWatcher(BtcRpc rpc, BtcWatchTarget target);

    // One observation of the HTLC. Cheap to call every tick: after the first
    // call it only scans blocks it has not seen (and re-checks the funding
    // block for reorgs).
    BtcWatchReport Observe();

private:
    BtcRpc rpc_;
    BtcWatchTarget target_;
    std::vector<uint8_t> script_pubkey_;
    uint32_t next_height_;
    std::optional<BtcFunding> funding_;
    uint32_t funding_height_{0};
    std::string funding_block_hash_;
    std::optional<HtlcObservation> confirmed_spend_;  // spend fields only
    uint32_t spend_height_{0};
    std::string spend_block_hash_;
};

}  // namespace dinero::swap
