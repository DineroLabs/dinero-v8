// #811: invoke the real ActivateBestChain / ConnectTip / GetBestCandidate path.
// A header-only gap shortens the activation target, but operational retry
// bookkeeping must still refer to the original queued candidate.
#include "daemon/services/chainstate_service.h"
#include "daemon/config.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "storage/chain_write_token.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace dinero {
struct ActivationRetryTestAccess {
    static void Prepare(ChainstateService& svc, CBlockIndex* active, ChainDB& db) {
        svc.active_tip_ = active;
        svc.consensus_utxo_set_ = std::make_unique<consensus::ConsensusUTXOSet>();
        svc.consensus_utxo_set_->SetBestBlock(active->hash, active->height);
        // Keep a real tracker, but make the immediate retry check independent
        // of machine speed. Test expiry against its explicit clock argument.
        svc.activation_retries_ = consensus::ActivationRetryTracker(
            std::chrono::hours(1), std::chrono::hours(2));
        const auto state = svc.CurrentShieldedStateSnapshot();
        ChainDB::ShieldedTipMarker marker;
        marker.height = active->height;
        marker.block_hash = active->hash;
        marker.shielded_root = state.root;
        marker.tree_size = state.tree_size;
        marker.nullifier_count = state.nullifier_count;
        const auto token = ChainWriteToken::CreateForTesting();
        if (db.putShieldedTipMarker(token, marker) != Status::Ok)
            throw std::runtime_error("cannot stage aligned shielded marker");
    }
    static uint32_t Failures(const ChainstateService& svc, const uint256& hash) {
        return svc.activation_retries_.FailureCount(hash);
    }
    static void CapAtSnapshotBase(ChainstateService& svc, CBlockIndex* base) {
        svc.assumeutxo_active_ = true;
        svc.assumeutxo_base_height_ = base->height;
        svc.assumeutxo_base_block_ = base->hash;
    }
    static bool Ready(const ChainstateService& svc, const uint256& hash,
                      consensus::ActivationRetryTracker::TimePoint when) {
        return svc.activation_retries_.IsReady(hash, when);
    }
    static CBlockIndex* Best(ChainstateService& svc) { return svc.GetBestCandidate(); }
    static bool Contains(const ChainstateService& svc, CBlockIndex* candidate) {
        const auto entries = svc.candidates_.Snapshot();
        return std::find(entries.begin(), entries.end(), candidate) != entries.end();
    }
};
} // namespace dinero

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::cout << "[PASS] " << message << '\n';
}

void RunCase(const std::filesystem::path& root, bool first_body, bool middle_gap,
             bool snapshot_cap = false) {
    using namespace dinero;
    ChainDB db;
    Require(db.init(root / "chaindb") == Status::Ok, "fresh ChainDB opens");
    std::vector<CBlockIndex*> chain;
    // Use the real index insertion: these header-only descendants acquire
    // BLOCK_VALID_CHAIN through the same propagation as headers-first sync.
    for (uint32_t height = 0; height <= 4; ++height) {
        BlockHeader header;
        header.version = 1;
        header.timestamp = 1000000 + height;
        header.difficulty = 0x207fffff;
        header.nonce = height + 1;
        if (!chain.empty()) header.prev_block_hash = chain.back()->hash;
        auto* entry = AddBlockIndex(header, height);
        Require(entry != nullptr, "real index entry created");
        chain.push_back(entry);
    }
    // Active tip 1, available prefix 2, missing ancestor 3, queued tip 4.
    chain[0]->status |= BLOCK_HAVE_DATA;
    chain[1]->status |= BLOCK_HAVE_DATA;
    chain[4]->status |= BLOCK_HAVE_DATA;
    if (first_body) chain[2]->status |= BLOCK_HAVE_DATA;
    if (!middle_gap) chain[3]->status |= BLOCK_HAVE_DATA;
    Require((chain[3]->status & BLOCK_VALID_CHAIN) != 0,
            "header-only ancestor carries the production CHAIN shortcut");

    const auto token = ChainWriteToken::CreateForTesting();
    Require(db.setTip(token, chain[1]->hash, 1, arith_uint256()) == Status::Ok,
            "database tip staged");
    {
        ChainstateService svc;
        svc.setChainDB(&db);
        ActivationRetryTestAccess::Prepare(svc, chain[1], db);
        if (snapshot_cap) ActivationRetryTestAccess::CapAtSnapshotBase(svc, chain[2]);
        svc.AddCandidate(chain[4]);
        Require(ActivationRetryTestAccess::Best(svc) == chain[4],
                "queued higher-work candidate is initially selected");
        // A missing BlockValidator is an existing operational failure at the
        // start of ConnectTip (height 2 is beyond its early-init exception).
        // No consensus checks are stubbed or weakened for this reproduction.
        svc.ActivateBestChain();
        const uint32_t expected = first_body ? 1 : 0;
        Require(ActivationRetryTestAccess::Failures(svc, chain[4]->hash) == expected,
                "operational failure cools the queued candidate, not a shortened target");
        Require(ActivationRetryTestAccess::Failures(svc, chain[2]->hash) == 0,
                "prefix tip does not receive the queued candidate cooldown");
        Require(svc.GetActiveTip() == chain[1], "failed or deferred activation preserves active tip");
        Require(ActivationRetryTestAccess::Contains(svc, chain[4]),
                "deferred candidate remains queued");
        if (first_body) {
            Require(ActivationRetryTestAccess::Best(svc) != chain[4],
                    "production candidate selection respects the cooldown");
            Require(ActivationRetryTestAccess::Ready(
                        svc, chain[4]->hash,
                        consensus::ActivationRetryTracker::Clock::now() + std::chrono::hours(3)),
                    "candidate becomes eligible after the bounded cooldown");
        }
        for (int repeat = 0; repeat < 3; ++repeat) svc.ActivateBestChain();
        Require(ActivationRetryTestAccess::Failures(svc, chain[4]->hash) == expected,
                "repeated real activation ticks do not retry the operational failure");
        Require(ActivationRetryTestAccess::Failures(svc, chain[2]->hash) == 0,
                "repeated ticks never assign retry state to the prefix");
    }
    // Every service has released its raw index pointers before clearing globals.
    g_candidates.clear();
    g_orphan_pool.clear();
    g_block_index.clear();
    InvalidateAncestryCache();
}
} // namespace

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("dinero-forward-activation-retry-" + std::to_string(unique));
    try {
        dinero::SelectParams(dinero::Chain::REGTEST);
        GetConfig().utreexo_stateless = false;
        GetConfig().assumeutxo_forward_connect = false;
        RunCase(root / "prefix-failure", true, true);
        RunCase(root / "first-body-gap", false, true);
        RunCase(root / "whole-path-failure", true, false);
        RunCase(root / "snapshot-cap-failure", true, false, true);
        std::filesystem::remove_all(root);
        std::cout << "ALL PASSED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
