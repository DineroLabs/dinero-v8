// Serialized slow-peer coverage only. Original scheduler main is not executed.
#include "consensus/active_chain_ancestry.h"
#include "consensus/block_download_scheduler.h"
#include "consensus/drain_failure_streak.h"
#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "storage/block_storage.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <map>
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
    dinero::SelectParams(dinero::Chain::REGTEST);
    {
        std::cout << "\n24. a peer whose block requests keep timing out is skipped for a "
                     "cooldown, and restored when it delivers..." << std::endl;

        // TX resync, 2026-10-03: one slow peer (a desktop node on a home uplink)
        // kept being handed tip getdata and let 42 of them expire against 1 block
        // delivered, while capable fleet peers sat idle.
        const std::string slow = "192.0.2.9:20999";  // RFC 5737 test address
        auto run = [&](int chain_len, auto&& body) -> int {
            dcs::HeaderChainSelector selector;
            std::vector<uint256> hashes;
            try {
                BuildLinearHeaders(selector, chain_len, &hashes);
            } catch (const std::exception& e) {
                std::cerr << "   ❌ failed to build header chain: " << e.what() << std::endl;
                return 1;
            }
            dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
            scheduler.SetLocalTipHeight(0);
            scheduler.SetTipRetryTimeout(std::chrono::hours(1));
            std::vector<std::unordered_set<std::string>> skips;
            scheduler.SetSendGetDataCallback(
                [&scheduler, &skips, &slow](const uint256& h, uint32_t /*height*/) {
                    skips.push_back(scheduler.CurrentRequestSkipPeers());
                    scheduler.NotifyGetDataDispatched(h, 1, slow);
                    return true;
                });
            scheduler.OnHeadersProcessed();
            for (int t = 0; t < 10; ++t) scheduler.Tick();
            if (!Require(!skips.empty(), "setup: blocks should be requested")) return 1;
            for (const auto& s : skips) {
                if (!Require(s.empty(), "no peer is skipped before any timeout")) return 1;
            }
            return body(scheduler, selector, hashes, skips);
        };
        auto expire_all = [](dcs::BlockDownloadScheduler& scheduler) {
            scheduler.SetStaleRequestTimeoutSeconds(0);  // zeroes the tip retry too
            scheduler.Tick();
            scheduler.SetStaleRequestTimeoutSeconds(3600);
            scheduler.SetTipRetryTimeout(std::chrono::hours(1));
        };
        auto all_skip = [&](const std::vector<std::unordered_set<std::string>>& skips,
                            bool expect) {
            if (skips.empty()) return false;
            for (const auto& s : skips) {
                if ((s.count(slow) > 0) != expect) return false;
            }
            return true;
        };

        // One miss is not enough: a single slow reply must not demote a peer.
        if (run(1, [&](auto& scheduler, auto&, auto&, auto& skips) {
                skips.clear();
                expire_all(scheduler);
                for (int t = 0; t < 5; ++t) scheduler.Tick();
                return Require(all_skip(skips, false),
                               "one timeout must not put the peer in the skip-set") ? 0 : 1;
            })) return 1;

        // Repeated misses demote; a block it delivers restores it.
        if (run(6, [&](auto& scheduler, auto& selector, auto& hashes, auto& skips) {
                skips.clear();
                expire_all(scheduler);  // all six requests to `slow` expire
                for (int t = 0; t < 5; ++t) scheduler.Tick();
                if (!Require(all_skip(skips, true),
                             "a peer whose requests keep expiring must be skipped")) return 1;

                // A slow peer still trickles in the odd late block (TX: 10
                // delivered against 36 expired). One delivery must not wipe six
                // misses, or the peer is handed a fresh batch to let expire.
                if (!Require(scheduler.OnBlockReceived(MakeBlockForHash(selector, hashes[1])),
                             "slow peer delivers block 1")) return 1;
                skips.clear();
                if (!Require(scheduler.ReRequestBlock(hashes[4]), "re-queue block 4")) return 1;
                for (int t = 0; t < 5; ++t) scheduler.Tick();
                if (!Require(all_skip(skips, true),
                             "one delivery must not cancel repeated misses")) return 1;

                // Deliveries that outweigh the misses restore it (6 misses - 5 = 1,
                // below the demotion threshold).
                for (int h : {2, 3, 4, 5}) {
                    if (!Require(scheduler.OnBlockReceived(MakeBlockForHash(selector, hashes[h])),
                                 "slow peer delivers block " + std::to_string(h))) return 1;
                }
                skips.clear();
                if (!Require(scheduler.ReRequestBlock(hashes[6]), "re-queue block 6")) return 1;
                for (int t = 0; t < 5; ++t) scheduler.Tick();
                return Require(all_skip(skips, false),
                               "deliveries that outweigh the misses must restore the peer") ? 0 : 1;
            })) return 1;

        // The cooldown lapses on its own.
        if (run(6, [&](auto& scheduler, auto&, auto& hashes, auto& skips) {
                expire_all(scheduler);
                scheduler.SetSlowPeerCooldown(std::chrono::milliseconds(0));
                skips.clear();
                if (!Require(scheduler.ReRequestBlock(hashes[2]), "re-queue block 2")) return 1;
                for (int t = 0; t < 5; ++t) scheduler.Tick();
                return Require(all_skip(skips, false),
                               "a demotion must end after the cooldown") ? 0 : 1;
            })) return 1;
        std::cout << "   ✅ slow peer demoted on repeated timeouts, restored by deliveries or cooldown"
                  << std::endl;
    }


    {
        // Replaying one stored response must not erase the peer's other misses.
        dcs::HeaderChainSelector selector;
        std::vector<uint256> hashes;BuildLinearHeaders(selector,6,&hashes);
        dcs::BlockDownloadScheduler scheduler(&selector,nullptr);
        scheduler.SetLocalTipHeight(0);
        scheduler.SetTipRetryTimeout(std::chrono::hours(1));
        scheduler.SetSlowPeerCooldown(std::chrono::hours(1));
        const std::string slow="192.0.2.9:20999";
        std::vector<std::unordered_set<std::string>> skips;
        scheduler.SetSendGetDataCallback([&](const uint256& hash,uint32_t) {
            skips.push_back(scheduler.CurrentRequestSkipPeers());
            scheduler.NotifyGetDataDispatched(hash,1,slow);return true;
        });
        scheduler.OnHeadersProcessed();scheduler.Tick();
        scheduler.SetStaleRequestTimeoutSeconds(0);scheduler.Tick();
        scheduler.SetStaleRequestTimeoutSeconds(3600);
        scheduler.SetTipRetryTimeout(std::chrono::hours(1));scheduler.Tick();
        const auto body=MakeBlockForHash(selector,hashes[1]);
        if(!Require(scheduler.OnBlockReceived(body),"first legacy receipt stored"))return 1;
        for(unsigned i=0;i<8;++i)
            if(!Require(scheduler.OnBlockReceived(body),"duplicate legacy receipt remains stored"))return 1;
        skips.clear();
        if(!Require(scheduler.ReRequestBlock(hashes[6]),"unrelated request can be retried"))return 1;
        scheduler.Tick();
        if(!Require(!skips.empty(),"unrelated request was dispatched"))return 1;
        for(const auto& skip:skips)
            if(!Require(skip.count(slow)>0,"duplicate legacy receipts must not cancel other misses"))return 1;
        std::cout<<"[PASS] LegacyDuplicateReceiptKeepsPenalty\n";
    }
    return 0;
}
