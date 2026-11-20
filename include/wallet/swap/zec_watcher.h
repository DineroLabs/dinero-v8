#pragma once
// Zcash watcher for DIN <-> ZEC swaps (milestone 4 of
// docs/design/din-zec-atomic-swaps.md): a Zebra node's view of one ZEC HTLC,
// of a per-swap deposit address, and of upcoming network upgrades.
//
// Talks to the user's own Zebra over JSON-RPC (Zebra has no wallet; only
// chain calls are used): getblockchaininfo, getblockhash, getblock
// (verbosity 2), getblockheader, getrawmempool, getrawtransaction and
// getaddressutxos.
//
// Differences from the Bitcoin watcher:
// - Zebra reports no median time past, so it is computed here from the last
//   11 block times.
// - Zebra has no gettxspendingprevout, so an unconfirmed spend is found by
//   scanning the mempool (each transaction fetched once).
// - The secret is read from the claim's scriptSig, not a witness.

#include "wallet/swap/engine.h"
#include "wallet/swap/zec_tx.h"

#include <json/json.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dinero::swap {

// Calls a Zebra RPC; returns std::nullopt on any error.
using ZecRpc = std::function<std::optional<Json::Value>(const std::string& method, const Json::Value& params)>;

struct ZecUpgrade {
    std::string name;
    uint32_t activation_height{};
    uint32_t branch_id{};
};

struct ZecChainInfo {
    uint32_t tip_height{};
    uint32_t tip_time_unix{};          // the tip block's own timestamp (what consensus checks lock times against)
    uint32_t mtp_unix{};               // median of the last 11 block times (what relay checks)
    uint32_t next_block_branch_id{};   // sign with this
    std::vector<ZecUpgrade> upgrades;  // every upgrade the node knows, active or pending
};

// Reads the chain state: tip, its time and median time past, the branch id to
// sign with and the known upgrades. `time_cache` (block hash -> time) saves
// header calls across ticks. Zebra names regtest "test", like testnet.
std::optional<ZecChainInfo> ObserveZecChain(const ZecRpc& rpc, const std::string& expected_chain,
                                            std::map<std::string, uint32_t>* time_cache = nullptr);

// Blocks per hour assumed when projecting a deadline to a height: Zcash's 75 s
// target, made 10% faster so more blocks (and so more upgrades) fall inside.
constexpr double kZecProjectedSecondsPerBlock = 75.0 / 1.10;

// The first upgrade that activates after the tip and at or before the height
// projected for `until_unix`. A pre-signed ZEC transaction binds the branch id,
// so a swap whose window reaches such an upgrade must be refused.
std::optional<ZecUpgrade> ZecUpgradeBefore(const ZecChainInfo& chain, uint32_t now_unix, uint32_t until_unix);

struct ZecWatchTarget {
    ZecHtlcTerms terms;
    uint32_t scan_from_height{0};      // first block that could contain the funding
    uint64_t expected_amount_zat{0};   // the swap amount: other outputs to the script are decoys (0 = any)
    std::string expected_funding_txid; // Bob's own funding tx (display hex), once known ("" = any)
    std::string expected_chain;        // Zebra "chain" name ("main" or "test"); another network is not trusted
};

struct ZecWatchReport {
    bool ok{false};  // false: node unreachable or inconsistent; do not act
    HtlcObservation htlc;
    ZecChainInfo chain;
    std::optional<ZecFunding> funding;  // the HTLC output, once seen
};

class ZecWatcher {
public:
    ZecWatcher(ZecRpc rpc, ZecWatchTarget target);

    // One observation. After the first call only new blocks are scanned (the
    // scanned range is re-checked for reorgs) and only new mempool entries
    // are fetched.
    ZecWatchReport Observe();

private:
    std::optional<std::string> HashAt(uint32_t height);
    bool FindMempoolSpend(HtlcObservation& htlc);

    ZecRpc rpc_;
    ZecWatchTarget target_;
    std::string spk_hex_;
    uint32_t next_height_;
    std::map<uint32_t, std::string> recent_;  // hashes of the last scanned blocks (the reorg anchor)
    std::map<std::string, uint32_t> block_time_;  // block hash -> time, for the median
    std::optional<ZecFunding> funding_;
    uint32_t funding_height_{0};
    std::string funding_block_hash_;
    std::optional<HtlcObservation> confirmed_spend_;
    uint32_t spend_height_{0};
    std::string spend_block_hash_;
    std::set<std::string> mempool_seen_;  // mempool txids already checked for a spend
};

// --- Deposit -------------------------------------------------------------------

struct ZecDepositCoin {
    ZecCoin coin;
    uint32_t confirmations{};
};

struct ZecDepositReport {
    bool ok{false};
    uint32_t tip_height{};
    std::vector<ZecDepositCoin> coins;  // confirmed coins at the address (getaddressutxos omits the mempool)
    uint64_t confirmed_zat(uint32_t min_confirmations) const;
    std::vector<ZecCoin> spendable(uint32_t min_confirmations) const;
};

constexpr uint32_t kZecDepositConfirmations = 10;

// The coins paid to `address` (a t1/tm string), with their depth.
ZecDepositReport ObserveZecDeposit(const ZecRpc& rpc, const std::string& address, const std::string& expected_chain);

}  // namespace dinero::swap
