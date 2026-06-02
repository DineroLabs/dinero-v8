/**
 * Phase N.4: BlockDownloadScheduler regression tests
 *
 * Covers the CSN deadlock class where external backpressure saturates the
 * scheduler window and previously prevented any new requests from being issued.
 */

#include "consensus/active_chain_ancestry.h"
#include "consensus/block_download_scheduler.h"
#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
    const auto* entry = selector.GetHeader(hash);
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

        const auto* best_header = selector.GetBestHeader();
        if (!Require(best_header != nullptr, "expected best header after fork build")) {
            return 1;
        }
        if (!Require(best_header->height == 5, "expected fork tip to become best header")) {
            return 1;
        }

        const auto* best_at_tip_height = selector.GetHeaderAtHeight(3);
        if (!Require(best_at_tip_height != nullptr, "expected best-chain header at local tip height")) {
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
        const auto* best_header = selector.GetBestHeader();
        if (!Require(best_header != nullptr && best_header->height == 4,
                     "expected fork tip Y at height 4 as best header")) {
            return 1;
        }

        // The best header chain at height 3 should be X (fork), not C (main).
        const auto* header_at_3 = selector.GetHeaderAtHeight(3);
        if (!Require(header_at_3 != nullptr && header_at_3->hash == fork_hashes[0],
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
        std::cout << "\n6. stateless reorg barrier fetches replacement ancestor before descendants..." << std::endl;

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

        if (!Require(requested_hashes.size() == 1,
                     "stateless reorg barrier must request only the replacement ancestor first")) {
            return 1;
        }
        if (!Require(requested_hashes.front() == fork_hashes[0],
                     "expected first stateless reorg request to be replacement block at current tip height")) {
            return 1;
        }

        // Simulate the competing block at height 3 becoming the active-chain
        // replacement. The scheduler should then release the barrier and
        // continue with the next descendant at height 4.
        fork_replacement_active = true;
        const size_t requests_before_release = requested_hashes.size();
        scheduler.Tick();

        if (!Require(requested_hashes.size() > requests_before_release,
                     "expected additional requests after replacement ancestor became active")) {
            return 1;
        }
        if (!Require(requested_hashes[requests_before_release] == fork_hashes[1],
                     "expected first descendant request after reorg activation to be height 4")) {
            return 1;
        }

        std::cout << "   ✅ replacement_first=" << requested_hashes.front().GetHex().substr(0, 16)
                  << "... descendant_after_activation="
                  << requested_hashes[requests_before_release].GetHex().substr(0, 16)
                  << "..." << std::endl;
    }

    std::cout << "\n✅ All BlockDownloadScheduler regression tests passed" << std::endl;
    return 0;
}
