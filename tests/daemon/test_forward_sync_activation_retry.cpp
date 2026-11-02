// #811: invoke the real ActivateBestChain / ConnectTip / GetBestCandidate path.
// A header-only gap shortens the activation target, but operational retry
// bookkeeping must still refer to the original queued candidate.
#include "daemon/services/chainstate_service.h"
#include "daemon/config.h"
#include "consensus/block_lifecycle.h"
#include "consensus/candidate_ancestry.h"
#include "consensus/chainparams.h"
#include "consensus/block_validation.h"
#include "consensus/merkle_root.h"
#include "consensus/witness_commitment.h"
#include "consensus/sigops.h"
#include "consensus/filter_commitment.h"
#include "consensus/block_download_scheduler.h"
#include "consensus/header_chain.h"
#include "daemon/daemon_context.h"
#include "mining/block_assembler.h"
#include "consensus/state_commitment.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/block_filter.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/script_verify.h"
#include "consensus/covenant_activation.h"
#include "consensus/csn_replay_data.h"
#include "storage/chain_write_token.h"
#include "storage/block_storage.h"
#include "network/stateless_node.h"
#include "network/bridge_node.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <iomanip>
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
    static uint256 ShieldedRoot(const ChainstateService& svc) {
        return svc.CurrentShieldedStateSnapshot().root;
    }
    static std::optional<uint256> ForkShieldedRoot(const ChainstateService& svc) {
        return consensus::shielded::ComputeShieldedRoot(
            svc.shielded_tree_, svc.shielded_nullifiers_,
            svc.shielded_anchor_history_);
    }
    static void ClearFailure(ChainstateService& svc, const uint256& hash) {
        svc.activation_retries_.Clear(hash);
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
    static void PrepareCandidateCost(ChainstateService& svc, CBlockIndex* active,
                                     const std::vector<std::unique_ptr<CBlockIndex>>& chain) {
        svc.active_tip_ = active;
        for (const auto& node : chain) svc.candidates_.insert(node.get());
    }

    static bool Contains(const ChainstateService& svc, CBlockIndex* candidate) {
        const auto entries = svc.candidates_.Snapshot();
        return std::find(entries.begin(), entries.end(), candidate) != entries.end();
    }
    static void PrepareConnect(ChainstateService& svc) {
        svc.block_validator_ = std::make_unique<consensus::BlockValidator>(
            svc.consensus_utxo_set_.get());
    }
    static bool Quarantine(ChainstateService& svc, CBlockIndex* index) {
        return svc.QuarantineStoredBodyForReplacement(index);
    }
    static void QueueCsnRefetch(ChainstateService& svc, const uint256& hash) {
        svc.pending_csn_body_refetches_.insert(hash);
    }
    static consensus::ConsensusUTXOSet& CsnSet(ChainstateService& svc) {
        return *svc.consensus_utxo_set_;
    }
    static bool Connect(ChainstateService& svc, CBlockIndex* candidate,
                        std::string& error, bool* consensus_invalid = nullptr) {
        return svc.ConnectTip(candidate, &error, consensus_invalid);
    }
};
} // namespace dinero

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::cout << "[PASS] " << message << '\n';
}

// The real daemon test above proves old sidecar detection. This companion
// isolates its liveness handoff: once a durable quarantine clears HAVE_DATA,
// a scheduler that still remembers RECEIVED + proof-staged will not fetch on
// an ordinary Tick. ActivateBestChain must explicitly reset that exact hash.
void RunCsnMarkerlessDispatchLiveness(const std::filesystem::path& root) {
    using namespace dinero;
    using namespace dinero::consensus;
    ChainDB db;
    Require(db.init(root / "chaindb") == Status::Ok,
            "markerless dispatch ChainDB opens");

    Transaction coinbase;
    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = UINT32_MAX;
    input.scriptSig = {1, 1};
    input.sequence = UINT32_MAX;
    coinbase.vin.push_back(input);
    TxOutput output;
    output.value = AmountUna::Una(1);
    output.scriptPubKey = {0x51};
    coinbase.vout.push_back(output);

    BlockHeader genesis;
    genesis.version = 1;
    genesis.timestamp = 1'000'000;
    genesis.difficulty = 0x207fffff;
    genesis.nonce = 1;
    genesis.merkle_root = ComputeMerkleRoot({coinbase});
    BlockHeader child = genesis;
    child.prev_block_hash = genesis.GetHash();
    child.timestamp++;
    child.nonce++;

    HeaderChainSelector selector;
    Require(selector.AddHeader(genesis) && selector.AddHeader(child),
            "markerless dispatch headers admitted");
    const auto hash = child.GetHash();
    auto* index = AddBlockIndex(child, 1);
    Require(index != nullptr, "markerless dispatch index created");
    index->status |= BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS;
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = genesis.GetHash();
    metadata.height = 1;
    metadata.chainwork = arith_uint256(2);
    metadata.status_flags = index->status;
    metadata.file_number = 1;
    metadata.data_pos = 128;
    metadata.data_size = 512;
    const auto token = ChainWriteToken::CreateForTesting();
    Require(db.putHeaderMetadata(token, hash, metadata) == Status::Ok,
            "markerless dispatch stored metadata staged");

    auto scheduler = std::make_shared<consensus::BlockDownloadScheduler>(
        &selector, nullptr);
    scheduler->SetStatelessMode(true);
    scheduler->SetLocalTipHeight(0);
    scheduler->SetGetTipHeightCallback([] { return uint32_t{0}; });
    scheduler->SetGetBlockHashAtHeightCallback(
        [&](uint32_t height, uint256& out) {
            if (height != 0) return false;
            out = genesis.GetHash();
            return true;
        });
    scheduler->SetTipRetryTimeout(std::chrono::hours(1));
    scheduler->SetStaleRequestTimeoutSeconds(3600);
    std::vector<uint256> requests;
    scheduler->SetSendGetDataCallback([&](const uint256& requested, uint32_t) {
        requests.push_back(requested);
    });
    scheduler->OnHeadersProcessed();
    scheduler->Tick();
    Require(requests.size() == 1 && requests[0] == hash,
            "markerless dispatch initial body request issued");
    Block body;
    body.header = child;
    body.vtx = {coinbase};
    Require(scheduler->OnBlockReceived(body) &&
                scheduler->AcknowledgeStatelessProofStaged(hash),
            "markerless dispatch stale receipt/proof state staged");
    requests.clear();
    scheduler->Tick();
    Require(requests.empty(), "stale proof receipt suppresses ordinary retry");

    DaemonContext context;
    Require(DaemonContext::instance() == nullptr,
            "markerless dispatch owns isolated daemon context");
    context.block_download = scheduler;
    DaemonContext::setInstance(&context);
    try {
        ChainstateService service;
        service.setChainDB(&db);
        Require(ActivationRetryTestAccess::Quarantine(service, index),
                "markerless dispatch body quarantined");
        const auto quarantined = db.getHeaderMetadata(hash);
        Require(quarantined.ok() && quarantined->data_size == 0 &&
                    (quarantined->status_flags &
                     (BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS)) == 0,
                "markerless dispatch quarantine durably clears body flags");
        ActivationRetryTestAccess::QueueCsnRefetch(service, hash);
        // No active chain is required for this dispatch-only case. The
        // activation pass exits before connecting, but the root tick still
        // drains the queued exact-hash refetch outside its selected lock.
        service.setChainDB(nullptr);
        service.ActivateBestChain();
        Require(requests.size() == 1 && requests[0] == hash,
                "activation dispatches exact quarantined hash");
        Require(scheduler->IsBlockInFlight(hash) &&
                    !scheduler->HasReceivedBlock(hash),
                "stale received/proof state reset to a fresh request");
        Require(scheduler->OnBlockReceived(body) &&
                    scheduler->AcknowledgeStatelessProofStaged(hash),
                "fresh receipt can be validated after dispatch");
    } catch (...) {
        DaemonContext::setInstance(nullptr);
        throw;
    }
    DaemonContext::setInstance(nullptr);
    db.close();
    g_candidates.clear();
    g_orphan_pool.clear();
    g_block_index.clear();
    InvalidateAncestryCache();
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

// Exercise the production connect-abort path with an exact stored body. Both a
// missing mandatory commitment and an empty header-first body are deterministic
// failures; leaving either retryable lets its lower-hash tip outrank an
// equal-work sibling forever.
void RunDeterministicCandidateFailure(const std::filesystem::path& root,
                                      bool empty_body = false) {
    using namespace dinero;
    ChainDB db;
    Require(db.init(root / "chaindb") == Status::Ok, "filter case ChainDB opens");
    auto files = std::make_shared<BlockStorage>();
    Require(files->init(root) == Status::Ok, "filter case flatfiles open");

    BlockHeader genesis_header;
    genesis_header.version = 1;
    genesis_header.timestamp = 1000000;
    genesis_header.difficulty = 0x207fffff;
    genesis_header.nonce = 1;
    auto* genesis = AddBlockIndex(genesis_header, 0);
    BlockHeader parent_header = genesis_header;
    parent_header.prev_block_hash = genesis->hash;
    parent_header.timestamp += 1;
    parent_header.nonce += 1;
    auto* parent = AddBlockIndex(parent_header, 1);
    genesis->status |= BLOCK_VALID_CHAIN;
    genesis->status |= BLOCK_HAVE_DATA;
    parent->status |= BLOCK_VALID_CHAIN;
    parent->status |= BLOCK_HAVE_DATA;

    Transaction coinbase;
    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = UINT32_MAX;
    input.scriptSig = {1, 2};
    input.sequence = UINT32_MAX;
    coinbase.vin.push_back(input);
    TxOutput output;
    output.value = AmountUna::Una(1);
    output.scriptPubKey = {0x51};
    coinbase.vout.push_back(output);
    Block first;
    first.vtx = {coinbase};
    first.header.version = 1;
    first.header.prev_block_hash = parent->hash;
    first.header.timestamp = 1000002;
    first.header.difficulty = 0x207fffff;
    first.header.merkle_root = consensus::ComputeMerkleRoot(first.vtx);
    first.header.nonce = 101;
    Block second = first;
    second.header.nonce = 102;
    if (empty_body) {
        // A zero-transaction body is bound only to a header with the empty
        // Merkle root. Keep this deterministic-invalid case distinct from a
        // corrupt body stored under an otherwise honest header.
        first.vtx.clear();
        first.header.merkle_root = consensus::ComputeMerkleRoot(first.vtx);
        while (!(first.GetHash() < second.GetHash())) ++first.header.nonce;
    }
    Block& bad = first.GetHash() < second.GetHash() ? first : second;
    Block& sibling = first.GetHash() < second.GetHash() ? second : first;
    auto* bad_index = AddBlockIndex(bad.header, 2);
    auto* sibling_index = AddBlockIndex(sibling.header, 2);
    bad_index->status |= BLOCK_HAVE_DATA;
    sibling_index->status |= BLOCK_HAVE_DATA;
    Require(bad_index->hash < sibling_index->hash,
            "deterministic invalid candidate ranks before equal-work sibling");

    const auto token = ChainWriteToken::CreateForTesting();
    Require(db.setTip(token, parent->hash, 1, arith_uint256()) == Status::Ok,
            "filter case active tip staged");
    Require(db.putHeader(token, bad_index->hash, bad.header, 2,
                         arith_uint256(3)) == Status::Ok,
            "filter case header staged");
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = parent->hash;
    metadata.height = 2;
    metadata.chainwork = arith_uint256(3);
    metadata.status_flags = bad_index->status;
    Require(db.putHeaderMetadata(token, bad_index->hash, metadata) == Status::Ok,
            "filter case metadata staged");
    const auto pos = files->writeBlock(bad_index->hash, bad);
    Require(pos.status() == Status::Ok, "filter case body written");
    {
        ChainstateService svc;
        svc.setChainDB(&db);
        svc.setBlockStorage(files);
        ActivationRetryTestAccess::Prepare(svc, parent, db);
        ActivationRetryTestAccess::PrepareConnect(svc);
        svc.PersistStoredBodyPosition(bad_index->hash, pos.value());
        const auto stored = svc.getBlockByHash(bad_index->hash);
        Require(stored.status() == Status::Ok,
                "filter case stored body is readable");
        if (empty_body) Require(stored->vtx.empty(),
                                "merkle-bound empty body reaches activation");
        svc.AddCandidate(bad_index);
        svc.AddCandidate(sibling_index);
        Require(ActivationRetryTestAccess::Best(svc) == bad_index,
                "lower-hash invalid candidate is selected first");
        svc.ActivateBestChain();
        Require((bad_index->status & BLOCK_FAILED_VALID) != 0,
                "deterministic failure marks candidate invalid in memory");
        const auto saved = db.getHeaderMetadata(bad_index->hash);
        Require(saved.ok() && (saved->status_flags & BLOCK_FAILED_VALID) != 0,
                "deterministic failure persists invalid status");
        Require(ActivationRetryTestAccess::Best(svc) == sibling_index,
                "invalid lower-hash candidate no longer excludes sibling");
        Require(svc.GetActiveTip() == parent,
                "failed candidate did not publish an invalid tip");
    }
    db.close();
    Require(db.init(root / "chaindb") == Status::Ok,
            "filter case ChainDB reopens");
    const auto reopened = db.getHeaderMetadata(bad_index->hash);
    Require(reopened.ok() && (reopened->status_flags & BLOCK_FAILED_VALID) != 0,
            "deterministic failure remains invalid after database reopen");
    files->close();
    db.close();
    g_candidates.clear();
    g_orphan_pool.clear();
    g_block_index.clear();
    InvalidateAncestryCache();
}

// A pre-fix binary could persist a body whose header hash is honest but whose
// transactions do not match that header's Merkle root. On restart, the stored
// body must be discarded and made replaceable; it must never poison the header.
void RunLegacyStoredBodyRecovery(const std::filesystem::path& root,
                                 bool mutated_body = false,
                                 bool witness_body = false) {
    using namespace dinero;
    ChainDB db;
    Require(db.init(root / "chaindb") == Status::Ok, "legacy-body ChainDB opens");
    auto files = std::make_shared<BlockStorage>();
    Require(files->init(root) == Status::Ok, "legacy-body flatfiles open");

    BlockHeader genesis_header;
    genesis_header.version = 1;
    genesis_header.timestamp = 1000000;
    genesis_header.difficulty = 0x207fffff;
    genesis_header.nonce = 1;
    auto* genesis = AddBlockIndex(genesis_header, 0);
    BlockHeader parent_header = genesis_header;
    parent_header.prev_block_hash = genesis->hash;
    parent_header.timestamp += 1;
    parent_header.nonce += 1;
    auto* parent = AddBlockIndex(parent_header, 1);
    genesis->status |= BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;
    parent->status |= BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

    Transaction coinbase;
    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = UINT32_MAX;
    input.scriptSig = {1, 2};
    input.sequence = UINT32_MAX;
    coinbase.vin.push_back(input);
    TxOutput output;
    output.value = AmountUna::Una(1);
    output.scriptPubKey = {0x51};
    coinbase.vout.push_back(output);
    Block correct;
    correct.vtx = {coinbase};
    if (witness_body) {
        Transaction spend;
        TxInput spent_input;
        spent_input.prevout.txid = TxId();
        spent_input.prevout.vout = 1;
        spent_input.witness.push_back({0x30, 0x31, 0x32});
        spend.vin.push_back(spent_input);
        spend.vout.push_back(output);
        correct.vtx.push_back(spend);
        TxOutput commitment_output;
        commitment_output.value = AmountUna::Zero();
        commitment_output.scriptPubKey =
            consensus::BuildWitnessCommitment(correct.vtx);
        correct.vtx[0].vout.push_back(commitment_output);
        std::string witness_error;
        Require(consensus::ValidateWitnessCommitment(correct.vtx, witness_error),
                "legacy-body fixture has valid witness commitment");
    } else if (mutated_body) {
        auto second = coinbase;
        second.vout.front().value = AmountUna::Una(2);
        auto third = coinbase;
        third.vout.front().value = AmountUna::Una(3);
        correct.vtx.push_back(second);
        correct.vtx.push_back(third);
    }
    correct.header.version = 1;
    correct.header.prev_block_hash = parent->hash;
    correct.header.timestamp = 1000002;
    correct.header.difficulty = 0x207fffff;
    correct.header.merkle_root = consensus::ComputeMerkleRoot(correct.vtx);
    correct.header.nonce = 101;
    const auto hash = correct.GetHash();
    auto* candidate = AddBlockIndex(correct.header, 2);
    candidate->status |= BLOCK_HAVE_DATA;

    Block wrong = correct;
    if (witness_body) {
        wrong.vtx[1].vin[0].witness[0][0] ^= 1;
        std::string witness_error;
        Require(wrong.vtx[1].GetTxid() == correct.vtx[1].GetTxid() &&
                    consensus::ComputeMerkleRoot(wrong.vtx) == correct.header.merkle_root &&
                    !consensus::ValidateWitnessCommitment(wrong.vtx, witness_error),
                "legacy-body fixture changes witness without changing header or txid");
    } else if (mutated_body) {
        wrong.vtx.push_back(wrong.vtx.back());
        bool mutated = false;
        Require(consensus::ComputeMerkleRoot(wrong.vtx, &mutated) ==
                    correct.header.merkle_root && mutated,
                "legacy-body fixture duplicates last transaction without changing Merkle root");
    } else {
        wrong.vtx.front().vout.front().value = AmountUna::Una(2);
        Require(consensus::ComputeMerkleRoot(wrong.vtx) != correct.header.merkle_root,
                "legacy-body fixture changes transactions without changing header");
    }
    const auto token = ChainWriteToken::CreateForTesting();
    Require(db.setTip(token, parent->hash, 1, arith_uint256()) == Status::Ok,
            "legacy-body active tip staged");
    Require(db.putHeader(token, hash, correct.header, 2, arith_uint256(3)) == Status::Ok,
            "legacy-body header staged");
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = parent->hash;
    metadata.height = 2;
    metadata.chainwork = arith_uint256(3);
    metadata.status_flags = candidate->status | BLOCK_VALID_TRANSACTIONS;
    Require(db.putHeaderMetadata(token, hash, metadata) == Status::Ok,
            "legacy-body metadata staged");
    const auto wrong_pos = files->writeBlock(hash, wrong);
    Require(wrong_pos.ok(), "legacy-body mismatched body stored");
    {
        ChainstateService svc;
        svc.setChainDB(&db);
        svc.setBlockStorage(files);
        ActivationRetryTestAccess::Prepare(svc, parent, db);
        ActivationRetryTestAccess::PrepareConnect(svc);
        svc.PersistStoredBodyPosition(hash, wrong_pos.value());
        Require(svc.getBlockByHash(hash).ok(), "legacy-body old stored body is readable by hash");
        svc.AddCandidate(candidate);
        svc.ActivateBestChain();
        Require((candidate->status & BLOCK_FAILED_VALID) == 0,
                "mismatched old body does not mark honest header consensus-invalid");
        Require((candidate->status & BLOCK_HAVE_DATA) == 0,
                "mismatched old body is removed from candidate data availability");
        const auto quarantined = db.getHeaderMetadata(hash);
        Require(quarantined.ok() && quarantined->data_size == 0 &&
                    (quarantined->status_flags &
                     (BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS)) == 0,
                "mismatched old body is durably quarantined");
        if (mutated_body) {
            const auto correct_pos = files->writeBlock(hash, correct);
            Require(correct_pos.ok(), "replacement body written");
            svc.PersistStoredBodyPosition(hash, correct_pos.value());
            const auto repaired = svc.getBlockByHash(hash);
            Require(repaired.ok() &&
                        consensus::ComputeMerkleRoot(repaired->vtx) == correct.header.merkle_root,
                    "verified replacement body becomes readable");
            Require(svc.hasBlockByHash(hash),
                    "verified replacement clears unreadable quarantine");
        }
    }
    db.close();
    Require(db.init(root / "chaindb") == Status::Ok,
            "legacy-body ChainDB reopens");
    if (!mutated_body) {
        const auto quarantined_reopen = db.getHeaderMetadata(hash);
        Require(quarantined_reopen.ok() && quarantined_reopen->data_size == 0 &&
                    (quarantined_reopen->status_flags &
                     (BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS)) == 0,
                "old unbound body remains quarantined after database reopen");
        {
            ChainstateService restarted;
            restarted.setChainDB(&db);
            restarted.setBlockStorage(files);
            Require(!restarted.hasBlockByHash(hash) &&
                        restarted.getBlockByHash(hash).status() == Status::NotFound,
                    "quarantined body stays unavailable after restart");
            const auto correct_pos = files->writeBlock(hash, correct);
            Require(correct_pos.ok(), "replacement body written after restart");
            restarted.PersistStoredBodyPosition(hash, correct_pos.value());
            const auto repaired = restarted.getBlockByHash(hash);
            Require(repaired.ok() &&
                        consensus::ComputeMerkleRoot(repaired->vtx) == correct.header.merkle_root,
                    "verified replacement body becomes readable after restart");
        }
    }
    const auto reopened = db.getHeaderMetadata(hash);
    Require(reopened.ok() && (reopened->status_flags & BLOCK_FAILED_VALID) == 0 &&
                (reopened->status_flags & BLOCK_HAVE_DATA) != 0,
            "replacement body retains valid header status after restart");
    files->close();
    db.close();
    g_candidates.clear();
    g_orphan_pool.clear();
    g_block_index.clear();
    InvalidateAncestryCache();
}

// Compare valid fork graphs against the unchanged original predicate. This
// reference deliberately has no memo: correctness must not rely on caching.
bool OriginalSharesAncestor(const dinero::CBlockIndex* active,
                            const dinero::CBlockIndex* candidate) {
    if (!active || !candidate) return true;
    auto* a = active;
    auto* b = candidate;
    while (a && b && a->height > b->height) a = a->pprev;
    while (a && b && b->height > a->height) b = b->pprev;
    while (a && b) {
        if (a->hash == b->hash) return true;
        a = a->pprev;
        b = b->pprev;
    }
    return false;
}

void RunAncestryEquivalence() {
    using namespace dinero;
    std::vector<std::unique_ptr<CBlockIndex>> nodes;
    uint32_t random = 0x42534a31;
    for (uint32_t i = 0; i < 80; ++i) {
        random = random * 1664525U + 1013904223U;
        auto node = std::make_unique<CBlockIndex>();
        node->pprev = i > 1 ? nodes[random % i].get() : nullptr;
        node->height = node->pprev ? node->pprev->height + 1 : 0;
        std::ostringstream serial;
        serial << std::hex << std::setfill('0') << std::setw(64) << (20000000ULL + i);
        node->hash = uint256::FromHexUnsafe(serial.str());
        if (node->pprev) node->prev_hash = node->pprev->hash;
        nodes.push_back(std::move(node));
    }
    bool equivalent = true;
    bool linear = true;
    for (uint32_t pass = 0; pass < 16; ++pass) {
        const auto* active = nodes[(pass * 17 + 7) % nodes.size()].get();
        consensus::CandidateAncestryView view(active);
        for (const auto& node : nodes)
            equivalent = (view.SharesActiveAncestor(node.get()) ==
                          OriginalSharesAncestor(active, node.get())) && equivalent;
        linear = view.ActiveWalkSteps() <= active->height &&
                 view.BranchWalkSteps() <= nodes.size() && linear;
    }
    Require(equivalent, "1280 fork/active-tip comparisons equal the original predicate");
    Require(linear, "shared fork branches are walked at most once per pass");
}

// Real GetBestCandidate, not a model of its ranking. A long-running pool can
// retain connected historical candidates; replaying the active ancestry for
// every one produces a quadratic ingress-lock hold. The 15-second public-pool
// request budget is generous for the linear implementation, including ASan.
void RunCandidateCostCase() {
    using namespace dinero;
    constexpr uint32_t height = 150000;
    std::vector<std::unique_ptr<CBlockIndex>> chain;
    chain.reserve(height + 1);
    for (uint32_t h = 0; h <= height; ++h) {
        auto node = std::make_unique<CBlockIndex>();
        node->height = h;
        std::ostringstream number;
        number << std::hex << std::setfill('0') << std::setw(64) << (uint64_t(h) + 1);
        node->hash = uint256::FromHexUnsafe(number.str());
        node->chainwork = number.str();
        node->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;
        node->pprev = h ? chain.back().get() : nullptr;
        if (node->pprev) node->prev_hash = node->pprev->hash;
        chain.push_back(std::move(node));
    }
    // Deterministic work-count controls complement the real service budget.
    consensus::CandidateAncestryView view(chain.back().get());
    bool all_share = true;
    for (uint32_t h = 0; h <= height; ++h)
        all_share = view.SharesActiveAncestor(chain[h].get()) && all_share;
    Require(all_share, "every historical candidate shares the active ancestry");
    Require(view.ActiveWalkSteps() == height && view.BranchWalkSteps() == 0,
            "all historical candidates share exactly one active-chain walk");

    CBlockIndex sibling;
    sibling.height = height;
    sibling.pprev = chain[height - 1].get();
    sibling.hash = uint256::FromHexUnsafe(std::string(63, 'f') + "1");
    Require(view.SharesActiveAncestor(&sibling), "fork sibling shares its real ancestor");
    Require(view.BranchWalkSteps() == 1, "fork sibling adds only one branch step");
    Require(view.SharesActiveAncestor(&sibling) && view.BranchWalkSteps() == 1,
            "same-pass fork result is reused");

    CBlockIndex duplicate;
    duplicate.height = height;
    duplicate.hash = chain.back()->hash;
    Require(view.SharesActiveAncestor(&duplicate),
            "same-height same-hash index objects retain original compatibility");
    CBlockIndex foreign;
    foreign.hash = uint256::FromHexUnsafe(std::string(64, 'e'));
    Require(!view.SharesActiveAncestor(&foreign), "disconnected genesis is incompatible");
    consensus::CandidateAncestryView changed(&foreign);
    Require(!changed.SharesActiveAncestor(chain.back().get()),
            "a fresh pass cannot reuse the previous tip's ancestry results");
    CBlockIndex cycle;
    cycle.height = 3;
    cycle.hash = uint256::FromHexUnsafe(std::string(64, 'd'));
    cycle.pprev = &cycle;
    Require(!view.SharesActiveAncestor(&cycle),
            "malformed candidate cycle refuses without an unbounded walk");
    consensus::CandidateAncestryView malformed(&cycle);
    Require(!malformed.SharesActiveAncestor(chain.front().get()),
            "malformed active cycle refuses without an unbounded walk");
    InvalidateAncestryCache();
    {
        ChainstateService service;
        ActivationRetryTestAccess::PrepareCandidateCost(service, chain.back().get(), chain);
        const auto start = std::chrono::steady_clock::now();
        std::cout << "Selecting among 150001 real connected historical candidates" << std::endl;
        Require(ActivationRetryTestAccess::Best(service) == chain.back().get(),
                "large historical candidate set selects the exact highest-work active tip");
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "candidate selection seconds: " << elapsed << std::endl;
        Require(ActivationRetryTestAccess::Contains(service, chain.front().get()),
                "performance repair preserves historical candidate membership");
    }
    InvalidateAncestryCache();
}
} // namespace

void RunSigopsStoredCandidate(const std::filesystem::path& root) {
    using namespace dinero;
    ChainDB db;
    Require(db.init(root / "chaindb") == Status::Ok, "sigops ChainDB opens");
    auto files = std::make_shared<BlockStorage>();
    Require(files->init(root) == Status::Ok, "sigops flatfiles open");
    BlockHeader genesis_header;
    genesis_header.version = 1;
    genesis_header.timestamp = 1000000;
    genesis_header.difficulty = 0x207fffff;
    genesis_header.nonce = 1;
    auto* genesis = AddBlockIndex(genesis_header, 0);
    BlockHeader parent_header = genesis_header;
    parent_header.prev_block_hash = genesis->hash;
    parent_header.timestamp += 1;
    parent_header.nonce += 1;
    auto* parent = AddBlockIndex(parent_header, 1);
    genesis->status |= BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;
    parent->status |= BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;
    Transaction coinbase;
    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = UINT32_MAX;
    input.scriptSig = {1, 2};
    input.sequence = UINT32_MAX;
    coinbase.vin.push_back(input);
    for (int n = 0; n < 321; ++n) {
        TxOutput output;
        output.value = AmountUna::Una(n == 0 ? 1 : 0);
        output.scriptPubKey = std::vector<uint8_t>(250, 0xac);
        coinbase.vout.push_back(output);
    }
    TxOutput dnrf;
    dnrf.value = AmountUna::Una(0);
    dnrf.scriptPubKey = consensus::BuildFilterCommitmentScript(
        uint256::FromHexUnsafe(std::string(63, '0') + '1'));
    coinbase.vout.push_back(dnrf);
    Block block;
    block.vtx = {coinbase};
    block.header.version = 1;
    block.header.prev_block_hash = parent->hash;
    block.header.timestamp = 1000002;
    block.header.difficulty = 0x207fffff;
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);
    block.header.nonce = 101;
    consensus::ConsensusUTXOSet preview_set;
    preview_set.SetBestBlock(parent->hash, 1);
    consensus::BlockValidator preview_validator(&preview_set);
    uint256 computed_root;
    std::string root_error;
    Require(preview_validator.ComputeUtreexoRootPure(block, 2, computed_root, root_error),
            "sigops body has computed Utreexo root");
    block.header.utreexo_root = computed_root;
    unsigned int sigop_cost = 0;
    std::string sigop_error;
    const bool sigops_ok = consensus::CheckBlockSigops(block, sigop_cost, sigop_error);
    std::cout << "SIGOPS_COUNT " << sigop_cost << " ok=" << sigops_ok
              << " error=" << sigop_error << "\n";
    Require(!sigops_ok &&
                sigop_cost > consensus::MAX_BLOCK_SIGOPS_COST,
            "same body is ingress-invalid for excessive sigops");
    consensus::ConsensusUTXOSet stateless_set;
    stateless_set.SetBestBlock(parent->hash, 1);
    const auto stateless_root_before = stateless_set.GetForest().getCommitment();
    consensus::BlockValidator stateless_validator(&stateless_set);
    stateless_validator.setValidationMode(consensus::ValidationMode::STATELESS);
    consensus::BlockUndo stateless_undo;
    std::string stateless_error;
    Require(!stateless_validator.ValidateAndApplyBlock(
                block, 2, block.GetHash(), stateless_undo, stateless_error) &&
                stateless_error.rfind("block-sigops-exceeded", 0) == 0,
            "stateless validator rejects ingress-invalid excessive-sigops body");
    Require(stateless_set.GetForest().getCommitment() == stateless_root_before,
            "stateless sigops rejection does not mutate the forest");
    consensus::ConsensusUTXOSet connect_set;
    connect_set.SetBestBlock(parent->hash, 1);
    consensus::BlockValidator connect_validator(&connect_set);
    consensus::BlockUndo connect_undo;
    std::string connect_error;
    Require(!connect_validator.ConnectBlock(block, 2, block.GetHash(),
                                            connect_undo, connect_error) &&
                connect_error.rfind("block-sigops-exceeded", 0) == 0,
            "shared connect validator rejects same excessive-sigops body");
    auto* candidate = AddBlockIndex(block.header, 2);
    candidate->status |= BLOCK_HAVE_DATA;
    const auto token = ChainWriteToken::CreateForTesting();
    Require(db.setTip(token, parent->hash, 1, arith_uint256()) == Status::Ok,
            "sigops active tip staged");
    Require(db.putHeader(token, candidate->hash, block.header, 2,
                         arith_uint256(3)) == Status::Ok,
            "sigops header staged");
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = parent->hash;
    metadata.height = 2;
    metadata.chainwork = arith_uint256(3);
    metadata.status_flags = candidate->status;
    Require(db.putHeaderMetadata(token, candidate->hash, metadata) == Status::Ok,
            "sigops metadata staged");
    const auto pos = files->writeBlock(candidate->hash, block);
    Require(pos.ok(), "sigops body stored as scheduler would");
    {
        ChainstateService svc;
        svc.setChainDB(&db);
        svc.setBlockStorage(files);
        ActivationRetryTestAccess::Prepare(svc, parent, db);
        ActivationRetryTestAccess::PrepareConnect(svc);
        svc.PersistStoredBodyPosition(candidate->hash, pos.value());
        svc.AddCandidate(candidate);
        svc.ActivateBestChain();
        Require(svc.GetActiveTip() == parent,
                "sigops-invalid stored body never becomes active tip");
        Require((candidate->status & BLOCK_FAILED_VALID) != 0,
                "sigops-invalid Merkle-bound candidate is marked failed");
    }
    files->close();
    db.close();
    g_candidates.clear();
    g_orphan_pool.clear();
    g_block_index.clear();
    InvalidateAncestryCache();
}

// Exercise the actual CSN ConnectTip recovery route with a proof-consistent
// block whose only invalidity is a missing Taproot signature. The ordinary
// full validator must reject it, and the CSN route must leave the tip alone.
void RunCsnInvalidSignatureReplay(const std::filesystem::path& root,
                                  bool activate_candidates = false,
                                  bool stage_unbound_fork = false) {
    using namespace dinero;
    GetConfig().utreexo_stateless = true;
    MutableParams().state_commitment_activation_height = UINT32_MAX;
    ChainDB db;
    Require(db.init(root / "chaindb") == Status::Ok, "CSN fixture ChainDB opens");
    auto files = std::make_shared<BlockStorage>();
    Require(files->init(root) == Status::Ok, "CSN fixture flatfiles open");

    const uint32_t height = consensus::GetUtreexoMaturityLeafActivationHeight() + 1;
    const uint32_t parent_height = height - 1;
    uint256 funding_hash;
    std::fill(funding_hash.begin(), funding_hash.end(), 0x11);
    const TxId funding_txid(funding_hash);
    const OutPoint funding(funding_txid, 0);
    std::vector<uint8_t> p2tr = {0x51, 0x20,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98};
    const consensus::UTXOEntry funded(AmountUna::Una(5000), p2tr, parent_height, false);
    const auto funding_leaf = consensus::HashUTXOForCreationHeight(
        funding_txid.AsUint256(), 0, 5000, p2tr, parent_height, false);
    consensus::ConsensusUTXOSet producer;
    Require(producer.AddCoin(funding, funded), "producer funding UTXO staged");
    Require(producer.GetForest().add(funding_leaf) != UINT64_MAX,
            "producer funding leaf staged");
    const auto parent_root = producer.GetForest().getCommitment();

    BlockHeader genesis_header;
    genesis_header.version = 1;
    genesis_header.timestamp = 1000000;
    genesis_header.difficulty = 0x207fffff;
    genesis_header.nonce = 1;
    auto* genesis = AddBlockIndex(genesis_header, 0);
    BlockHeader parent_header = genesis_header;
    parent_header.prev_block_hash = genesis->hash;
    parent_header.timestamp += parent_height;
    parent_header.nonce += parent_height;
    std::copy(parent_root.begin(), parent_root.end(), parent_header.utreexo_root.begin());
    auto* parent = AddBlockIndex(parent_header, parent_height);
    genesis->status |= BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;
    parent->status |= BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = parent->hash;
    block.header.timestamp = parent_header.timestamp + 120;
    block.header.difficulty = 0x207fffff;
    block.header.nonce = 100;
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.witness_version = 1;
    TxInput coinbase_input;
    coinbase_input.prevout.txid = TxId();
    coinbase_input.prevout.vout = UINT32_MAX;
    coinbase_input.scriptSig = {1, static_cast<uint8_t>(height), 0};
    coinbase_input.sequence = UINT32_MAX;
    coinbase.vin.push_back(coinbase_input);
    TxOutput coinbase_payout;
    coinbase_payout.value = AmountUna::Una(1);
    coinbase_payout.scriptPubKey = p2tr;
    coinbase.vout.push_back(coinbase_payout);
    Transaction spend;
    spend.version = 2;
    spend.witness_version = 1;
    TxInput spend_input;
    spend_input.prevout.txid = funding_txid;
    spend_input.prevout.vout = 0;
    spend_input.sequence = 0xfffffffe;
    spend_input.witness.push_back(std::vector<uint8_t>(64, 0x01));
    spend.vin.push_back(spend_input);
    TxOutput spend_output;
    spend_output.value = AmountUna::Una(4000);
    spend_output.scriptPubKey = p2tr;
    spend.vout.push_back(spend_output);
    block.vtx = {coinbase, spend};
    TxOutput witness;
    witness.value = AmountUna::Zero();
    witness.scriptPubKey = consensus::BuildWitnessCommitment(block.vtx);
    block.vtx.front().vout.push_back(witness);
    const auto filter = consensus::GCSFilter::Build({p2tr, p2tr, p2tr},
                                                     block.header.prev_block_hash);
    TxOutput dnrf;
    dnrf.value = AmountUna::Zero();
    dnrf.scriptPubKey = consensus::BuildFilterCommitmentScript(filter.GetHash());
    block.vtx.front().vout.push_back(dnrf);
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);
    auto provider = std::shared_ptr<consensus::IUTXOProvider>(
        std::shared_ptr<void>{}, static_cast<consensus::IUTXOProvider*>(&producer));
    network::BridgeNode bridge(provider, &producer.GetForest());
    block.utreexo = bridge.GenerateProofForBlock(block, height);
    consensus::BlockValidator producer_validator(&producer);
    uint256 post_root;
    std::string root_error;
    Require(producer_validator.ComputeUtreexoRootPure(block, height, post_root, root_error),
            "producer computes authentic post-root");
    block.header.utreexo_root = post_root;
    std::string schnorr_error;
    Require(!consensus::ScriptVerifier::VerifyTaproot(
                block.vtx[1], 0, {funded}, schnorr_error,
                consensus::CovenantActivationParams::StandardFlags(height, Params())) &&
                schnorr_error == "Schnorr signature verification failed",
            "64-byte key-path signature is cryptographically invalid");
    producer_validator.setValidationMode(consensus::ValidationMode::STATEFUL);
    consensus::BlockUndo full_undo;
    std::string full_error;
    const bool full_accepted = producer_validator.ConnectBlock(
        block, height, block.GetHash(), full_undo, full_error);
    std::cout << "FULL_INVALID_SIGNATURE_RESULT connected=" << full_accepted
              << " error=" << full_error << '\n';
    Require(!full_accepted &&
            full_error.find("Script validation failed") != std::string::npos,
            "full validator rejects identical authenticated block for invalid signature");
    auto* candidate = AddBlockIndex(block.header, height);
    candidate->status |= BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS | BLOCK_VALID_CHAIN;
    // The competing body is independently proof-consistent and valid. Choose
    // its nonce so the invalid block wins the equal-work hash ordering first.
    Block honest;
    honest.header = block.header;
    honest.header.nonce = block.header.nonce + 1;
    honest.vtx = {coinbase};
    TxOutput honest_witness;
    honest_witness.value = AmountUna::Zero();
    honest_witness.scriptPubKey = consensus::BuildWitnessCommitment(honest.vtx);
    honest.vtx.front().vout.push_back(honest_witness);
    TxOutput honest_filter;
    honest_filter.value = AmountUna::Zero();
    honest_filter.scriptPubKey = consensus::BuildFilterCommitmentScript(
        consensus::GCSFilter::Build({p2tr}, honest.header.prev_block_hash).GetHash());
    honest.vtx.front().vout.push_back(honest_filter);
    honest.header.merkle_root = consensus::ComputeMerkleRoot(honest.vtx);
    honest.utreexo = bridge.GenerateProofForBlock(honest, height);
    std::string honest_root_error;
    uint256 honest_root;
    Require(producer_validator.ComputeUtreexoRootPure(
                honest, height, honest_root, honest_root_error),
            "honest sibling post-root computed");
    honest.header.utreexo_root = honest_root;
    while (!(candidate->hash < honest.GetHash())) ++honest.header.nonce;
    auto* honest_index = AddBlockIndex(honest.header, height);
    honest_index->status |= BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS | BLOCK_VALID_CHAIN;
    Require(candidate->hash < honest_index->hash &&
                candidate->chainwork == honest_index->chainwork,
            "invalid candidate ranks before equal-work honest sibling");
    const auto token = ChainWriteToken::CreateForTesting();
    Require(db.setTip(token, parent->hash, parent_height, arith_uint256()) == Status::Ok,
            "CSN fixture active parent staged");
    Require(db.putHeader(token, parent->hash, parent_header, parent_height,
                         arith_uint256(parent_height + 1)) == Status::Ok,
            "CSN fixture parent header staged");
    if (activate_candidates) {
        Require(db.putUtreexoCheckpoint(token, parent_height,
                                        producer.GetForest().serialize()) == Status::Ok,
                "ABC fork-point forest checkpoint staged");
    }
    Require(db.putHeader(token, candidate->hash, block.header, height,
                         arith_uint256(height + 1)) == Status::Ok,
            "CSN fixture candidate header staged");
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = parent->hash;
    metadata.height = height;
    metadata.chainwork = arith_uint256(height + 1);
    metadata.status_flags = candidate->status;
    Require(db.putHeaderMetadata(token, candidate->hash, metadata) == Status::Ok,
            "CSN fixture candidate metadata staged");
    const auto pos = files->writeBlock(candidate->hash, block);
    Require(pos.status() == Status::Ok, "CSN fixture exact body stored");
    if (activate_candidates) {
        const auto replay_record = consensus::SerializeCsnReplayData(
            block.utreexo->spend_proof.targets, block.utreexo->spent_outputs,
            consensus::GetUtreexoProofFormatVersion(height));
        Require(db.putCSNSpendTargets(token, candidate->hash, replay_record) == Status::Ok,
                "ABC bad candidate replay metadata staged");
    }
    Require(db.putHeader(token, honest_index->hash, honest.header, height,
                         arith_uint256(height + 1)) == Status::Ok,
            "honest sibling header staged");
    metadata.status_flags = honest_index->status;
    Require(db.putHeaderMetadata(token, honest_index->hash, metadata) == Status::Ok,
            "honest sibling metadata staged");
    const auto honest_pos = files->writeBlock(honest_index->hash, honest);
    Require(honest_pos.status() == Status::Ok, "honest sibling body stored");

    {
        ChainstateService svc;
        svc.setChainDB(&db);
        svc.setBlockStorage(files);
        ActivationRetryTestAccess::Prepare(svc, parent, db);
        auto& csn_set = ActivationRetryTestAccess::CsnSet(svc);
        Require(csn_set.AddCoin(funding, funded), "CSN funding UTXO staged");
        Require(csn_set.GetForest().add(funding_leaf) != UINT64_MAX,
                "CSN funding leaf staged");
        Require(svc.GetShieldedNullifierSet()->Open(":memory:") ==
                    consensus::shielded::NullifierSet::OpenResult::Ok,
                "CSN scratch shielded nullifier state opened");
        ActivationRetryTestAccess::PrepareConnect(svc);
        auto replay = std::make_shared<network::StatelessNode>(&csn_set.GetForest());
        replay->SyncToForestState(parent_height);
        svc.setStatelessNode(replay);
        svc.PersistStoredBodyPosition(candidate->hash, pos.value());
        svc.PersistStoredBodyPosition(honest_index->hash, honest_pos.value());
        auto stored = svc.getBlockByHash(candidate->hash);
        Require(stored.status() == Status::Ok && stored->utreexo.has_value(),
                "CSN exact body and proof readable");
        const auto forest_before = csn_set.GetForest().getCommitment();
        if (activate_candidates) {
            svc.AddCandidate(candidate);
            svc.AddCandidate(honest_index);
            Require(ActivationRetryTestAccess::Best(svc) == candidate,
                    "ABC selects lower-hash invalid candidate first");
            svc.ActivateBestChain();
            const auto failed_status = db.getHeaderMetadata(candidate->hash);
            std::cout << "CSN_ABC_INVALID_SIGNATURE_RESULT active_parent="
                      << (svc.GetActiveTip() == parent)
                      << " forest_unchanged="
                      << (csn_set.GetForest().getCommitment() == forest_before)
                      << " failed_memory="
                      << ((candidate->status & BLOCK_FAILED_VALID) != 0)
                      << " failed_durable="
                      << (failed_status.ok() &&
                          (failed_status->status_flags & BLOCK_FAILED_VALID) != 0)
                      << '\n';
            Require(svc.GetActiveTip() == parent &&
                        csn_set.GetForest().getCommitment() == forest_before,
                    "ABC CSN failure preserves selected tip and forest");
            Require((candidate->status & BLOCK_FAILED_VALID) != 0 &&
                        failed_status.ok() &&
                        (failed_status->status_flags & BLOCK_FAILED_VALID) != 0,
                    "ABC durably marks authenticated invalid-signature body failed");
            Require(ActivationRetryTestAccess::Best(svc) == honest_index,
                    "ABC elects honest equal-work sibling after invalid candidate");
            svc.ActivateBestChain();
            std::cout << "CSN_ABC_HONEST_SIBLING_RESULT active="
                      << (svc.GetActiveTip() == honest_index) << '\n';
            Require(svc.GetActiveTip() == honest_index,
                    "ABC connects honest equal-work sibling");
        } else {
        std::string connect_error;
        bool consensus_invalid = false;
        const bool connected = ActivationRetryTestAccess::Connect(
            svc, candidate, connect_error, &consensus_invalid);
        std::cout << "CSN_INVALID_SIGNATURE_RESULT connected=" << connected
                  << " active=" << (svc.GetActiveTip() == candidate)
                  << " forest_unchanged="
                  << (csn_set.GetForest().getCommitment() == forest_before)
                  << " error=" << connect_error << '\n';
        Require(!connected && consensus_invalid && svc.GetActiveTip() == parent &&
                csn_set.GetForest().getCommitment() == forest_before,
                "CSN classifies unauthorized spend as invalid before tip or forest mutation");

        // A transparent-only block can carry a syntactically present but
        // incorrect DNRS commitment. Its shielded post-state must be checked
        // before the recovery path mutates the shared forest.
        Block wrong_dnrs;
        wrong_dnrs.header = block.header;
        wrong_dnrs.header.nonce += 2;
        wrong_dnrs.vtx = {coinbase};
        TxOutput wrong_witness;
        wrong_witness.value = AmountUna::Zero();
        wrong_witness.scriptPubKey = consensus::BuildWitnessCommitment(wrong_dnrs.vtx);
        wrong_dnrs.vtx.front().vout.push_back(wrong_witness);
        TxOutput wrong_filter;
        wrong_filter.value = AmountUna::Zero();
        wrong_filter.scriptPubKey = consensus::BuildFilterCommitmentScript(
            consensus::GCSFilter::Build({p2tr}, wrong_dnrs.header.prev_block_hash).GetHash());
        wrong_dnrs.vtx.front().vout.push_back(wrong_filter);
        TxOutput wrong_commitment;
        wrong_commitment.value = AmountUna::Zero();
        uint256 unrelated_root;
        std::fill(unrelated_root.begin(), unrelated_root.end(), 0x42);
        wrong_commitment.scriptPubKey = consensus::BuildStateCommitmentScript(unrelated_root);
        wrong_dnrs.vtx.front().vout.push_back(wrong_commitment);
        wrong_dnrs.header.merkle_root = consensus::ComputeMerkleRoot(wrong_dnrs.vtx);
        wrong_dnrs.utreexo = bridge.GenerateProofForBlock(wrong_dnrs, height);
        uint256 wrong_post_root;
        std::string wrong_root_error;
        Require(producer_validator.ComputeUtreexoRootPure(
                    wrong_dnrs, height, wrong_post_root, wrong_root_error),
                "transparent-only wrong-DNRS post-root computed");
        wrong_dnrs.header.utreexo_root = wrong_post_root;
        auto* wrong_index = AddBlockIndex(wrong_dnrs.header, height);
        wrong_index->status |= BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS | BLOCK_VALID_CHAIN;
        Require(db.putHeader(token, wrong_index->hash, wrong_dnrs.header, height,
                             arith_uint256(height + 1)) == Status::Ok,
                "wrong-DNRS candidate header staged");
        metadata.status_flags = wrong_index->status;
        Require(db.putHeaderMetadata(token, wrong_index->hash, metadata) == Status::Ok,
                "wrong-DNRS candidate metadata staged");
        const auto wrong_pos = files->writeBlock(wrong_index->hash, wrong_dnrs);
        Require(wrong_pos.status() == Status::Ok,
                "wrong-DNRS exact body stored");
        svc.PersistStoredBodyPosition(wrong_index->hash, wrong_pos.value());
        MutableParams().state_commitment_activation_height = height;
        std::string wrong_error;
        bool wrong_consensus_invalid = false;
        const bool wrong_connected = ActivationRetryTestAccess::Connect(
            svc, wrong_index, wrong_error, &wrong_consensus_invalid);
        std::cout << "CSN_WRONG_DNRS_RESULT connected=" << wrong_connected
                  << " active=" << (svc.GetActiveTip() == wrong_index)
                  << " forest_unchanged="
                  << (csn_set.GetForest().getCommitment() == forest_before)
                  << " error=" << wrong_error << '\n';
        Require(!wrong_connected && svc.GetActiveTip() == parent &&
                csn_set.GetForest().getCommitment() == forest_before &&
                wrong_error.find("state-commitment-mismatch") != std::string::npos,
                "CSN rejects wrong DNRS before forest mutation");
        if (stage_unbound_fork) {
            // An archival read checks the header hash, but an old stored body
            // can still carry an unrelated coinbase. Place a DNRS commitment
            // equal to the scratch fork root in such a body. It cannot
            // authenticate permanent failure of the proposed branch.
            Block unbound_fork;
            unbound_fork.header = parent_header;
            unbound_fork.vtx = {coinbase};
            TxOutput fork_commitment;
            fork_commitment.value = AmountUna::Zero();
            const auto scratch_fork_root =
                ActivationRetryTestAccess::ForkShieldedRoot(svc);
            Require(scratch_fork_root.has_value(),
                    "selected fork shielded root is available");
            fork_commitment.scriptPubKey = consensus::BuildStateCommitmentScript(
                *scratch_fork_root);
            unbound_fork.vtx.front().vout.push_back(fork_commitment);
            Require(consensus::ComputeMerkleRoot(unbound_fork.vtx) !=
                        unbound_fork.header.merkle_root,
                    "selected fork body is deliberately unbound from its header");
            ChainDB::PersistedHeaderMetadata parent_metadata;
            parent_metadata.parent_hash = genesis->hash;
            parent_metadata.height = parent_height;
            parent_metadata.chainwork = arith_uint256(parent_height + 1);
            parent_metadata.status_flags = parent->status;
            Require(db.putHeaderMetadata(token, parent->hash, parent_metadata) == Status::Ok,
                    "unbound selected fork metadata staged");
            const auto fork_pos = files->writeBlock(parent->hash, unbound_fork);
            Require(fork_pos.status() == Status::Ok,
                    "unbound selected fork body staged");
            svc.PersistStoredBodyPosition(parent->hash, fork_pos.value());
            const auto readable_fork = svc.getBlockByHash(parent->hash);
            Require(readable_fork.status() == Status::Ok &&
                        consensus::ComputeMerkleRoot(readable_fork->vtx) !=
                            readable_fork->header.merkle_root,
                    "unbound selected fork body is readable at its header hash");
            MutableParams().state_commitment_activation_height = parent_height;
        }
        // Force the same transparent-only wrong-DNRS body through the ABC-CSN
        // replay route (a reconnect from a verified fork checkpoint). The
        // recovery call above covers ConnectTip; it does not cover ABC's
        // distinct stored-body path. The old ABC gate skipped this section
        // because there is no shielded transaction in the block.
        Require(db.putUtreexoCheckpointWithChecksum(
                    token, parent_height, csn_set.GetForest().serialize()) == Status::Ok,
                "CSN fork checkpoint staged for ABC preflight");
        svc.AddCandidate(wrong_index);
        const auto shielded_before = ActivationRetryTestAccess::ShieldedRoot(svc);
        svc.ActivateBestChain();
        const auto shielded_after = ActivationRetryTestAccess::ShieldedRoot(svc);
        if (stage_unbound_fork) {
            const auto stored_wrong = db.getHeaderMetadata(wrong_index->hash);
            std::cout << "CSN_UNBOUND_FORK_RESULT failed_memory="
                      << ((wrong_index->status & BLOCK_FAILED_VALID) != 0)
                      << " failed_durable="
                      << (stored_wrong.ok() &&
                          (stored_wrong->status_flags & BLOCK_FAILED_VALID) != 0)
                      << '\n';
            Require(stored_wrong.ok() &&
                        (wrong_index->status & BLOCK_FAILED_VALID) == 0 &&
                        (stored_wrong->status_flags & BLOCK_FAILED_VALID) == 0,
                    "unbound selected fork cannot durably fail proposed header");
        }
        Require(svc.GetActiveTip() == parent &&
                    csn_set.GetForest().getCommitment() == forest_before &&
                    shielded_after == shielded_before &&
                    ActivationRetryTestAccess::Failures(svc, wrong_index->hash) > 0 &&
                    (wrong_index->status & BLOCK_FAILED_VALID) == 0,
                "ABC-CSN wrong DNRS leaves canonical tip, forest and shielded state untouched; unproven fork stays retryable");
        MutableParams().state_commitment_activation_height = UINT32_MAX;

        // A different valid body at the same height must remain connectable
        // after the bad candidate is refused. This catches a partial rewind
        // that would pass the negative check but leave the node unable to
        // follow an honest branch.
        std::string honest_error;
        const bool honest_connected = ActivationRetryTestAccess::Connect(
            svc, honest_index, honest_error);
        std::cout << "CSN_HONEST_SIBLING_RESULT connected=" << honest_connected
                  << " active=" << (svc.GetActiveTip() == honest_index)
                  << " error=" << honest_error << '\n';
        Require(honest_connected && svc.GetActiveTip() == honest_index,
                "CSN can select honest same-height sibling after invalid candidate");
        // Now make the wrong-DNRS sibling the proposed heavier branch. This
        // exercises ABC with an actual old-branch disconnect, not just the
        // reconnect path above. Work is staged in the index for candidate
        // selection; the test concerns shielded preflight before mutation.
        MutableParams().state_commitment_activation_height =
            stage_unbound_fork ? parent_height : height;
        auto forced_work = ChainworkFromHex(honest_index->chainwork);
        forced_work += arith_uint256(1);
        wrong_index->chainwork = forced_work.GetHex();
        ActivationRetryTestAccess::ClearFailure(svc, wrong_index->hash);
        svc.AddCandidate(wrong_index);
        const auto honest_forest = csn_set.GetForest().getCommitment();
        const auto honest_shielded = ActivationRetryTestAccess::ShieldedRoot(svc);
        svc.ActivateBestChain();
        Require(svc.GetActiveTip() == honest_index &&
                    csn_set.GetForest().getCommitment() == honest_forest &&
                    ActivationRetryTestAccess::ShieldedRoot(svc) == honest_shielded &&
                    (wrong_index->status & BLOCK_FAILED_VALID) == 0,
                "ABC-CSN wrong-DNRS competing branch cannot disconnect honest tip or mutate canonical state");
        MutableParams().state_commitment_activation_height = UINT32_MAX;
        }
    }
    if (activate_candidates) {
        db.close();
        Require(db.init(root / "chaindb") == Status::Ok,
                "ABC fixture database reopens after candidate election");
        const auto failed_after_restart = db.getHeaderMetadata(candidate->hash);
        Require(failed_after_restart.ok() &&
                    (failed_after_restart->status_flags & BLOCK_FAILED_VALID) != 0,
                "ABC invalid-signature status survives database reopen");
    }
    files->close();
    db.close();
    g_candidates.clear();
    g_orphan_pool.clear();
    g_block_index.clear();
    InvalidateAncestryCache();
}

int main(int argc, char** argv) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("dinero-forward-activation-retry-" + std::to_string(unique));
    try {
        dinero::SelectParams(dinero::Chain::REGTEST);
        GetConfig().utreexo_stateless = false;
        GetConfig().assumeutxo_forward_connect = false;
        if (argc == 2 && std::string(argv[1]) == "--csn-invalid-signature") {
            RunCsnInvalidSignatureReplay(root / "csn-invalid-signature");
            std::filesystem::remove_all(root);
            std::cout << "ALL PASSED\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--csn-abc-invalid-signature") {
            RunCsnInvalidSignatureReplay(root / "csn-abc-invalid-signature", true);
            std::filesystem::remove_all(root);
            std::cout << "ALL PASSED\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--csn-unbound-fork-dnrs") {
            RunCsnInvalidSignatureReplay(root / "csn-unbound-fork-dnrs", false, true);
            std::filesystem::remove_all(root);
            std::cout << "ALL PASSED\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--ancestor-cost") {
            RunAncestryEquivalence();
            RunCandidateCostCase();
            std::cout << "ALL PASSED\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--markerless-dispatch") {
            RunCsnMarkerlessDispatchLiveness(root / "markerless-dispatch");
            std::filesystem::remove_all(root);
            std::cout << "ALL PASSED\n";
            return 0;
        }

        RunCase(root / "prefix-failure", true, true);
        RunCase(root / "first-body-gap", false, true);
        RunCase(root / "whole-path-failure", true, false);
        RunCase(root / "snapshot-cap-failure", true, false, true);
        RunDeterministicCandidateFailure(root / "filter-failure");
        // A header-first empty body skips the DNRF precheck and reaches the
        // validator's structural rejection. It must not remain retryable and
        // exclude an equal-work sibling after activation or restart.
        RunDeterministicCandidateFailure(root / "structural-empty", true);
        RunLegacyStoredBodyRecovery(root / "legacy-stored-body");
        RunLegacyStoredBodyRecovery(root / "legacy-mutated-body", true);
        RunLegacyStoredBodyRecovery(root / "legacy-witness-body", false, true);
        dinero::SelectParams(dinero::Chain::TESTNET);
        RunSigopsStoredCandidate(root / "sigops-stored");
        std::filesystem::remove_all(root);
        std::cout << "ALL PASSED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
