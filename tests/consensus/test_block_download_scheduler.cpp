/**
 * Phase N.4: BlockDownloadScheduler regression tests
 *
 * Covers the CSN deadlock class where external backpressure saturates the
 * scheduler window and previously prevented any new requests from being issued.
 */

#include "consensus/active_chain_ancestry.h"
#include "consensus/block_download_scheduler.h"
#include "consensus/drain_failure_streak.h"
#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using dinero::BlockHeader;
using dinero::Block;
using dinero::uint256;
namespace dcs = dinero::consensus;

namespace {

BlockHeader CreateTestHeader(
    const uint256& prev_hash,
    uint32_t time,
    uint32_t bits = 0x1d00ffff
) {
    BlockHeader header;
    header.version = 1;
    header.prev_block_hash = prev_hash;
    header.merkle_root = uint256();
    header.timestamp = time;
    header.difficulty = bits;
    header.nonce = 1;
    header.utreexo_root = uint256();
    return header;
}

void BuildLinearHeaders(
    dcs::HeaderChainSelector& selector,
    uint32_t count,
    std::vector<uint256>* hashes = nullptr,
    uint32_t base_time = 1'000'000
) {
    uint256 null_hash;
    null_hash.SetNull();

    BlockHeader genesis = CreateTestHeader(null_hash, base_time);
    if (!selector.AddHeader(genesis)) {
        throw std::runtime_error("failed to add genesis header");
    }

    if (hashes) {
        hashes->clear();
        hashes->push_back(genesis.GetHash());
    }

    uint256 prev = genesis.GetHash();
    for (uint32_t i = 1; i <= count; ++i) {
        BlockHeader next = CreateTestHeader(prev, base_time + i);
        if (!selector.AddHeader(next)) {
            throw std::runtime_error("failed to add linear header at height " + std::to_string(i));
        }
        prev = next.GetHash();
        if (hashes) {
            hashes->push_back(prev);
        }
    }
}

void AppendForkHeaders(
    dcs::HeaderChainSelector& selector,
    const uint256& fork_parent,
    uint32_t count,
    std::vector<uint256>* hashes,
    uint32_t base_time
) {
    if (hashes) {
        hashes->clear();
    }

    uint256 prev = fork_parent;
    for (uint32_t i = 0; i < count; ++i) {
        BlockHeader next = CreateTestHeader(prev, base_time + i);
        if (!selector.AddHeader(next)) {
            throw std::runtime_error("failed to add fork header at index " + std::to_string(i));
        }
        prev = next.GetHash();
        if (hashes) {
            hashes->push_back(prev);
        }
    }
}

Block MakeBlockForHash(dcs::HeaderChainSelector& selector, const uint256& hash) {
    const auto entry = selector.GetHeaderValue(hash);
    if (!entry) {
        throw std::runtime_error("missing header for block hash " + hash.GetHex());
    }

    Block block;
    block.header = entry->header;
    return block;
}

std::vector<std::unique_ptr<dinero::CBlockIndex>> BuildActiveChainIndex(
    const std::vector<uint256>& hashes
) {
    std::vector<std::unique_ptr<dinero::CBlockIndex>> entries;
    entries.reserve(hashes.size());

    for (size_t i = 0; i < hashes.size(); ++i) {
        auto entry = std::make_unique<dinero::CBlockIndex>();
        entry->hash = hashes[i];
        entry->height = static_cast<uint32_t>(i);
        if (i > 0) {
            entry->pprev = entries.back().get();
            entry->prev_hash = hashes[i - 1];
        }
        entries.push_back(std::move(entry));
    }

    return entries;
}

bool Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "   ❌ " << message << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // Synthetic headers carry fake PoW (arbitrary bits, nonce=1). Header
    // validation now enforces PoW on mainnet/testnet, so run under regtest
    // (PoW skipped, matching block_acceptor) — correct for scheduler logic tests.
    dinero::SelectParams(dinero::Chain::REGTEST);

    std::cout << "=== BlockDownloadScheduler Regression Tests ===" << std::endl;

    {
        std::cout << "\n1. external backpressure clamp preserves one request slot..." << std::endl;

        dcs::HeaderChainSelector selector;
        try {
            BuildLinearHeaders(selector, 8);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build linear header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);

        std::vector<uint256> requested_hashes;
        scheduler.SetSendGetDataCallback([&requested_hashes](const uint256& block_hash, uint32_t /*height*/) {
            requested_hashes.push_back(block_hash);
        });

        // Simulate saturated CSN pending buffer:
        // before the fix, this blocked all requests and stalled sync.
        scheduler.SetExternalBackpressureCallback([&scheduler]() -> size_t {
            return static_cast<size_t>(scheduler.GetMaxInFlight());
        });

        scheduler.OnHeadersProcessed();
        const size_t missing_before = scheduler.GetMissingBlockCount();
        if (!Require(missing_before > 0, "expected missing blocks after headers are processed")) {
            return 1;
        }

        scheduler.Tick();

        // Regression invariant: even with saturated external backpressure, Tick()
        // must request at least one block to recover gaps and keep liveness.
        if (!Require(requested_hashes.size() == 1, "expected exactly one block request under saturated backpressure")) {
            return 1;
        }
        if (!Require(scheduler.GetInFlightCount() == 1, "expected one in-flight block under saturated backpressure")) {
            return 1;
        }

        std::cout << "   ✅ requested=" << requested_hashes.size()
                  << " in_flight=" << scheduler.GetInFlightCount()
                  << " missing_before=" << missing_before << std::endl;
    }

    {
        std::cout << "\n1b. getdata callback carries each block's true header height..." << std::endl;

        // The daemon wiring uses the height passed here to skip peers whose
        // advertised height is below the block (they would reply NOTFOUND and
        // cancel the in-flight request — the catch-up stall). That filter is
        // only correct if the scheduler hands over the block's real header
        // height, so assert that contract at the scheduler boundary.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;  // index == height (hashes[0] == genesis)
        try {
            BuildLinearHeaders(selector, 6, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);

        std::vector<std::pair<uint256, uint32_t>> requests;
        scheduler.SetSendGetDataCallback([&requests](const uint256& block_hash, uint32_t height) {
            requests.emplace_back(block_hash, height);
        });

        scheduler.OnHeadersProcessed();
        for (int t = 0; t < 8; ++t) scheduler.Tick();  // fan out across the missing range

        if (!Require(!requests.empty(), "expected at least one getdata request")) {
            return 1;
        }

        bool all_correct = true;
        for (const auto& [hash, height] : requests) {
            if (height == 0 || height >= hashes.size() || hashes[height] != hash) {
                all_correct = false;
                std::cerr << "   ❌ getdata carried wrong height " << height
                          << " for block " << hash.GetHex().substr(0, 16) << "..." << std::endl;
                break;
            }
        }
        if (!Require(all_correct, "every getdata must carry the block's true header height")) {
            return 1;
        }

        std::cout << "   ✅ " << requests.size()
                  << " requests, each carrying the correct header height" << std::endl;
    }

    {
        std::cout << "\n2. fork detection requests competing branch block at local tip height..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> main_hashes;
        std::vector<uint256> fork_hashes;
        try {
            BuildLinearHeaders(selector, 3, &main_hashes, 2'000'000);
            AppendForkHeaders(selector, main_hashes[2], 3, &fork_hashes, 3'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build forked header chain: " << e.what() << std::endl;
            return 1;
        }

        const auto best_header = selector.GetBestHeaderValue();
        if (!Require(best_header.has_value(), "expected best header after fork build")) {
            return 1;
        }
        if (!Require(best_header->height == 5, "expected fork tip to become best header")) {
            return 1;
        }

        const auto best_at_tip_height = selector.GetHeaderAtHeightValue(3);
        if (!Require(best_at_tip_height.has_value(), "expected best-chain header at local tip height")) {
            return 1;
        }
        if (!Require(best_at_tip_height->hash == fork_hashes[0], "expected best chain to diverge at height 3")) {
            return 1;
        }

        auto active_chain = BuildActiveChainIndex(main_hashes);
        uint256 active_hash_at_3;
        if (!Require(
                dcs::GetActiveChainHashAtHeight(active_chain.back().get(), 3, active_hash_at_3),
                "expected active ancestry lookup to resolve height 3")) {
            return 1;
        }
        if (!Require(active_hash_at_3 == main_hashes[3], "expected active ancestry lookup to return active-chain hash")) {
            return 1;
        }
        if (!Require(active_hash_at_3 != fork_hashes[0], "expected active ancestry hash to differ from fork hash")) {
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(3);

        std::vector<uint256> requested_hashes;
        scheduler.SetSendGetDataCallback([&requested_hashes](const uint256& block_hash, uint32_t /*height*/) {
            requested_hashes.push_back(block_hash);
        });
        scheduler.SetGetBlockHashAtHeightCallback(
            [&active_chain](uint32_t height, uint256& out_hash) -> bool {
                return dcs::GetActiveChainHashAtHeight(
                    active_chain.back().get(),
                    height,
                    out_hash
                );
            }
        );

        scheduler.OnHeadersProcessed();

        if (!Require(scheduler.GetMissingBlockCount() == 3, "expected fork rescan to queue heights 3-5")) {
            return 1;
        }

        scheduler.Tick();

        if (!Require(!requested_hashes.empty(), "expected at least one request from forked queue")) {
            return 1;
        }
        if (!Require(requested_hashes[0] == fork_hashes[0],
                     "expected scheduler to request competing branch block at local tip height first")) {
            return 1;
        }
        if (!Require(requested_hashes.size() == 3, "expected forked queue to request heights 3-5")) {
            return 1;
        }

        std::cout << "   ✅ first_requested=" << requested_hashes[0].GetHex().substr(0, 16)
                  << "... fork_height=3 requested=" << requested_hashes.size() << std::endl;
    }

    {
        std::cout << "\n3. stale height-index does not suppress fork block at same height..." << std::endl;

        // Regression for commit 568cba13c: the old code used a height-index
        // lookup to decide whether the local chain already had a block at a
        // given height.  When a fork existed at the same height, the stale
        // index returned the fork block's hash, making the scheduler believe
        // the competing branch block was already present — so it was never
        // requested and sync stalled.
        //
        // Topology (mirrors the real-world block-2498 stall):
        //
        //   Main chain:  genesis -> A(1) -> B(2) -> C(3)        [local tip]
        //   Fork chain:  genesis -> A(1) -> B(2) -> X(3) -> Y(4)  [best header]
        //
        // The scheduler must queue X(3) even though the local chain already
        // has a block at height 3 (C).  A stale height-index that maps
        // height 3 → C would see C != X and correctly detect the fork.
        // But if the index is stale and returns *X* (e.g. because a
        // previous header-only import populated it), the scheduler would
        // think height 3 is already satisfied — the exact bug.

        dcs::HeaderChainSelector selector;
        std::vector<uint256> main_hashes;  // [genesis, A, B, C]
        std::vector<uint256> fork_hashes;  // [X, Y]
        try {
            BuildLinearHeaders(selector, 3, &main_hashes, 4'000'000);
            // Fork after B (main_hashes[2]), so X is at height 3, Y at height 4.
            AppendForkHeaders(selector, main_hashes[2], 2, &fork_hashes, 5'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build forked header chain: " << e.what() << std::endl;
            return 1;
        }

        // Sanity: fork tip Y(4) is the best header.
        const auto best_header = selector.GetBestHeaderValue();
        if (!Require(best_header.has_value() && best_header->height == 4,
                     "expected fork tip Y at height 4 as best header")) {
            return 1;
        }

        // The best header chain at height 3 should be X (fork), not C (main).
        const auto header_at_3 = selector.GetHeaderAtHeightValue(3);
        if (!Require(header_at_3.has_value() && header_at_3->hash == fork_hashes[0],
                     "expected best-chain header at height 3 to be fork block X")) {
            return 1;
        }

        // === BUG-DOCUMENTING SCENARIO: stale height-index returns fork's hash ===
        // Simulates a height-index that was populated by header import and
        // maps height 3 → X (the fork block).  ScanForMissingBlocks compares
        // get_block_hash_at_height(3) against header_chain[3] (also X), sees
        // equality, and concludes "no fork" — skipping height 3 entirely.
        // Y(4) then stalls because its predecessor X was never requested.
        //
        // This sub-scenario DOCUMENTS the limitation: with a stale height-index
        // callback the scheduler is fundamentally unable to detect the fork
        // (the only oracle it queries agrees with the header chain). The
        // architectural fix lives at the CALLSITE — production wires the
        // callback to consensus::GetActiveChainHashAtHeight (active-tip pprev
        // walker, see include/consensus/active_chain_ancestry.h), which
        // "deliberately ignores persisted height indexes because they can lag
        // or remain stale across reorgs while the active tip/pprev chain is
        // already authoritative in memory." That contract is exercised in the
        // CORRECT SCENARIO below.
        //
        // We assert the documented stale-callback behaviour (missing=1) so
        // any future scheduler change that grows a second oracle, or that
        // alters this stall, fires this assert and forces a deliberate
        // re-evaluation of the contract.
        {
            dcs::BlockDownloadScheduler scheduler_buggy(&selector, nullptr);
            scheduler_buggy.SetLocalTipHeight(3);

            std::vector<uint256> requested;
            scheduler_buggy.SetSendGetDataCallback([&requested](const uint256& h, uint32_t /*height*/) {
                requested.push_back(h);
            });

            // Stale callback: returns fork_hashes[0] (X) for height 3.
            // This is what the old height-index did — it got updated when
            // headers were imported but before blocks were connected.
            scheduler_buggy.SetGetBlockHashAtHeightCallback(
                [&fork_hashes](uint32_t height, uint256& out_hash) -> bool {
                    if (height == 3) {
                        out_hash = fork_hashes[0];  // X — stale index entry
                        return true;
                    }
                    return false;
                }
            );

            scheduler_buggy.OnHeadersProcessed();

            // With a stale callback the scheduler cannot detect the fork:
            // get_block_hash_at_height(3) == header_chain[3] (both X), so
            // start_height = local_tip+1 = 4, and only Y is queued.
            // This is the diagnostic the CORRECT SCENARIO contract avoids
            // by using GetActiveChainHashAtHeight at the callsite.
            const size_t missing_buggy = scheduler_buggy.GetMissingBlockCount();
            if (!Require(missing_buggy == 1,
                         "stale-callback path: scheduler must observe missing=1 "
                         "(only Y at height 4); fork at height 3 is undetectable "
                         "from the single agreeing oracle "
                         "(got " + std::to_string(missing_buggy) +
                         "). If this fires, the scheduler grew a second oracle "
                         "or its scan logic changed — re-evaluate the "
                         "active-ancestry callsite contract before relaxing.")) {
                return 1;
            }

            std::cout << "   ✅ stale-index limitation documented: missing=" << missing_buggy
                      << " (X at height 3 is undetectable without ancestry walker)"
                      << std::endl;
        }

        // === CORRECT SCENARIO: active-tip ancestry callback ===
        // The fix uses pprev-chain walking from the active tip (C), which
        // correctly returns C at height 3, detects C != X, and queues X.
        {
            auto active_chain = BuildActiveChainIndex(main_hashes);

            dcs::BlockDownloadScheduler scheduler_fixed(&selector, nullptr);
            scheduler_fixed.SetLocalTipHeight(3);

            std::vector<uint256> requested;
            scheduler_fixed.SetSendGetDataCallback([&requested](const uint256& h, uint32_t /*height*/) {
                requested.push_back(h);
            });
            scheduler_fixed.SetGetBlockHashAtHeightCallback(
                [&active_chain](uint32_t height, uint256& out_hash) -> bool {
                    return dcs::GetActiveChainHashAtHeight(
                        active_chain.back().get(), height, out_hash);
                }
            );

            scheduler_fixed.OnHeadersProcessed();

            if (!Require(scheduler_fixed.GetMissingBlockCount() == 2,
                         "active-ancestry callback must queue X(3) and Y(4)")) {
                return 1;
            }

            scheduler_fixed.Tick();

            if (!Require(!requested.empty() && requested[0] == fork_hashes[0],
                         "first requested block must be fork block X at height 3")) {
                return 1;
            }

            std::cout << "   ✅ active-ancestry scenario: missing=2"
                      << " first_requested=" << requested[0].GetHex().substr(0, 16)
                      << "..." << std::endl;
        }
    }

    {
        std::cout << "\n4. stateless liveness slot resets to the first gap..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 20, &hashes, 6'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build linear header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetStatelessMode(true);
        scheduler.SetLocalTipHeight(0);
        scheduler.SetGetTipHeightCallback([]() -> uint32_t { return 0; });

        size_t pending_count = 0;
        scheduler.SetExternalBackpressureCallback([&pending_count]() -> size_t {
            return pending_count;
        });

        std::vector<uint256> requested_hashes;
        scheduler.SetSendGetDataCallback([&requested_hashes](const uint256& block_hash, uint32_t /*height*/) {
            requested_hashes.push_back(block_hash);
        });

        scheduler.OnHeadersProcessed();
        scheduler.Tick();

        const size_t window = scheduler.GetMaxInFlight();
        if (!Require(requested_hashes.size() == window,
                     "expected initial stateless window to request max_in_flight blocks")) {
            return 1;
        }

        // Simulate the CSN reorder buffer receiving heights 2..window while the
        // true gap block at height 1 never arrives.
        for (size_t i = 1; i < window; ++i) {
            if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, requested_hashes[i]))) {
                std::cerr << "   ❌ failed to inject received block at window index " << i << std::endl;
                return 1;
            }
        }

        // Height 1 is still missing, but the pending reorder buffer is saturated.
        pending_count = window;
        if (!scheduler.ReRequestBlock(requested_hashes[0])) {
            std::cerr << "   ❌ failed to re-request the missing gap block" << std::endl;
            return 1;
        }

        const size_t requests_before_gap_retry = requested_hashes.size();
        scheduler.Tick();

        if (!Require(requested_hashes.size() == requests_before_gap_retry + 1,
                     "expected exactly one liveness-slot request under saturated stateless backpressure")) {
            return 1;
        }
        if (!Require(requested_hashes.back() == requested_hashes.front(),
                     "stateless liveness slot must re-request the first missing gap block, not drift ahead")) {
            return 1;
        }

        std::cout << "   ✅ first_gap=" << requested_hashes.front().GetHex().substr(0, 16)
                  << "... retried_after_backpressure=true window=" << window << std::endl;
    }

    {
        std::cout << "\n5. stateless frontier re-requests stale RECEIVED gap..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 20, &hashes, 7'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build linear header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetStatelessMode(true);
        scheduler.SetLocalTipHeight(0);
        scheduler.SetGetTipHeightCallback([]() -> uint32_t { return 0; });

        size_t pending_count = 0;
        scheduler.SetExternalBackpressureCallback([&pending_count]() -> size_t {
            return pending_count;
        });

        std::vector<uint256> requested_hashes;
        scheduler.SetSendGetDataCallback([&requested_hashes](const uint256& block_hash, uint32_t /*height*/) {
            requested_hashes.push_back(block_hash);
        });

        scheduler.OnHeadersProcessed();
        scheduler.Tick();

        const size_t window = scheduler.GetMaxInFlight();
        if (!Require(requested_hashes.size() == window,
                     "expected initial stateless window to request max_in_flight blocks")) {
            return 1;
        }

        // Simulate the frontier gap block having been downloaded/stored once,
        // while the active tip is still stuck below it. All later heights are
        // buffered, saturating external backpressure. The liveness slot must
        // re-request the gap block instead of drifting ahead.
        if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, requested_hashes[0]))) {
            std::cerr << "   ❌ failed to inject stale frontier RECEIVED block" << std::endl;
            return 1;
        }
        for (size_t i = 1; i < window; ++i) {
            if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, requested_hashes[i]))) {
                std::cerr << "   ❌ failed to inject buffered block at window index " << i << std::endl;
                return 1;
            }
        }

        pending_count = window;
        const size_t requests_before_retry = requested_hashes.size();
        scheduler.Tick();

        if (!Require(requested_hashes.size() == requests_before_retry + 1,
                     "expected exactly one frontier retry under saturated stateless backpressure")) {
            return 1;
        }
        if (!Require(requested_hashes.back() == requested_hashes.front(),
                     "frontier RECEIVED state must re-request the missing active-chain gap")) {
            return 1;
        }

        std::cout << "   ✅ stale_received_gap=" << requested_hashes.front().GetHex().substr(0, 16)
                  << "... retried=true window=" << window << std::endl;
    }

    {
        std::cout << "\n6. stateless reorg barrier assembles the whole competing branch (frontier first)..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> main_hashes;
        std::vector<uint256> fork_hashes;
        try {
            BuildLinearHeaders(selector, 3, &main_hashes, 8'000'000);
            AppendForkHeaders(selector, main_hashes[2], 3, &fork_hashes, 9'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build stateless reorg test chain: " << e.what() << std::endl;
            return 1;
        }

        uint32_t active_tip_height = 3;
        bool fork_replacement_active = false;

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetStatelessMode(true);
        scheduler.SetLocalTipHeight(active_tip_height);
        scheduler.SetGetTipHeightCallback([&active_tip_height]() -> uint32_t {
            return active_tip_height;
        });
        scheduler.SetGetBlockHashAtHeightCallback(
            [&](uint32_t height, uint256& out_hash) -> bool {
                if (height == 0) {
                    out_hash = main_hashes[0];
                    return true;
                }
                if (height == 1) {
                    out_hash = main_hashes[1];
                    return true;
                }
                if (height == 2) {
                    out_hash = main_hashes[2];
                    return true;
                }
                if (height == 3) {
                    out_hash = fork_replacement_active ? fork_hashes[0] : main_hashes[3];
                    return true;
                }
                return false;
            }
        );

        std::vector<uint256> requested_hashes;
        scheduler.SetSendGetDataCallback([&requested_hashes](const uint256& block_hash, uint32_t /*height*/) {
            requested_hashes.push_back(block_hash);
        });

        scheduler.OnHeadersProcessed();
        if (!Require(scheduler.GetMissingBlockCount() == 3,
                     "expected fork rescan to queue competing heights 3-5")) {
            return 1;
        }

        scheduler.Tick();

        // ASSEMBLY BARRIER (see BlockDownloadScheduler::TickLocked +
        // docs/design/csn-stateless-reorg-convergence.md): a competing branch
        // can only win once the WHOLE branch is downloaded, so the scheduler
        // requests the fork-point frontier AND its descendants in one pass
        // instead of holding after the ancestor. ActivateBestChain owns the
        // canonical rewind + replay; the scheduler only assembles.
        if (!Require(requested_hashes.size() == 3,
                     "stateless reorg barrier must assemble the whole competing branch (heights 3-5)")) {
            return 1;
        }
        if (!Require(requested_hashes.front() == fork_hashes[0],
                     "expected the fork-point frontier (height 3) to be requested first")) {
            return 1;
        }
        const bool assembled_descendants =
            std::find(requested_hashes.begin(), requested_hashes.end(),
                      fork_hashes[1]) != requested_hashes.end() &&
            std::find(requested_hashes.begin(), requested_hashes.end(),
                      fork_hashes[2]) != requested_hashes.end();
        if (!Require(assembled_descendants,
                     "expected descendants (heights 4-5) to be assembled without waiting for the ancestor to activate")) {
            return 1;
        }

        // The barrier assembles without needing the ancestor to activate first:
        // once a competing block becomes the active replacement, ActivateBestChain
        // drives the reorg and the scheduler issues no duplicate re-requests.
        fork_replacement_active = true;
        const size_t requests_before_activation = requested_hashes.size();
        scheduler.Tick();
        if (!Require(requested_hashes.size() == requests_before_activation,
                     "expected no duplicate re-requests after replacement ancestor became active")) {
            return 1;
        }

        std::cout << "   ✅ assembled_branch frontier_first="
                  << requested_hashes.front().GetHex().substr(0, 16)
                  << "... descendants=" << (requested_hashes.size() - 1) << std::endl;
    }

    {
        std::cout << "\n6. issue #216: stale-timeout must NOT re-request a block already on the active chain..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 4, &hashes);  // genesis + heights 1..4
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);

        std::unordered_map<uint256, int> sends;
        scheduler.SetSendGetDataCallback([&sends](const uint256& h, uint32_t) { sends[h]++; return true; });

        // Active-chain hash lookup: the chain holds the linear hash at each height
        // up to `connected_tip` (blocks above it aren't connected yet).
        uint32_t connected_tip = 0;
        scheduler.SetGetBlockHashAtHeightCallback(
            [&hashes, &connected_tip](uint32_t height, uint256& out) -> bool {
                if (height >= hashes.size() || height > connected_tip) return false;
                out = hashes[height];
                return true;
            });

        scheduler.OnHeadersProcessed();
        for (int t = 0; t < 20; ++t) scheduler.Tick();  // request the whole 1..4 range
        const int s1_before = sends[hashes[1]];
        const int s2_before = sends[hashes[2]];
        if (!Require(s1_before >= 1 && s2_before >= 1, "setup: blocks 1 and 2 should each be requested")) return 1;

        // Block 1 connects to the active chain; tip advances to 1. Force the stale timeout.
        connected_tip = 1;
        scheduler.SetLocalTipHeight(1);
        scheduler.SetStaleRequestTimeoutSeconds(0);
        scheduler.Tick();

        // FIX: block 1 is on the active chain (hash matches at its height) → NOT re-requested.
        if (!Require(sends[hashes[1]] == s1_before,
                     "#216: a block already on the active chain must NOT be re-requested by the stale timeout")) return 1;
        // Safety net: block 2 (height 2 > tip 1) is genuinely missing → still re-requested.
        if (!Require(sends[hashes[2]] > s2_before,
                     "a genuinely-missing block must still be re-requested after the timeout")) return 1;
        std::cout << "   ✅ connected block not re-requested; missing block retried" << std::endl;
    }

    {
        std::cout << "\n6b. stored-but-not-yet-active body is adopted instead of re-requested..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 2, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);
        std::unordered_map<uint256, int> sends;
        scheduler.SetSendGetDataCallback(
            [&sends](const uint256& h, uint32_t) { sends[h]++; });

        bool body_persisted = false;
        scheduler.SetGetBlockBodyPositionCallback(
            [&hashes, &body_persisted](const uint256& hash, uint32_t height)
                -> std::optional<dinero::FilePosition> {
                if (body_persisted && height == 1 && hash == hashes[1]) {
                    return dinero::FilePosition(1, 128, 256);
                }
                return std::nullopt;
            });

        scheduler.OnHeadersProcessed();
        for (int t = 0; t < 8; ++t) scheduler.Tick();
        const int first_before = sends[hashes[1]];
        const int second_before = sends[hashes[2]];
        if (!Require(first_before >= 1 && second_before >= 1,
                     "setup: both bodies should be requested")) return 1;

        // A parallel relay/acceptance path durably stores block 1 while its
        // expensive ConnectTip is still running. The active tip remains at 0,
        // so the older active-chain-only #216 guard cannot see it yet.
        body_persisted = true;
        scheduler.SetStaleRequestTimeoutSeconds(0);
        scheduler.Tick();

        if (!Require(sends[hashes[1]] == first_before,
                     "stored body must not be re-requested while validation is pending")) {
            return 1;
        }
        if (!Require(sends[hashes[2]] > second_before,
                     "genuinely absent body must still be retried")) {
            return 1;
        }
        if (!Require(scheduler.HasReceivedBlock(hashes[1]),
                     "adopted body must enter received bookkeeping")) {
            return 1;
        }
        std::cout << "   ✅ durable body adopted; absent sibling retried" << std::endl;
    }

    {
        std::cout << "\n7. issue #216 reorg-safety: a stale block whose hash differs from the active chain at its height IS re-requested..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 4, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);
        std::unordered_map<uint256, int> sends;
        scheduler.SetSendGetDataCallback([&sends](const uint256& h, uint32_t) { sends[h]++; return true; });

        // The active chain holds a DIFFERENT hash at height 1 (a fork) — so the
        // requested best-chain block (hashes[1]) is NOT what's connected there.
        scheduler.SetGetBlockHashAtHeightCallback(
            [](uint32_t height, uint256& out) -> bool {
                if (height == 1) { out.SetNull(); return true; }  // != hashes[1]
                return false;
            });

        scheduler.OnHeadersProcessed();
        for (int t = 0; t < 20; ++t) scheduler.Tick();
        const int s1_before = sends[hashes[1]];
        if (!Require(s1_before >= 1, "setup: block 1 should be requested")) return 1;

        scheduler.SetLocalTipHeight(1);  // tip says height 1 reached, but with a fork hash
        scheduler.SetStaleRequestTimeoutSeconds(0);
        scheduler.Tick();

        // Hash-precise guard: chain hash at height 1 != hashes[1] → must re-request.
        // A bare height<=tip check would wrongly mark this RECEIVED → reorg stall.
        if (!Require(sends[hashes[1]] > s1_before,
                     "#216 reorg-safety: a block whose hash != the active-chain hash at its height MUST be re-requested")) return 1;
        std::cout << "   ✅ fork block correctly re-requested" << std::endl;
    }

    {
        std::cout << "\n8. issue #241: a peer that replied NOTFOUND is excluded from the "
                     "re-request skip-set at/below that height (snapshot-peer wedge)..." << std::endl;

        // The from-genesis IBD wedge: an AssumeUTXO/snapshot peer advertises the
        // full chain height (so it clears the advertised-height filter) but lacks
        // pre-snapshot block bodies, so it replies NOTFOUND. Before the fix,
        // OnBlockNotFound only erased the in-flight entry and the next rescan
        // re-broadcast to the SAME peer — an infinite loop. The scheduler must
        // now remember the peer is body-incapable at/below that height and stage
        // it into the per-request skip-set so the daemon callback drops it.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;  // index == height (hashes[0] == genesis)
        try {
            BuildLinearHeaders(selector, 6, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);

        const std::string bad_peer = "192.0.2.7:1234";  // RFC 5737 test address

        // Capture the skip-set staged for every send, keyed by requested hash.
        // CurrentRequestSkipPeers() is read inside the callback, which fires
        // under the scheduler lock right after the skip-set is staged.
        std::vector<std::pair<uint256, std::unordered_set<std::string>>> sends;
        scheduler.SetSendGetDataCallback(
            [&scheduler, &sends](const uint256& h, uint32_t /*height*/) {
                sends.emplace_back(h, scheduler.CurrentRequestSkipPeers());
                return true;
            });

        scheduler.OnHeadersProcessed();
        for (int t = 0; t < 10; ++t) scheduler.Tick();  // request the 1..6 range

        // Baseline: with no NOTFOUND recorded, no peer is ever skipped.
        for (const auto& s : sends) {
            if (!Require(s.second.empty(),
                         "no peer should be in the skip-set before any NOTFOUND")) {
                return 1;
            }
        }

        // The snapshot peer NOTFOUNDs the block at height 3.
        scheduler.OnBlockNotFound(hashes[3], bad_peer);

        // The post-NOTFOUND rescan re-requests that block. The staged skip-set
        // for the height-3 request must now exclude the body-incapable peer.
        sends.clear();
        if (!Require(scheduler.ReRequestBlock(hashes[3]),
                     "expected the NOTFOUND'd block to be re-queued")) {
            return 1;
        }
        for (int t = 0; t < 10; ++t) scheduler.Tick();

        bool bad_peer_skipped_at_3 = false;
        for (const auto& s : sends) {
            if (s.first == hashes[3] && s.second.count(bad_peer)) {
                bad_peer_skipped_at_3 = true;
            }
        }
        if (!Require(bad_peer_skipped_at_3,
                     "#241: a peer that replied NOTFOUND for a block at height H must be "
                     "staged into the skip-set when that block is re-requested")) {
            return 1;
        }

        // Height gating: a request ABOVE the gap height must NOT skip the peer
        // (it may legitimately have higher bodies, e.g. its own snapshot range).
        scheduler.OnBlockNotFound(hashes[2], bad_peer);  // still ≤3, no change to gap
        sends.clear();
        if (!Require(scheduler.ReRequestBlock(hashes[5]),
                     "expected the height-5 block to be re-queued")) {
            return 1;
        }
        for (int t = 0; t < 10; ++t) scheduler.Tick();
        bool bad_peer_skipped_at_5 = false;
        for (const auto& s : sends) {
            if (s.first == hashes[5] && s.second.count(bad_peer)) {
                bad_peer_skipped_at_5 = true;
            }
        }
        if (!Require(!bad_peer_skipped_at_5,
                     "#241: a peer marked body-incapable at/below height 3 must NOT be "
                     "skipped for a request at height 5 (gating is height-bounded)")) {
            return 1;
        }

        std::cout << "   ✅ NOTFOUND peer skipped at height 3, retained at height 5" << std::endl;
    }

    {
        std::cout << "\n9. issue #241/#214: a blocking send callback must not wedge other "
                     "scheduler threads (send must happen outside mutex_)..." << std::endl;

        // Live-fleet wedge signature (rc37 cold-IBD reproduction): the
        // send_getdata callback ultimately reaches a blocking ::send() on a
        // peer socket. If a peer stops draining its socket (half-dead
        // connection, full 128KB send buffer), the send blocks indefinitely.
        // When the scheduler invokes the callback while holding mutex_, that
        // one blocked send wedges EVERY peer handler thread (OnNewBlock ->
        // Tick) and the tick loop on the scheduler mutex: block ingest stops,
        // the tip freezes, and the process looks healthy. Observed live:
        // 437 OnNewBlock ENTRY vs 385 EXIT (52 threads parked), sendto()
        // blocked across the whole sample, tip frozen at height 385.
        //
        // Contract under test: while one thread's send callback is parked
        // (simulating the blocked sendto), other threads must still be able
        // to enter the scheduler.
        dcs::HeaderChainSelector selector;
        try {
            BuildLinearHeaders(selector, 8);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build linear header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);

        std::promise<void> callback_parked;
        auto callback_parked_seen = callback_parked.get_future();
        std::atomic<bool> release_send{false};
        std::atomic<bool> first_send{true};
        scheduler.SetSendGetDataCallback([&](const uint256&, uint32_t) {
            // Park only the first send: simulates sendto() blocked forever on
            // a peer with a full TCP send buffer.
            if (first_send.exchange(false)) {
                callback_parked.set_value();
                while (!release_send.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });

        scheduler.OnHeadersProcessed();

        std::thread ticker([&scheduler]() { scheduler.Tick(); });

        bool parked = callback_parked_seen.wait_for(std::chrono::seconds(5)) ==
                      std::future_status::ready;
        if (!parked) {
            release_send.store(true);
            ticker.join();
            if (!Require(false, "send callback was never invoked (test setup broken)")) {
                return 1;
            }
        }

        // A second thread (a peer handler delivering a block, the tick loop,
        // an RPC status probe) must not be blocked behind the parked send.
        auto probe = std::async(std::launch::async, [&scheduler]() {
            (void)scheduler.GetInFlightCount();
            (void)scheduler.IsFullySynchronized();
        });
        const bool scheduler_responsive =
            probe.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

        release_send.store(true);
        ticker.join();
        probe.wait();

        if (!Require(scheduler_responsive,
                     "#241/#214: scheduler mutex_ must not be held across the send "
                     "callback — one blocked peer send wedges all block ingest")) {
            return 1;
        }

        std::cout << "   ✅ scheduler stayed responsive while a send callback was blocked"
                  << std::endl;
    }

    {
        std::cout << "\n10. issue #241 perf: ScanForMissingBlocks must be linear in the "
                     "header window (no per-height tip walk)..." << std::endl;

        // Live profile on a from-genesis sync (~39k headers): the scan called
        // GetHeaderAtHeight(h) for every missing height, and each call walked
        // parent pointers from the best header — O(n^2) total, re-run on
        // EVERY headers message, all under the scheduler mutex_. One core
        // pinned at 100% in GetHeaderAtHeight; block ingest throttled to
        // ~7 blocks/min. The scan must instead walk the header chain once.
        //
        // Budget: a linear scan of 60k headers is ~milliseconds. The O(n^2)
        // version is billions of pointer derefs (seconds to minutes). 5s is
        // >100x headroom for slow CI while still failing the quadratic scan.
        dcs::HeaderChainSelector selector;
        try {
            BuildLinearHeaders(selector, 60000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build 60k header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);
        scheduler.SetSendGetDataCallback([](const uint256&, uint32_t) {});

        const auto t0 = std::chrono::steady_clock::now();
        scheduler.OnHeadersProcessed();  // triggers ScanForMissingBlocks
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);

        if (!Require(scheduler.GetMissingBlockCount() == 60000,
                     "scan must queue all 60000 missing blocks (got " +
                         std::to_string(scheduler.GetMissingBlockCount()) + ")")) {
            return 1;
        }
        if (!Require(elapsed < std::chrono::seconds(5),
                     "ScanForMissingBlocks took " + std::to_string(elapsed.count()) +
                         "ms for 60k headers — quadratic per-height tip walk is back")) {
            return 1;
        }

        std::cout << "   ✅ 60k-header scan completed in " << elapsed.count() << "ms"
                  << std::endl;
    }


    {
        std::cout << "\n11. assumeutxo backfill: EnableBackfill queues exactly the missing "
                     "pre-base heights, skips bodies already present, exposes progress; "
                     "backfill never flips IsFullySynchronized..." << std::endl;

        // Topology: genesis + heights 1..8.  SetLocalTipHeight(8) simulates an
        // AssumeUTXO snapshot loaded at base=8 — tip sync is fully satisfied,
        // but pre-base bodies 1..8 need to be backfilled.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;  // hashes[i] == header hash at height i
        try {
            BuildLinearHeaders(selector, 8, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(8);   // snapshot base = 8
        scheduler.OnHeadersProcessed();   // tip sync: nothing missing

        // Node already has bodies at heights 3 and 5 (e.g. partial prior backfill).
        scheduler.SetHasBlockBodyCallback([](const uint256& /*hash*/, uint32_t height) -> bool {
            return height == 3 || height == 5;
        });

        std::vector<uint32_t> requested_heights;
        scheduler.SetSendGetDataCallback([&](const uint256& /*hash*/, uint32_t height) {
            requested_heights.push_back(height);
        });

        if (!Require(scheduler.IsFullySynchronized(), "pre: should be synced at tip after snapshot")) {
            return 1;
        }

        // Regression (anchor-by-hash): EnableBackfill with a WRONG anchor hash
        // for end_height must refuse to enable. A height-anchored walk would
        // silently feed a diverged best chain's hashes to the downloader; the
        // hash anchor makes that impossible.
        scheduler.EnableBackfill(1, 8, hashes[7]);  // hashes[7] is height 7, not 8
        if (!Require(!scheduler.GetBackfillProgress().enabled,
                     "EnableBackfill with wrong anchor hash must leave backfill disabled")) {
            return 1;
        }

        scheduler.EnableBackfill(1, 8, hashes[8]);   // heights 1..base, anchored on base hash

        auto prog = scheduler.GetBackfillProgress();
        if (!Require(prog.enabled, "backfill not enabled after EnableBackfill")) return 1;
        // 8 heights total, 2 already present (3 and 5) → 6 missing
        if (!Require(prog.total == 6,
                     "expected 6 missing (1,2,4,6,7,8), got " + std::to_string(prog.total))) {
            return 1;
        }
        if (!Require(prog.completed == 0, "no bodies completed yet")) return 1;
        if (!Require(prog.start_height == 1, "start_height must be 1")) return 1;
        if (!Require(prog.end_height == 8, "end_height must be 8")) return 1;

        // Backfill must NOT flip IsFullySynchronized (it lives in its own queue).
        if (!Require(scheduler.IsFullySynchronized(),
                     "backfill must not flip IsFullySynchronized")) {
            return 1;
        }

        // Idempotent re-enable with the same range must not duplicate the queue.
        scheduler.EnableBackfill(1, 8, hashes[8]);
        if (!Require(scheduler.GetBackfillProgress().total == 6,
                     "idempotent re-enable must not duplicate queue")) {
            return 1;
        }

        // DisableBackfill resets all state.
        scheduler.DisableBackfill();
        if (!Require(!scheduler.GetBackfillProgress().enabled, "disable failed")) return 1;
        if (!Require(scheduler.GetBackfillProgress().total == 0, "total should be 0 after disable")) return 1;

        // After disable, IsFullySynchronized must still hold.
        if (!Require(scheduler.IsFullySynchronized(),
                     "IsFullySynchronized must remain true after DisableBackfill")) {
            return 1;
        }

        // Task 2: with tip sync idle (snapshot tip == best header, nothing in
        // missing_blocks_), Tick SERVICES the backfill queue and requests
        // exactly the 6 missing pre-base heights via the staged-dispatch path.
        scheduler.EnableBackfill(1, 8, hashes[8]);
        scheduler.Tick();
        const std::vector<uint32_t> want{1, 2, 4, 6, 7, 8};
        if (!Require(requested_heights == want,
                     "#300: backfill requests must be staged in ascending height order")) {
            return 1;
        }
        std::vector<uint32_t> got = requested_heights;
        if (!Require(got == want,
                     "Tick must service the backfill queue (Task 2): expected exactly "
                     "the 6 missing heights {1,2,4,6,7,8}, got " +
                         std::to_string(got.size()) + " requests")) {
            return 1;
        }

        std::cout << "   ✅ backfill total=" << scheduler.GetBackfillProgress().total
                  << " IsFullySynchronized preserved, 6 heights serviced when tip idle"
                  << std::endl;
    }


    {
        std::cout << "\n12. assumeutxo backfill: reserved window share while tip sync is "
                     "busy (half the window), full window when idle, quota is a hard "
                     "cap..." << std::endl;

        // Topology: genesis + heights 1..40. Snapshot base = 20: tip sync owns
        // 21..40 (20 blocks), backfill owns 1..20 (20 blocks > max_in_flight 16,
        // so both the reserve quota and the full-window cap are observable).
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 40, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(20);  // snapshot base = 20

        std::vector<std::pair<uint256, uint32_t>> requests;  // (hash, height)
        scheduler.SetSendGetDataCallback([&requests](const uint256& h, uint32_t height) {
            requests.emplace_back(h, height);
        });

        scheduler.OnHeadersProcessed();   // tip sync: heights 21..40 missing
        scheduler.EnableBackfill(1, 20, hashes[20]);  // pre-base bodies 1..20, anchored on base hash

        // Tick #1: tip sync fills its window (16), and backfill gets its
        // RESERVED share on the same tick — half the window (8) — instead of
        // starving while tip work is outstanding. Total in-flight is bounded
        // by max_window + quota.
        const size_t max_window = scheduler.GetMaxInFlight();
        const size_t busy_quota = std::max<size_t>(1, max_window / 2);
        scheduler.Tick();
        size_t tip_reqs = 0, backfill_reqs = 0;
        for (const auto& rq : requests) {
            (rq.second > 20 ? tip_reqs : backfill_reqs)++;
        }
        if (!Require(tip_reqs == max_window,
                     "expected tip sync to fill its window (got " +
                         std::to_string(tip_reqs) + ")")) {
            return 1;
        }
        if (!Require(backfill_reqs == busy_quota,
                     "backfill must get its reserved half-window share while tip "
                     "sync is busy (got " + std::to_string(backfill_reqs) + ")")) {
            return 1;
        }
        if (!Require(scheduler.GetBackfillProgress().in_flight == busy_quota,
                     "backfill in_flight must equal the busy quota")) {
            return 1;
        }
        if (!Require(scheduler.GetInFlightCount() == max_window + busy_quota,
                     "total in-flight must be bounded by max_window + quota (got " +
                         std::to_string(scheduler.GetInFlightCount()) + ")")) {
            return 1;
        }

        // Drain the first window of TIP blocks (leave backfill bodies pending
        // so its in-flight stays pinned at the quota).
        auto first_window = requests;
        requests.clear();
        for (const auto& rq : first_window) {
            if (rq.second <= 20) continue;  // backfill request — not delivered
            if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, rq.first))) {
                std::cerr << "   ❌ failed to inject tip block at height " << rq.second << std::endl;
                return 1;
            }
        }

        // Tick #2: the remaining 4 tip blocks become REQUESTED. Backfill is
        // still holding its full quota (tip busy → quota unchanged), so the
        // quota acts as a HARD cap: no additional backfill getdata may be
        // staged beyond it.
        scheduler.Tick();
        if (!Require(requests.size() == 4, "expected the 4 remaining tip requests")) {
            return 1;
        }
        for (const auto& rq : requests) {
            if (!Require(rq.second > 20,
                         "backfill exceeded its reserved quota while tip busy (height " +
                             std::to_string(rq.second) + ")")) {
                return 1;
            }
        }
        if (!Require(scheduler.GetBackfillProgress().in_flight == busy_quota,
                     "backfill in_flight must stay at the busy quota")) {
            return 1;
        }
        auto second_window = requests;
        requests.clear();
        for (const auto& rq : second_window) {
            if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, rq.first))) {
                std::cerr << "   ❌ failed to inject tip block at height " << rq.second << std::endl;
                return 1;
            }
        }

        // Tick #3: tip queue fully RECEIVED → idle. Backfill's quota widens to
        // the FULL window; with busy_quota already in flight it tops up the
        // remaining slots.
        scheduler.Tick();
        if (!Require(!requests.empty(), "backfill not serviced after tip idle")) {
            return 1;
        }
        for (const auto& rq : requests) {
            if (!Require(rq.second <= 20,
                         "non-backfill height requested after tip idle (height " +
                             std::to_string(rq.second) + ")")) {
                return 1;
            }
        }
        if (!Require(requests.size() == max_window - busy_quota,
                     "backfill must top up exactly to the full window when tip idle (got " +
                         std::to_string(requests.size()) + ")")) {
            return 1;
        }
        if (!Require(scheduler.GetInFlightCount() == max_window,
                     "backfill requests must share the global in-flight accounting")) {
            return 1;
        }
        if (!Require(scheduler.GetBackfillProgress().in_flight == max_window,
                     "backfill progress in_flight must track its staged requests")) {
            return 1;
        }
        if (!Require(scheduler.IsFullySynchronized(),
                     "backfill servicing must not flip IsFullySynchronized")) {
            return 1;
        }

        std::cout << "   ✅ backfill held its reserved " << busy_quota << "-slot share while "
                  << "tip sync was busy, then filled the full " << max_window
                  << "-slot window when idle" << std::endl;
    }

    // The MSG_BLOCK-vs-MSG_UTREEXO_BLOCK routing invariant. The daemon wiring
    // (daemon_app.cpp) reads CurrentRequestIsBackfill() *inside* the send
    // callback to decide the getdata inv type: a backfill body must be
    // requested as raw MSG_BLOCK (archival bridges cannot serve historical
    // utreexo proofs, so MSG_UTREEXO_BLOCK for a pre-base body goes unserved
    // and wedges backfill), while tip work stays MSG_UTREEXO_BLOCK. When tip
    // and backfill sends drain in the SAME batch, the flag must be correct
    // per-send — not a single value smeared across the batch. This guards the
    // routing signal at the scheduler seam the daemon depends on.
    {
        std::cout << "\n📋 Section: per-send backfill routing flag (MSG_BLOCK vs "
                     "MSG_UTREEXO_BLOCK)" << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 40, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(20);  // snapshot base = 20

        // Capture the routing flag AT SEND TIME, per request. The daemon reads
        // it exactly here — synchronously inside the callback — so the flag is
        // only meaningful in this scope (DispatchDeferredSends resets it to
        // false after the batch). Key by height: tip heights (21..40) and
        // backfill heights (1..20) are disjoint, so no collision.
        std::unordered_map<uint32_t, bool> flag_at_send;
        scheduler.SetSendGetDataCallback(
            [&flag_at_send, &scheduler](const uint256&, uint32_t height) {
                flag_at_send[height] = scheduler.CurrentRequestIsBackfill();
            });

        scheduler.OnHeadersProcessed();   // tip sync: heights 21..40 missing
        scheduler.EnableBackfill(1, 20, hashes[20]);  // pre-base bodies 1..20

        // Tick #1 drains tip sends (>20) and backfill sends (<=20) in one batch.
        scheduler.Tick();

        size_t tip_seen = 0, backfill_seen = 0;
        for (const auto& [height, is_backfill] : flag_at_send) {
            if (height > 20) {
                ++tip_seen;
                if (!Require(!is_backfill,
                             "tip send at height " + std::to_string(height) +
                                 " must NOT carry the backfill flag (would force "
                                 "MSG_BLOCK for tip work)")) {
                    return 1;
                }
            } else {
                ++backfill_seen;
                if (!Require(is_backfill,
                             "backfill send at height " + std::to_string(height) +
                                 " must carry the backfill flag (else MSG_UTREEXO_"
                                 "BLOCK wedges the pre-base body)")) {
                    return 1;
                }
            }
        }

        // Non-vacuous: both lanes must have actually sent in this batch, or the
        // per-send assertion above proved nothing about the mixed case.
        if (!Require(tip_seen > 0, "expected at least one tip send in the batch")) {
            return 1;
        }
        if (!Require(backfill_seen > 0,
                     "expected at least one backfill send in the batch")) {
            return 1;
        }

        std::cout << "   ✅ routing flag correct per-send across a mixed batch ("
                  << tip_seen << " tip sends flagged tip, " << backfill_seen
                  << " backfill sends flagged backfill)" << std::endl;
    }

    {
        std::cout << "\n13. assumeutxo backfill: received backfill block is stored-only — "
                     "completion accounting without connect bookkeeping..." << std::endl;

        // Setup as case 11 with tip at 4 == snapshot base; tip sync is idle so
        // backfill is serviced immediately.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 4, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(4);   // snapshot base = 4
        scheduler.OnHeadersProcessed();   // tip sync: nothing missing

        std::vector<std::pair<uint256, uint32_t>> requests;
        scheduler.SetSendGetDataCallback([&requests](const uint256& h, uint32_t height) {
            requests.emplace_back(h, height);
        });

        if (!Require(scheduler.IsFullySynchronized(), "pre: synced at snapshot tip")) {
            return 1;
        }

        scheduler.EnableBackfill(1, 4, hashes[4]);
        scheduler.Tick();
        if (!Require(requests.size() == 4, "expected all 4 backfill bodies requested")) {
            return 1;
        }
        {
            const auto prog = scheduler.GetBackfillProgress();
            if (!Require(prog.total == 4 && prog.in_flight == 4 && prog.completed == 0,
                         "post-request progress must be total=4 in_flight=4 completed=0")) {
                return 1;
            }
        }

        // Deliver each backfill body: OnBlockReceived must return true, progress
        // must increment per body, IsFullySynchronized must stay true throughout,
        // and the body must never enter tip-connect bookkeeping.
        uint64_t delivered = 0;
        for (const auto& rq : requests) {
            if (!Require(scheduler.OnBlockReceived(MakeBlockForHash(selector, rq.first)),
                         "backfill OnBlockReceived must return true (height " +
                             std::to_string(rq.second) + ")")) {
                return 1;
            }
            delivered++;
            const auto prog = scheduler.GetBackfillProgress();
            if (!Require(prog.completed == delivered,
                         "completed must increment per stored body (want " +
                             std::to_string(delivered) + ", got " +
                             std::to_string(prog.completed) + ")")) {
                return 1;
            }
            if (!Require(scheduler.IsFullySynchronized(),
                         "IsFullySynchronized must stay true throughout backfill")) {
                return 1;
            }
            // Store-only contract: no tip-connect bookkeeping for backfill bodies.
            if (!Require(!scheduler.HasReceivedBlock(rq.first),
                         "backfill body must NOT enter received_blocks_ (tip bookkeeping)")) {
                return 1;
            }
            if (!Require(!scheduler.IsBlockExpected(rq.first),
                         "backfill body must NOT enter expected_blocks_ (tip bookkeeping)")) {
                return 1;
            }
            if (!Require(!scheduler.IsBlockInFlight(rq.first),
                         "received backfill body must leave the shared in-flight set")) {
                return 1;
            }
            if (!Require(scheduler.GetQueuedBlockCount() == 0 &&
                             scheduler.GetMissingBlockCount() == 0,
                         "backfill receive must not touch the tip queue")) {
                return 1;
            }
        }

        const auto prog = scheduler.GetBackfillProgress();
        if (!Require(prog.enabled, "progress.enabled stays true after completion")) return 1;
        if (!Require(prog.in_flight == 0, "in_flight must drain to 0 on completion")) return 1;
        if (!Require(prog.total == prog.completed && prog.total == 4,
                     "completion must reach total==completed==4")) {
            return 1;
        }

        std::cout << "   ✅ 4/4 backfill bodies stored-only, accounting completed"
                  << std::endl;
    }

    {
        std::cout << "\n14. assumeutxo backfill: NOTFOUND demotes the peer for backfill "
                     "heights too (#241 shared map) and flips the entry back to MISSING..."
                  << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;  // index == height
        try {
            BuildLinearHeaders(selector, 4, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(4);   // snapshot base = 4; tip sync idle
        scheduler.OnHeadersProcessed();

        const std::string bad_peer = "192.0.2.9:5678";  // RFC 5737 test address

        // Mirror case 8's observation: capture the staged skip-set for every
        // send via CurrentRequestSkipPeers(), keyed by requested height.
        std::vector<std::pair<uint32_t, std::unordered_set<std::string>>> sends;
        scheduler.SetSendGetDataCallback(
            [&scheduler, &sends](const uint256& /*h*/, uint32_t height) {
                sends.emplace_back(height, scheduler.CurrentRequestSkipPeers());
            });

        scheduler.EnableBackfill(1, 4, hashes[4]);
        scheduler.Tick();
        if (!Require(sends.size() == 4, "expected all 4 backfill bodies requested")) {
            return 1;
        }
        for (const auto& s : sends) {
            if (!Require(s.second.empty(),
                         "no peer should be in the skip-set before any NOTFOUND")) {
                return 1;
            }
        }
        if (!Require(scheduler.GetBackfillProgress().in_flight == 4,
                     "setup: 4 backfill requests in flight")) {
            return 1;
        }

        // The snapshot peer NOTFOUNDs the backfill block at height 3: the entry
        // must flip back to MISSING and release the in-flight accounting.
        scheduler.OnBlockNotFound(hashes[3], bad_peer);
        if (!Require(scheduler.GetBackfillProgress().in_flight == 3,
                     "NOTFOUND must release the backfill in-flight slot")) {
            return 1;
        }
        if (!Require(!scheduler.IsBlockInFlight(hashes[3]),
                     "NOTFOUND must clear the shared in-flight entry")) {
            return 1;
        }

        // Force the stale sweep (#216 lineage, now covering backfill_blocks_)
        // so heights 1, 2, 4 also return to MISSING and get re-requested.
        scheduler.SetStaleRequestTimeoutSeconds(0);
        sends.clear();
        scheduler.Tick();

        if (!Require(sends.size() == 4,
                     "stale sweep + NOTFOUND demotion must re-request all 4 backfill "
                     "heights (got " + std::to_string(sends.size()) + ")")) {
            return 1;
        }
        // #241 height-bounded gating over the SHARED demotion map: the peer that
        // NOTFOUND'd height 3 must be skipped for every backfill request at
        // height <= 3 (including the height-2 request) and must NOT be skipped
        // at height 4.
        for (const auto& s : sends) {
            if (s.first <= 3) {
                if (!Require(s.second.count(bad_peer) > 0,
                             "#241: NOTFOUND peer must be in the skip-set for backfill "
                             "height " + std::to_string(s.first))) {
                    return 1;
                }
            } else {
                if (!Require(s.second.count(bad_peer) == 0,
                             "#241: NOTFOUND peer must NOT be skipped above its gap "
                             "(height " + std::to_string(s.first) + ")")) {
                    return 1;
                }
            }
        }
        if (!Require(scheduler.GetBackfillProgress().in_flight == 4,
                     "re-requests must restore in-flight accounting")) {
            return 1;
        }

        std::cout << "   ✅ NOTFOUND peer skipped at backfill heights <=3, retained at 4; "
                     "entry demoted to MISSING and retried" << std::endl;
    }

    {
        std::cout << "\n15. ScanForMissingBlocks preserves backfill in-flight hashes "
                     "across rescans (shared window stays truthful)..." << std::endl;

        // Mirror case 12's topology: genesis + heights 1..40, snapshot base = 20.
        // Tip sync (21..40) is fully drained via the 3-tick sequence, leaving the
        // backfill queue (1..20) with max_in_flight (16) REQUESTED entries and
        // 4 MISSING.  A headers-processed rescan (ScanForMissingBlocks with no
        // new tip work) must NOT wipe the backfill hashes from in_flight_blocks_.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 40, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(20);  // snapshot base = 20

        std::vector<std::pair<uint256, uint32_t>> requests;  // (hash, height)
        scheduler.SetSendGetDataCallback([&requests](const uint256& h, uint32_t height) {
            requests.emplace_back(h, height);
        });

        scheduler.OnHeadersProcessed();   // tip sync: heights 21..40 missing
        scheduler.EnableBackfill(1, 20, hashes[20]);  // pre-base bodies 1..20, anchored on base hash

        // Tick #1: tip sync fills the window (16 REQUESTED) and backfill stages
        // its reserved half-window share (8) on the same tick.
        scheduler.Tick();
        if (!Require(requests.size() == scheduler.GetMaxInFlight() +
                         std::max<size_t>(1, scheduler.GetMaxInFlight() / 2),
                     "case 15 setup: expected tip window + backfill share (tick 1)")) {
            return 1;
        }
        auto first_window = requests;
        requests.clear();
        for (const auto& rq : first_window) {
            if (rq.second <= 20) continue;  // backfill request — stays in flight
            if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, rq.first))) {
                std::cerr << "   ❌ case 15: failed to drain tip block at height "
                          << rq.second << std::endl;
                return 1;
            }
        }

        // Tick #2: the remaining 4 tip blocks REQUESTED (backfill holds its quota).
        scheduler.Tick();
        if (!Require(requests.size() == 4,
                     "case 15 setup: expected 4 remaining tip requests (tick 2)")) {
            return 1;
        }
        auto second_window = requests;
        requests.clear();
        for (const auto& rq : second_window) {
            if (!scheduler.OnBlockReceived(MakeBlockForHash(selector, rq.first))) {
                std::cerr << "   ❌ case 15: failed to drain remaining tip block at height "
                          << rq.second << std::endl;
                return 1;
            }
        }

        // Tick #3: tip idle → backfill tops up to max_in_flight (16 REQUESTED,
        // 4 MISSING remain in the 20-entry backfill queue).
        scheduler.Tick();
        if (!Require(requests.size() == scheduler.GetMaxInFlight() -
                         std::max<size_t>(1, scheduler.GetMaxInFlight() / 2),
                     "case 15 setup: expected backfill to top up the window (tick 3)")) {
            return 1;
        }

        // Capture one in-flight backfill hash for IsBlockInFlight probing.
        const uint256 backfill_hash = requests.front().first;
        const size_t in_flight_before = scheduler.GetInFlightCount();
        if (!Require(in_flight_before == scheduler.GetMaxInFlight(),
                     "case 15 setup: expected full window before rescan")) {
            return 1;
        }

        requests.clear();  // reset send counter for assertion (c)

        // ── The rescan under test ──────────────────────────────────────────────
        // OnHeadersProcessed() triggers ScanForMissingBlocks. No new tip work
        // changes (best_height == 40, local_tip == 20) so the scan re-walks
        // heights 21..40 and rebuilds tip-REQUESTED entries — but the bug path
        // calls in_flight_blocks_.clear() without re-adding backfill hashes.
        scheduler.OnHeadersProcessed();

        // (a) in-flight count must be unchanged: backfill hashes share the window.
        const size_t in_flight_after = scheduler.GetInFlightCount();
        if (!Require(in_flight_after == in_flight_before,
                     "ScanForMissingBlocks must preserve backfill in-flight count "
                     "(before=" + std::to_string(in_flight_before) +
                     " after=" + std::to_string(in_flight_after) + ")")) {
            return 1;
        }

        // (b) the specific backfill hash must still be tracked as in-flight.
        if (!Require(scheduler.IsBlockInFlight(backfill_hash),
                     "ScanForMissingBlocks must preserve IsBlockInFlight for "
                     "a backfill hash that was REQUESTED before the rescan")) {
            return 1;
        }

        // (c) a further Tick must stage NO additional backfill getdata: the cap
        // (in_flight == max_in_flight) blocks all new requests.  Only meaningful
        // because the backfill range (20) exceeds max_in_flight (16), leaving
        // 4 MISSING entries that an undercounting scan would re-stage.
        scheduler.Tick();
        if (!Require(requests.empty(),
                     "Tick after truthful rescan must stage 0 new backfill getdata "
                     "(cap is full); got " + std::to_string(requests.size()))) {
            return 1;
        }

        std::cout << "   ✅ in_flight preserved across rescan (before=" << in_flight_before
                  << " after=" << in_flight_after
                  << "); IsBlockInFlight still true; 0 extra sends after Tick"
                  << std::endl;
    }

    {
        std::cout << "\n16. assumeutxo backfill: re-Enable with a different (range, anchor) "
                     "releases the old queue's in-flight slots (no leak on a direct base "
                     "change), and a refused re-Enable leaves backfill disabled..." << std::endl;

        // Topology: genesis + heights 1..8, snapshot base = 8, tip sync idle.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 8, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(8);

        std::vector<std::pair<uint256, uint32_t>> requests;  // (hash, height)
        scheduler.SetSendGetDataCallback([&requests](const uint256& h, uint32_t height) {
            requests.emplace_back(h, height);
        });

        scheduler.OnHeadersProcessed();  // tip sync: nothing missing
        scheduler.EnableBackfill(1, 8, hashes[8]);
        scheduler.Tick();  // 8 backfill bodies staged + dispatched (< max_in_flight 16)
        if (!Require(requests.size() == 8,
                     "case 16 setup: expected 8 backfill requests on the wire, got " +
                         std::to_string(requests.size()))) {
            return 1;
        }
        if (!Require(scheduler.GetInFlightCount() == 8,
                     "case 16 setup: expected 8 in-flight slots before the base change")) {
            return 1;
        }

        // ── The base change under test ─────────────────────────────────────────
        // A reset + new (shorter) snapshot between two periodic ticks re-Enables
        // with a different (range, anchor) WITHOUT an intervening DisableBackfill.
        // The old 8 REQUESTED hashes are still on the wire; EnableBackfill must
        // perform the Disable cleanup itself or those hashes leak in
        // in_flight_blocks_ forever (the bodies will never be OnBlockReceived-
        // routed once backfill_expected_ is rebuilt), permanently shrinking the
        // shared download window.
        scheduler.EnableBackfill(1, 6, hashes[6]);

        auto prog = scheduler.GetBackfillProgress();
        if (!Require(prog.enabled, "re-Enable with new anchor must enable")) return 1;
        if (!Require(prog.end_height == 6, "re-Enable must adopt the new range")) return 1;
        if (!Require(prog.total == 6, "new queue must cover heights 1..6")) return 1;

        // (a) ZERO leaked slots: the new queue is staged but not yet dispatched,
        // so the shared in-flight window must be completely empty.
        if (!Require(scheduler.GetInFlightCount() == 0,
                     "re-Enable with a different (range, anchor) leaked in-flight slots: "
                     "expected 0, got " + std::to_string(scheduler.GetInFlightCount()))) {
            return 1;
        }
        // (b) Every OLD wire hash must have left the in-flight set — including
        // heights 7..8, which the new range does not even cover.
        for (const auto& rq : requests) {
            if (!Require(!scheduler.IsBlockInFlight(rq.first),
                         "old backfill hash at height " + std::to_string(rq.second) +
                         " still tracked in-flight after the base change")) {
                return 1;
            }
        }

        // (c) The next Tick services exactly the NEW queue (heights 1..6).
        requests.clear();
        scheduler.Tick();
        std::vector<uint32_t> got;
        for (const auto& rq : requests) got.push_back(rq.second);
        std::sort(got.begin(), got.end());
        const std::vector<uint32_t> want{1, 2, 3, 4, 5, 6};
        if (!Require(got == want,
                     "Tick after the base change must request exactly heights 1..6, got " +
                         std::to_string(got.size()) + " requests")) {
            return 1;
        }

        // ── Refused re-Enable leaves backfill DISABLED (carryover 2) ──────────
        // Anchor exists but at the wrong height (caller-bug shape; the unknown-
        // anchor shape behaves identically): the refusal must tear down the
        // active queue and leave enabled == false so the periodic re-arm
        // genuinely retries — and must release the 6 in-flight slots staged
        // above (same no-leak contract on the refusal path).
        scheduler.EnableBackfill(1, 7, hashes[6]);
        if (!Require(!scheduler.GetBackfillProgress().enabled,
                     "refused re-Enable must leave backfill disabled (retry-able)")) {
            return 1;
        }
        if (!Require(scheduler.GetInFlightCount() == 0,
                     "refused re-Enable leaked in-flight slots: expected 0, got " +
                         std::to_string(scheduler.GetInFlightCount()))) {
            return 1;
        }
        // The periodic retry then re-arms cleanly.
        scheduler.EnableBackfill(1, 6, hashes[6]);
        if (!Require(scheduler.GetBackfillProgress().enabled &&
                         scheduler.GetBackfillProgress().total == 6,
                     "periodic re-arm after a refusal must re-enable cleanly")) {
            return 1;
        }

        std::cout << "   ✅ base change released all 8 old in-flight slots; new queue "
                     "serviced 1..6; refused re-Enable left backfill disabled and clean"
                  << std::endl;
    }

    {
        std::cout << "\n17. ScanForMissingBlocks 'Already synchronized' early return "
                     "must preserve backfill in-flight hashes (steady-state snapshot "
                     "node at tip)..." << std::endl;

        // Case 15 covers the FULL-scan path (new tip work re-walks the window
        // and re-adds backfill hashes at the end of the function). This case
        // hits the EARLY-RETURN path instead: local_tip == best header height,
        // so ScanForMissingBlocks clears in_flight_blocks_ and then bails at
        // "Already synchronized" — the NORMAL steady state of every snapshot-
        // loaded node at tip. If the backfill re-add sits after that return,
        // every headers message drops the outstanding backfill hashes from the
        // cap authority and the next Tick stages MORE getdata on top of the 16
        // already on the wire (2x oversubscription class; e2e in_flight=29).
        //
        // Topology: genesis + heights 1..20, snapshot base = 20, tip AT base.
        // Backfill range (20) > max_in_flight (16) so an undercounting scan
        // leaves 4 MISSING entries for the next Tick to over-stage.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 20, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(20);  // tip == best header: synchronized

        std::vector<std::pair<uint256, uint32_t>> requests;  // (hash, height)
        scheduler.SetSendGetDataCallback([&requests](const uint256& h, uint32_t height) {
            requests.emplace_back(h, height);
        });

        scheduler.OnHeadersProcessed();  // takes the "Already synchronized" return
        scheduler.EnableBackfill(1, 20, hashes[20]);  // pre-base bodies 1..20

        // Tick #1: tip idle → backfill fills the window (16 REQUESTED, 4 MISSING).
        scheduler.Tick();
        if (!Require(requests.size() == scheduler.GetMaxInFlight(),
                     "case 17 setup: expected backfill to fill the window, got " +
                         std::to_string(requests.size()))) {
            return 1;
        }
        const uint256 backfill_hash = requests.front().first;
        const size_t in_flight_before = scheduler.GetInFlightCount();
        if (!Require(in_flight_before == scheduler.GetMaxInFlight(),
                     "case 17 setup: expected full window before rescan")) {
            return 1;
        }
        requests.clear();

        // ── The rescan under test ──────────────────────────────────────────────
        // local_tip (20) == best header (20) → start_height > best_height →
        // ScanForMissingBlocks hits the "Already synchronized" early return
        // AFTER clearing in_flight_blocks_.
        scheduler.OnHeadersProcessed();

        // (a) in-flight count must be unchanged: backfill hashes stay in the
        // cap authority even when the scan exits early.
        const size_t in_flight_after = scheduler.GetInFlightCount();
        if (!Require(in_flight_after == in_flight_before,
                     "'Already synchronized' rescan must preserve backfill in-flight "
                     "count (before=" + std::to_string(in_flight_before) +
                     " after=" + std::to_string(in_flight_after) + ")")) {
            return 1;
        }

        // (b) the specific backfill hash must still be tracked as in-flight.
        if (!Require(scheduler.IsBlockInFlight(backfill_hash),
                     "'Already synchronized' rescan must preserve IsBlockInFlight "
                     "for a REQUESTED backfill hash")) {
            return 1;
        }

        // (c) a further Tick must stage ZERO additional getdata: the cap is
        // full (16/16). An undercounting scan would stage the 4 leftover
        // MISSING entries here → 20 outstanding vs cap 16.
        scheduler.Tick();
        if (!Require(requests.empty(),
                     "Tick after 'Already synchronized' rescan must stage 0 new "
                     "getdata (cap is full); got " + std::to_string(requests.size()))) {
            return 1;
        }

        std::cout << "   ✅ in_flight preserved across the early-return rescan (before="
                  << in_flight_before << " after=" << in_flight_after
                  << "); IsBlockInFlight still true; 0 extra sends after Tick"
                  << std::endl;
    }

    {
        std::cout << "\n18. #298 RequestMissingBackfillBodies re-queues pre-base bodies "
                     "validation reports unreadable — including after the one-shot window "
                     "reported itself complete — skips durable ones, and the re-delivery "
                     "round-trips through the backfill path (not the tip path)..."
                  << std::endl;

        // Topology: genesis + heights 1..4, snapshot base = 4, tip sync idle.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;  // index == height
        try {
            BuildLinearHeaders(selector, 4, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(4);   // snapshot base = 4; tip sync idle
        scheduler.OnHeadersProcessed();

        std::vector<std::pair<uint256, uint32_t>> requests;  // (hash, height)
        scheduler.SetSendGetDataCallback([&requests](const uint256& h, uint32_t height) {
            requests.emplace_back(h, height);
        });

        // Wake-on-store counter (#2): must fire once per stored backfill body.
        std::atomic<int> wake_count{0};
        scheduler.SetOnBackfillBodyStored([&wake_count]() { wake_count.fetch_add(1); });

        // Controllable HasBlockBody oracle: a height is "durable" iff present here.
        // Empty during EnableBackfill so the full 1..4 queue is populated.
        std::unordered_set<uint32_t> durable_heights;
        scheduler.SetHasBlockBodyCallback(
            [&durable_heights](const uint256&, uint32_t height) -> bool {
                return durable_heights.count(height) > 0;
            });

        // ── Part A: drive the window to completion (the #298 false-complete) ──
        scheduler.EnableBackfill(1, 4, hashes[4]);
        scheduler.Tick();
        if (!Require(requests.size() == 4, "case 18 setup: expected 4 backfill requests")) {
            return 1;
        }
        for (const auto& rq : requests) {
            if (!Require(scheduler.OnBlockReceived(MakeBlockForHash(selector, rq.first)),
                         "case 18 setup: backfill body delivery must store")) {
                return 1;
            }
        }
        {
            const auto prog = scheduler.GetBackfillProgress();
            if (!Require(prog.completed == 4 && prog.total == 4,
                         "case 18 setup: window must reach completed==total==4")) {
                return 1;
            }
        }
        if (!Require(wake_count.load() == 4,
                     "#298 wake-on-store must fire once per delivered backfill body (got " +
                         std::to_string(wake_count.load()) + ")")) {
            return 1;
        }

        // Validation discovers height 1's body is unreadable. The entry is still
        // RECEIVED in the completed window → found path (RECEIVED→MISSING flip).
        requests.clear();
        if (!Require(scheduler.RequestMissingBackfillBodies({{hashes[1], 1}}) == 1,
                     "#298: an unreadable completed body must be re-queued (found path)")) {
            return 1;
        }
        {
            const auto prog = scheduler.GetBackfillProgress();
            if (!Require(prog.enabled, "#298: re-request keeps backfill enabled")) return 1;
            if (!Require(prog.completed == 3,
                         "#298: RECEIVED→MISSING flip must un-count completed (4→3)")) {
                return 1;
            }
            if (!Require(prog.total == 4, "#298: found path must not grow total")) return 1;
        }

        scheduler.Tick();  // drain must re-issue getdata for the re-queued body
        {
            bool got_h1 = false;
            for (const auto& rq : requests) if (rq.second == 1) got_h1 = true;
            if (!Require(got_h1 && requests.size() == 1,
                         "#298: Tick must re-request exactly the re-queued height-1 body "
                         "(the one-shot window had already gone idle)")) {
                return 1;
            }
        }

        // Re-delivery MUST route through the backfill path again (not the tip
        // path). This is the regression for the RECEIVED entry having erased
        // itself from backfill_expected_ on its first store: without re-arming
        // that routing, the re-delivered block falls through to the tip path,
        // never re-counts completed, pollutes received_blocks_, and loops.
        const int wake_before_redelivery = wake_count.load();
        if (!Require(scheduler.OnBlockReceived(MakeBlockForHash(selector, hashes[1])),
                     "#298: re-delivered backfill body must store")) {
            return 1;
        }
        {
            const auto prog = scheduler.GetBackfillProgress();
            if (!Require(prog.completed == 4,
                         "#298: re-delivery must route to the BACKFILL path (completed 3→4); "
                         "if it routed to the tip path this stays 3")) {
                return 1;
            }
        }
        if (!Require(!scheduler.HasReceivedBlock(hashes[1]),
                     "#298: a re-delivered backfill body must NOT pollute tip "
                     "received_blocks_ (store-only contract preserved)")) {
            return 1;
        }
        if (!Require(wake_count.load() == wake_before_redelivery + 1,
                     "#298 wake-on-store must fire on the re-delivery store too")) {
            return 1;
        }

        // ── Part B: window cleared → emplace path + durable-skip ──────────────
        scheduler.DisableBackfill();
        if (!Require(!scheduler.GetBackfillProgress().enabled, "case 18: disable failed")) {
            return 1;
        }
        durable_heights = {2};  // height 2 is now durably stored

        requests.clear();
        // height 1 unreadable (emplace into a cleared queue), height 2 durable (skip).
        if (!Require(
                scheduler.RequestMissingBackfillBodies({{hashes[1], 1}, {hashes[2], 2}}) == 1,
                "#298: only the unreadable height re-queued; the durable one is skipped")) {
            return 1;
        }
        if (!Require(scheduler.GetBackfillProgress().enabled,
                     "#298: re-arming a cleared window must set enabled=true")) {
            return 1;
        }

        scheduler.Tick();
        {
            bool got_h1 = false, got_h2 = false;
            for (const auto& rq : requests) {
                if (rq.second == 1) got_h1 = true;
                if (rq.second == 2) got_h2 = true;
            }
            if (!Require(got_h1, "#298: emplace path must re-request the unreadable body")) {
                return 1;
            }
            if (!Require(!got_h2, "#298: a durable body must NOT be re-requested")) {
                return 1;
            }
            if (!Require(requests.size() == 1,
                         "#298: exactly one getdata (the unreadable body), got " +
                             std::to_string(requests.size()))) {
                return 1;
            }
        }

        std::cout << "   ✅ unreadable bodies re-queued (found + emplace paths), durable "
                     "skipped, re-delivery round-tripped through backfill, wake fired"
                  << std::endl;
    }

    {
        std::cout << "\nN. zero-recipient getdata must not pin a phantom in-flight slot "
                     "(window-wedge regression)..." << std::endl;

        // Live failure (v8.0.5, CSN wallet): with a lone non-bridge peer present,
        // the daemon callback resolved every getdata to 0 recipients (bridge
        // filter + body-incapable skip-set), yet RequestNextBlock() had already
        // reserved the in-flight slots. max_in_flight (16) such phantoms filled
        // the window and wedged all queued blocks; the tip froze (scheduler
        // in_flight=16, per-peer inflight=0). NotifyGetDataDispatched(h, 0) must
        // release the phantom slot so the window recovers.
        dcs::HeaderChainSelector selector;
        try {
            BuildLinearHeaders(selector, 40);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);

        std::vector<uint256> requested;
        scheduler.SetSendGetDataCallback([&requested](const uint256& h, uint32_t) {
            requested.push_back(h);
        });

        scheduler.OnHeadersProcessed();
        scheduler.Tick();  // fills the in-flight window up to max_in_flight

        const size_t window = scheduler.GetMaxInFlight();
        if (!Require(scheduler.GetInFlightCount() == window,
                     "setup: Tick should fill the window to max_in_flight (" +
                         std::to_string(window) + "), got " +
                         std::to_string(scheduler.GetInFlightCount()))) {
            return 1;
        }
        if (!Require(requested.size() == window,
                     "setup: one getdata staged per in-flight slot")) {
            return 1;
        }

        // Simulate the daemon callback reporting ZERO recipients for every staged
        // getdata — the exact live stall signature.
        for (const auto& h : requested) {
            scheduler.NotifyGetDataDispatched(h, /*recipient_count=*/0);
        }

        // Regression invariant: phantom in-flight must be released, not pin the
        // window. Pre-fix this stayed at `window` and the node never recovered.
        if (!Require(scheduler.GetInFlightCount() == 0,
                     "zero-recipient getdata must release the in-flight slot; in_flight=" +
                         std::to_string(scheduler.GetInFlightCount()) +
                         " (window still wedged)")) {
            return 1;
        }

        // The freed window must be usable again: the next Tick re-issues getdata
        // (with eligible peers, recipients>0 → real progress resumes).
        requested.clear();
        scheduler.Tick();
        if (!Require(!requested.empty(),
                     "after releasing phantoms, Tick must re-issue getdata (window recovered)")) {
            return 1;
        }

        std::cout << "   ✅ released " << window
                  << " phantom in-flight slots from zero-recipient sends; window recovered, "
                     "re-requested="
                  << requested.size() << std::endl;
    }

    {
        std::cout << "\nN+1. stall watchdog clears poisoned skip-set + resets in-flight when "
                     "the tip is frozen with work queued..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 20, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build header chain: " << e.what() << std::endl;
            return 1;
        }
        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(0);
        std::vector<uint256> requested;
        scheduler.SetSendGetDataCallback([&requested](const uint256& h, uint32_t) {
            requested.push_back(h);
        });

        scheduler.OnHeadersProcessed();
        scheduler.Tick();  // initializes the watchdog progress baseline + requests blocks

        // Poison the body-incapable skip-set, as a NOTFOUND from a peer would.
        scheduler.OnBlockNotFound(hashes[5], "peerA");
        if (!Require(scheduler.GetSkipSetSizeForTest() == 1,
                     "setup: skip-set should hold the demoted peer")) {
            return 1;
        }

        // Tip never advances (frozen) while blocks stay queued. Arm a 1s watchdog
        // and let it elapse, then Tick: it must force-recover.
        scheduler.SetStallWatchdogSeconds(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        scheduler.Tick();

        if (!Require(scheduler.GetSkipSetSizeForTest() == 0,
                     "stall watchdog must clear the poisoned skip-set; size=" +
                         std::to_string(scheduler.GetSkipSetSizeForTest()))) {
            return 1;
        }
        std::cout << "   ✅ watchdog cleared skip-set + reset in-flight after frozen-tip stall"
                  << std::endl;
    }

    {
        // ── bug #4 regression: per-queue NOTFOUND demotion ────────────────────
        // A tip-sync NOTFOUND must NOT demote the peer from the AssumeUTXO
        // backfill (pre-base) queue, and vice versa. Before the fix a single
        // shared skip-set let one tip NOTFOUND at a height >= the snapshot base
        // exclude a FULL ARCHIVAL peer from the ENTIRE backfill range — starving
        // backfill of the peers that actually hold every pre-base body (the
        // "no reachable peer holds the genesis..base bodies" symptom).
        std::cout << "\nN+2. bug #4: tip NOTFOUND must not poison the backfill skip-set "
                     "(per-queue separation)..." << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;  // index == height (hashes[0] == genesis)
        try {
            BuildLinearHeaders(selector, 12, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build header chain: " << e.what() << std::endl;
            return 1;
        }
        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        // Snapshot base = 8: tip-sync queue = heights 9..12; pre-base bodies
        // 1..8 are backfilled. No bodies present anywhere.
        scheduler.SetLocalTipHeight(8);
        scheduler.SetSendGetDataCallback([](const uint256&, uint32_t) { return true; });
        scheduler.SetHasBlockBodyCallback(
            [](const uint256&, uint32_t) -> bool { return false; });
        scheduler.OnHeadersProcessed();
        scheduler.Tick();  // stage tip-sync requests for heights 9..12

        scheduler.EnableBackfill(1, 8, hashes[8]);  // pre-base queue, anchored on base
        if (!Require(scheduler.GetBackfillProgress().enabled,
                     "setup: backfill should be enabled")) {
            return 1;
        }

        const std::string archival_peer = "198.51.100.9:9999";  // RFC5737 full node

        // The archival peer NOTFOUNDs a TIP block (height 10) — e.g. a transient
        // during its own restart, or a fork hash it never held. Tip-queue hash.
        scheduler.OnBlockNotFound(hashes[10], archival_peer);

        if (!Require(scheduler.GetSkipSetSizeForTest() == 1,
                     "tip NOTFOUND should record exactly one demotion (tip queue)")) {
            return 1;
        }
        // THE bug #4 invariant: that tip demotion must NOT land in the backfill
        // skip-set. A shared map would put archival_peer here at gap=10, and
        // ServiceBackfillLocked would then skip it for every pre-base height ≤10
        // — i.e. all of 1..8.
        if (!Require(scheduler.GetBackfillSkipSetSizeForTest() == 0,
                     "bug #4: a tip-queue NOTFOUND must NOT demote the peer from backfill; "
                     "backfill skip-set size=" +
                         std::to_string(scheduler.GetBackfillSkipSetSizeForTest()))) {
            return 1;
        }

        // Symmetric direction: a NOTFOUND for a pre-base (backfill) hash lands in
        // the backfill map only, leaving the tip map for that peer unchanged.
        scheduler.OnBlockNotFound(hashes[4], archival_peer);  // height 4 ∈ backfill 1..8
        if (!Require(scheduler.GetBackfillSkipSetSizeForTest() == 1,
                     "backfill NOTFOUND should demote the peer in the backfill skip-set")) {
            return 1;
        }
        // Total across both queues = 2 (one tip demotion + one backfill demotion),
        // proving the two queues track the peer independently.
        if (!Require(scheduler.GetSkipSetSizeForTest() == 2,
                     "tip + backfill demotions must be tracked separately (expected 2); got " +
                         std::to_string(scheduler.GetSkipSetSizeForTest()))) {
            return 1;
        }
        std::cout << "   ✅ tip and backfill NOTFOUND demotions are queue-isolated; a tip "
                     "NOTFOUND no longer starves backfill of archival peers" << std::endl;
    }

    // A backfill request can legitimately take longer than the scheduler's
    // in-flight timeout. Its body is still expected by the backfill queue and
    // must survive the daemon's IsBlockKnown-based unsolicited-block gate.
    {
        std::cout << "\n📋 Section: timed-out backfill reply remains known" << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 40, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(40);  // snapshot base; no forward tip work
        scheduler.OnHeadersProcessed();
        scheduler.SetHasBlockBodyCallback(
            [](const uint256&, uint32_t) -> bool { return false; });

        std::vector<uint32_t> sent_heights;
        scheduler.SetSendGetDataCallback(
            [&sent_heights](const uint256&, uint32_t height) {
                sent_heights.push_back(height);
            });
        scheduler.SetStaleRequestTimeoutSeconds(0);
        scheduler.EnableBackfill(1, 40, hashes[40]);

        scheduler.Tick();  // heights 1..16 become REQUESTED
        if (!Require(scheduler.IsBlockInFlight(hashes[1]),
                     "late-reply setup: height 1 must start in flight")) return 1;

        scheduler.Tick();  // expire 1..16; cursor requests 17..32
        if (!Require(!scheduler.IsBlockInFlight(hashes[1]),
                     "late-reply setup: height 1 must have aged out of in-flight")) return 1;
        if (!Require(scheduler.IsBlockKnown(hashes[1]),
                     "timed-out backfill body must remain known while backfill expects it")) {
            return 1;
        }
        if (!Require(!scheduler.IsBlockKnown(hashes[0]),
                     "an unrelated, never-queued body must remain unknown")) return 1;

        if (!Require(scheduler.OnBlockReceived(
                         MakeBlockForHash(selector, hashes[1])),
                     "late backfill reply must still be accepted and stored")) return 1;
        if (!Require(scheduler.GetBackfillProgress().completed == 1,
                     "late backfill reply must advance completed")) return 1;
        if (!Require(!scheduler.HasReceivedBlock(hashes[1]),
                     "late backfill reply must stay out of tip receive bookkeeping")) return 1;
        if (!Require(!scheduler.IsBlockKnown(hashes[1]),
                     "consumed backfill body must leave the expected set")) return 1;

        // A validation scan can race the post-store ChainDB locator callback
        // and report the just-received body missing once. The frontier setter
        // must not demote/un-count that RECEIVED entry or re-arm a duplicate.
        scheduler.SetBackfillValidationFrontier(hashes[1], 1);
        if (!Require(scheduler.GetBackfillProgress().completed == 1,
                     "stale validation report must not un-count a RECEIVED body")) return 1;
        if (!Require(!scheduler.IsBlockKnown(hashes[1]),
                     "stale validation report must not re-arm a RECEIVED body")) return 1;

        std::cout << "   ✅ timed-out body stayed known, was consumed late, and advanced "
                     "backfill without polluting tip state" << std::endl;
    }

    // Validation's earliest gap gets one retry lane without stopping bulk
    // traversal or duplicating an already-active request.
    {
        std::cout << "\n📋 Section: validation-frontier lane retries behind-cursor gap"
                  << std::endl;

        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 48, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(48);
        scheduler.OnHeadersProcessed();
        scheduler.SetHasBlockBodyCallback(
            [](const uint256&, uint32_t) -> bool { return false; });

        std::vector<uint32_t> sent;
        scheduler.SetSendGetDataCallback(
            [&sent](const uint256&, uint32_t height) { sent.push_back(height); });
        scheduler.SetStaleRequestTimeoutSeconds(0);
        scheduler.EnableBackfill(1, 48, hashes[48]);

        scheduler.Tick();  // 1..16
        sent.clear();
        scheduler.Tick();  // expire 1..16, advance bulk cursor to 17..32
        if (!Require(!scheduler.IsBlockInFlight(hashes[1]),
                     "frontier setup: height 1 must be MISSING behind the cursor")) return 1;

        scheduler.SetBackfillValidationFrontier(hashes[1], 1);
        sent.clear();
        scheduler.Tick();  // priority h1 + bulk 33..47
        const size_t h1_count = static_cast<size_t>(
            std::count(sent.begin(), sent.end(), uint32_t{1}));
        const bool bulk_advanced =
            std::any_of(sent.begin(), sent.end(), [](uint32_t h) { return h >= 33; });
        if (!Require(h1_count == 1,
                     "validation frontier must be staged exactly once ahead of cursor")) return 1;
        if (!Require(bulk_advanced,
                     "frontier lane must leave capacity for bulk cursor progress")) return 1;
        if (!Require(scheduler.GetBackfillProgress().in_flight <= 16,
                     "frontier lane must stay inside the backfill quota")) return 1;

        // Re-reporting an active frontier is idempotent: no status reset, no
        // duplicate getdata, and no accounting change.
        scheduler.SetStaleRequestTimeoutSeconds(3600);
        const auto before = scheduler.GetBackfillProgress().in_flight;
        scheduler.SetBackfillValidationFrontier(hashes[1], 1);
        sent.clear();
        scheduler.Tick();
        if (!Require(sent.empty(),
                     "already-REQUESTED frontier must not be duplicated")) return 1;
        if (!Require(scheduler.GetBackfillProgress().in_flight == before,
                     "idempotent frontier update must preserve in-flight accounting")) return 1;

        // Once stale, the same frontier is immediately retried while bulk work
        // still advances; it never waits for a full vector wrap.
        scheduler.SetStaleRequestTimeoutSeconds(0);
        sent.clear();
        scheduler.Tick();
        if (!Require(std::count(sent.begin(), sent.end(), uint32_t{1}) == 1,
                     "stale validation frontier must retry immediately")) return 1;
        if (!Require(std::any_of(sent.begin(), sent.end(),
                                 [](uint32_t h) { return h != 1; }),
                     "frontier retry must not starve ordinary backfill")) return 1;

        std::cout << "   ✅ one frontier slot retried immediately; remaining slots kept "
                     "bulk traversal moving" << std::endl;
    }

    // ────────────────────────────────────────────────────────────────────
    // #375: OnBackfillBodyReceived — consume-if-expected, side-effect-free
    // otherwise. The CSN OnUtxoBlock stale-guard must be able to offer any
    // below-cursor utxoblk to this API before dropping it; pre-#375 the guard
    // dropped 100% of backfill bodies (DineroTX e2e: completed=0/52287).
    // ────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n📋 Section: #375 backfill body consume-if-expected" << std::endl;
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 8, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }
        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetLocalTipHeight(8);   // snapshot base = 8
        scheduler.OnHeadersProcessed();
        scheduler.SetHasBlockBodyCallback([](const uint256&, uint32_t height) -> bool {
            return height == 3;           // height 3 body already present
        });
        scheduler.EnableBackfill(1, 8, hashes[8]);
        if (!Require(scheduler.GetBackfillProgress().total == 7,
                     "expected 7 missing backfill bodies (all but height 3)")) return 1;

        // Expected backfill body → consumed (stored, completed++).
        Block b4 = MakeBlockForHash(selector, hashes[4]);
        if (!Require(scheduler.OnBackfillBodyReceived(b4),
                     "expected backfill body must be consumed")) return 1;
        if (!Require(scheduler.GetBackfillProgress().completed == 1,
                     "completed must be 1 after consuming the body")) return 1;

        // Duplicate of an already-consumed body → NOT consumed, no change.
        if (!Require(!scheduler.OnBackfillBodyReceived(b4),
                     "duplicate backfill body must not be consumed")) return 1;
        if (!Require(scheduler.GetBackfillProgress().completed == 1,
                     "duplicate must not advance completed")) return 1;

        // Never-expected body (height 3 was skipped as already-present) →
        // NOT consumed and NO side effects (this is the stale-duplicate case
        // the CSN guard then drops).
        Block b3 = MakeBlockForHash(selector, hashes[3]);
        if (!Require(!scheduler.OnBackfillBodyReceived(b3),
                     "unexpected body must not be consumed")) return 1;
        if (!Require(scheduler.GetBackfillProgress().completed == 1,
                     "unexpected body must not advance completed")) return 1;

        std::cout << "   ✅ OnBackfillBodyReceived consumes exactly the expected bodies, "
                     "side-effect-free otherwise" << std::endl;
    }

    // ────────────────────────────────────────────────────────────────────
    // #378: an INVALID stateless frontier must halt TIP requests only —
    // never the independent pre-base backfill queue. Run 4 on DineroTX
    // wedged backfill at 34,630/52,287 because the frontier halt returned
    // out of the whole tick before ServiceBackfillLocked.
    // ────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n📋 Section: #378 frontier halt must not kill backfill" << std::endl;
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;
        try {
            BuildLinearHeaders(selector, 12, &hashes);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ header build failed: " << e.what() << std::endl;
            return 1;
        }
        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetStatelessMode(true);
        scheduler.SetGetTipHeightCallback([]() -> uint32_t { return 8; });
        scheduler.SetLocalTipHeight(8);          // snapshot base = 8; tip queue = 9..12
        scheduler.OnHeadersProcessed();

        std::vector<uint32_t> sent_heights;
        scheduler.SetSendGetDataCallback([&](const uint256& /*hash*/, uint32_t height) {
            sent_heights.push_back(height);
        });
        scheduler.SetHasBlockBodyCallback([](const uint256&, uint32_t) { return false; });
        scheduler.EnableBackfill(1, 8, hashes[8]);
        if (!Require(scheduler.GetBackfillProgress().enabled, "backfill armed")) return 1;

        // Poison the stateless frontier (worker gave up on height 9 — the
        // run-4 shape: forward frontier INVALID while backfill is healthy).
        if (!Require(scheduler.MarkBlockInvalid(hashes[9]),
                     "frontier block must be markable INVALID")) return 1;

        sent_heights.clear();
        scheduler.Tick();

        bool any_backfill = false;
        bool any_tip = false;
        for (uint32_t h : sent_heights) {
            if (h <= 8) any_backfill = true;
            if (h >= 9) any_tip = true;
        }
        if (!Require(any_backfill,
                     "backfill getdata must still dispatch while the frontier is INVALID")) {
            return 1;
        }
        if (!Require(!any_tip,
                     "tip requests must stay halted while the frontier is INVALID")) {
            return 1;
        }
        std::cout << "   ✅ INVALID frontier halts tip requests only; backfill keeps flowing"
                  << std::endl;
    }

    // ────────────────────────────────────────────────────────────────────
    // #371: DrainFailureStreak — persistent TEMPORARY_FAIL escalation.
    // The EU1 zombie retried "next tick" 17,979+ times against a latched
    // rocksdb error with zero escalation. The streak must fire exactly once
    // per stuck height, reset on height change and on progress.
    // ────────────────────────────────────────────────────────────────────
    {
        std::cout << "\n📋 Section: #371 drain-failure streak escalation" << std::endl;
        using dinero::consensus::DrainFailureStreak;
        DrainFailureStreak streak;
        const int T = DrainFailureStreak::kEscalationThreshold;

        bool early_fired = false;
        for (int i = 0; i < T - 1; ++i) {
            early_fired = streak.RecordFailure(57480) || early_fired;
        }
        if (!Require(!early_fired, "streak must NOT escalate below the threshold")) return 1;
        if (!Require(streak.RecordFailure(57480),
                     "streak must escalate exactly at the threshold")) return 1;
        if (!Require(!streak.RecordFailure(57480),
                     "streak must not re-fire while the same height stays stuck")) return 1;

        // A different height starts a fresh streak (log-once is per height).
        if (!Require(!streak.RecordFailure(57481),
                     "new height must start a fresh streak (no immediate fire)")) return 1;
        for (int i = 0; i < T - 2; ++i) (void)streak.RecordFailure(57481);
        if (!Require(streak.RecordFailure(57481),
                     "fresh streak must escalate at its own threshold")) return 1;

        // Progress clears the streak entirely.
        streak.RecordProgress();
        if (!Require(!streak.RecordFailure(57481),
                     "after progress, one failure must not escalate")) return 1;
        for (int i = 0; i < T - 2; ++i) (void)streak.RecordFailure(57481);
        if (!Require(streak.RecordFailure(57481),
                     "post-progress streak must need a full threshold again")) return 1;

        std::cout << "   ✅ drain-failure streak escalates once per stuck height and resets "
                     "on height change / progress" << std::endl;
    }

    {
        std::cout << "\n#579: reorg-superseded INVALID frontier re-seats to the new branch..."
                  << std::endl;
        // Regression for #579 (CSN permanent-desync wedge, found by the TSan
        // forest-lock stress run). A reorg that invalidates the block at the CSN's
        // frontier height leaves a stale old-branch entry — marked INVALID after
        // proof-retry exhaustion — at that height. FindStatelessFrontierLocked
        // returns it as the frontier, and the pre-fix halt at that check stopped
        // ALL tip requests, including the new-branch block that is now the best
        // chain at that height. The CSN wedged forever (ABC saw the better header
        // chain but deferred body fetches to this halted scheduler). The fix
        // re-seats the entry to the new-branch hash so TIP requests resume.
        //
        // Exact-frontier boundary — the case that actually wedged (frontier ==
        // invalidated height); a fork BEHIND the frontier never engaged the halt.
        //   Main:  g -> 1 -> 2 -> M3           (M3 = old block at height 3)
        //   Fork:  g -> 1 -> 2 -> X3 -> Y4     (X3 = new best-chain block at h3)
        // CSN local/active tip = 2; frontier height = 3.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> main_hashes;   // [g, 1, 2, M3]
        std::vector<uint256> fork_hashes;   // [X3, Y4]
        try {
            BuildLinearHeaders(selector, 3, &main_hashes, 8'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build main header chain: " << e.what() << std::endl;
            return 1;
        }

        dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
        scheduler.SetStatelessMode(true);
        scheduler.SetLocalTipHeight(2);
        scheduler.SetGetTipHeightCallback([]() -> uint32_t { return 2; });

        std::vector<uint256> requested;
        scheduler.SetSendGetDataCallback(
            [&requested](const uint256& h, uint32_t /*height*/) { requested.push_back(h); });

        // Active chain = main up to the CSN's connected tip (height 2). Heights
        // above 2 are not on the active chain (the CSN is stuck at 2).
        auto active_chain = BuildActiveChainIndex({main_hashes[0], main_hashes[1], main_hashes[2]});
        scheduler.SetGetBlockHashAtHeightCallback(
            [&active_chain](uint32_t height, uint256& out_hash) -> bool {
                return dcs::GetActiveChainHashAtHeight(active_chain.back().get(), height, out_hash);
            });

        // 1) Queue + request the old frontier block M3 at height 3.
        scheduler.OnHeadersProcessed();
        scheduler.Tick();
        if (!Require(!requested.empty() && requested.back() == main_hashes[3],
                     "expected the old frontier block M3 to be requested first")) {
            return 1;
        }

        // 2) M3's proof fails (the reorg invalidated it) -> marked INVALID.
        if (!Require(scheduler.MarkBlockInvalid(main_hashes[3]),
                     "expected M3 to be queued so it can be marked INVALID")) {
            return 1;
        }

        // 3) The reorg: a fork at height 2 introduces X3(3), Y4(4). Y4 outweighs
        //    M3, so the best chain's block at height 3 is now X3 (!= M3).
        try {
            AppendForkHeaders(selector, main_hashes[2], 2, &fork_hashes, 9'000'000);
        } catch (const std::exception& e) {
            std::cerr << "   ❌ failed to build fork header chain: " << e.what() << std::endl;
            return 1;
        }
        const auto header_at_3 = selector.GetHeaderAtHeightValue(3);
        if (!Require(header_at_3.has_value() && header_at_3->hash == fork_hashes[0],
                     "expected best-chain header at height 3 to be the fork block X3")) {
            return 1;
        }

        // 4) The wedge tick. Pre-fix: FindStatelessFrontierLocked returns the
        //    INVALID M3 and the halt stops all TIP requests -> X3 never requested.
        //    With the fix: M3's slot is re-seated to X3 and requested THIS tick.
        const size_t before = requested.size();
        scheduler.Tick();
        bool requested_x3 = false;
        for (size_t i = before; i < requested.size(); ++i) {
            if (requested[i] == fork_hashes[0]) { requested_x3 = true; break; }
        }
        if (!Require(requested_x3,
                     "REGRESSION #579: a reorg-superseded INVALID frontier must re-seat to the "
                     "new-branch block and resume TIP requests (pre-fix: halted permanently)")) {
            return 1;
        }
        std::cout << "   ✅ re-seated INVALID M3 -> requested new-branch X3="
                  << fork_hashes[0].GetHex().substr(0, 16) << "..." << std::endl;
    }

    std::cout << "\n✅ All BlockDownloadScheduler regression tests passed" << std::endl;
    return 0;
}
