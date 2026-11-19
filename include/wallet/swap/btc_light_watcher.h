#pragma once
// Watches one Bitcoin HTLC without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, sections 3.2 and 4).
//
// The counterpart of btc_watcher.h: it answers the swap engine's question
// "what is true about this lock right now?" with the same BtcWatchReport, from
// the node's own header chain and from public servers that are not believed.
//
// What it reports, and on what evidence:
//   - The lock exists: a transaction paying the HTLC script the exact swap
//     amount, proven by Merkle proof into the node's best chain. Its
//     confirmations are counted on that chain.
//   - The lock is spent, confirmed: the spending transaction, proven the same
//     way.
//   - The lock is spent, unconfirmed: ONLY if the spending transaction carries
//     the swap's secret (it hashes to the payment hash), which proves itself.
//     Any other unconfirmed "spend" a server reports is ignored: a server can
//     invent one, and the engine would stop refunding or re-sending on it.
//   - Median time: computed from the node's own headers.
//
// The report is "not ok" (the engine then does nothing) when the header chain
// is behind the wall clock, when its median time is not known yet, or when
// fewer servers than required answered. Servers that answer nothing found are
// counted as answering; one honest server is enough to learn of the lock or of
// the secret, and every server is asked.
//
// No I/O of its own: servers are EsploraServer objects (HTTP injected) and the
// header chain is kept in sync by someone else.

#include "wallet/swap/btc_esplora.h"
#include "wallet/swap/btc_header_chain.h"
#include "wallet/swap/btc_watcher.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dinero::swap {

struct BtcLightWatchConfig {
    // The chain's newest header may be this old before the watcher stops
    // answering: older means our view of Bitcoin is stale.
    uint32_t max_tip_age_seconds{2 * 60 * 60};
    // Servers that must answer in a round for the report to count.
    size_t min_servers{1};
    // Candidate transactions examined per round when looking for the lock.
    size_t max_candidates{20};
};

class BtcLightWatcher {
public:
    // The chain and the servers must outlive the watcher. `target` is the same
    // description BtcWatcher takes; its expected_chain must be the chain's
    // network name ("main", "testnet4", "regtest").
    BtcLightWatcher(const BtcHeaderChain& chain, std::vector<EsploraServer*> servers, BtcWatchTarget target,
                    BtcLightWatchConfig config = {});

    // One observation. Cheap after the lock (and its spend) are proven: those
    // are remembered and only re-checked against the header chain.
    BtcWatchReport Observe(uint32_t now_unix);

    // Why the last report was not ok ("" when it was).
    const std::string& last_problem() const { return problem_; }

private:
    struct Proven {
        Bytes32 txid{};
        Bytes32 block_hash{};
        uint32_t vout{0};  // funding: the output; spend: unused
        uint64_t value_sat{0};
    };
    bool FindFunding(size_t& answered);
    void FindSpend(HtlcObservation& htlc, size_t& answered);
    std::optional<std::vector<uint8_t>> RawFromAnyServer(const Bytes32& txid);

    const BtcHeaderChain& chain_;
    std::vector<EsploraServer*> servers_;
    BtcWatchTarget target_;
    BtcLightWatchConfig config_;
    std::vector<uint8_t> script_pubkey_;
    std::optional<Bytes32> expected_funding_txid_;

    std::optional<Proven> funding_;
    std::optional<Proven> confirmed_spend_;
    std::optional<Bytes32> confirmed_spend_preimage_;
    std::set<Bytes32> not_the_lock_;  // proven transactions paying the script that are not this swap's lock
    std::string problem_;
};

}  // namespace dinero::swap
