#include "consensus/orchard_header.h"
#include "consensus/orchard_profile.h"
#include "consensus/pow_context.h"
#include "consensus/pow.h"
#include "consensus/pow.hpp"
#include "consensus/genesis_canonical.h"
#include <cstdlib>
#include <iostream>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x << '\n'; std::exit(1); } } while (false)
using namespace dinero;
using namespace dinero::consensus;

BlockHeader Solve(BlockHeader header, bool valid = true) {
    for (uint32_t n = 0; n < 4'000'000; ++n) {
        header.nonce = n;
        if (CheckProofOfWork(header, false) == valid) return header;
    }
    throw std::runtime_error("isolated regtest nonce search exhausted");
}
OrchardBlockContext Context(const BlockHeader& header, uint32_t height) {
    OrchardBlockContext context;
    context.height = height; context.activation_height = 1;
    context.block_hash = header.GetHash(); context.parent_hash = header.prev_block_hash;
    context.domain.network_code = 2; context.domain.branch_id = 1; // isolated fixture, not a production branch ID
    uint256 genesis; CHECK(uint256::FromHex(Params().genesis_hash, genesis));
    std::copy(genesis.begin(), genesis.end(), context.domain.genesis_wire.begin());
    return context;
}
template<class F> void Rejected(OrchardHeaderErrorCode expected, F run) {
    bool rejected = false;
    try { run(); } catch (const OrchardHeaderError& e) { CHECK(e.Code() == expected); rejected = true; }
    CHECK(rejected);
}
template<class F> void LookupFailure(F run) {
    bool rejected = false;
    try { run(); } catch (const OrchardHeaderLookupError&) { rejected = true; }
    CHECK(rejected);
}
int main() {
    // All shipped network configurations remain inactive, including the
    // sentinel height. Only local copies are scheduled by this test.
    for (auto network : {Chain::MAINNET, Chain::TESTNET, Chain::REGTEST}) {
        SelectParams(network);
        CHECK(Params().orchard_activation_height == UINT32_MAX);
        CHECK(Params().orchard_branch_id == 0);
        CHECK(OrchardProfileConfigurationValid(Params()));
        CHECK(!OrchardActiveForHeight(Params(), UINT32_MAX));
        CHECK(!SelectedOrchardBlockContext(BlockHeader{}, UINT32_MAX));
        auto scheduled = Params();
        // Synthetic prerequisites avoid assigning an actual public schedule.
        scheduled.shielded_activation_height = 1;
        scheduled.shielded_input_binding_activation_height = 2;
        scheduled.shielded_cv_binding_activation_height = scheduled.shielded_epoch_reset_height = 3;
        scheduled.shielded_spend_auth_activation_height = scheduled.shielded_spend_auth_epoch_reset_height = 4;
        for (auto [height, branch] : {std::pair<uint32_t,uint32_t>{0,1}, {10,0},
                                    {uint32_t(INT32_MAX)+1,1}, {UINT32_MAX,1}}) {
            bool failed = false;
            try { ConfigureOrchardRelease(scheduled, height, branch); }
            catch (const std::invalid_argument&) { failed = true; }
            CHECK(failed);
            CHECK(scheduled.orchard_activation_height == UINT32_MAX);
            CHECK(scheduled.orchard_branch_id == 0);
            CHECK(scheduled.release_v8113_activation_height == UINT32_MAX);
            CHECK(scheduled.shielded_compact_activation_height == UINT32_MAX);
            CHECK(scheduled.sixty_second_activation_height == UINT32_MAX);
        }
        ConfigureOrchardRelease(scheduled, 10, 1); // fixture branch only
        CHECK(!OrchardActiveForHeight(scheduled, 9));
        CHECK(OrchardActiveForHeight(scheduled, 10));
        CHECK(!OrchardActiveForHeight(scheduled, 9)); // boundary rewind
        auto mismatch = scheduled;
        mismatch.sixty_second_activation_height = 11;
        CHECK(OrchardProfileConfigurationValid(mismatch) == (network == Chain::REGTEST));
        mismatch = scheduled; mismatch.orchard_activation_height = 11;
        CHECK(OrchardProfileConfigurationValid(mismatch) == (network == Chain::REGTEST));
        mismatch = scheduled; mismatch.name = "unknown";
        CHECK(!OrchardProfileConfigurationValid(mismatch));
        ConfigureOrchardRelease(scheduled, UINT32_MAX, 0);
        CHECK(!OrchardActiveForHeight(scheduled, UINT32_MAX));
        CHECK(Params().orchard_activation_height == UINT32_MAX);
    }
    SelectParams(Chain::REGTEST);
    MutableParams().orchard_activation_height = 1;
    MutableParams().orchard_branch_id = 1;
    MutableParams().regtest_enforce_pow = true;
    MutableParams().sixty_second_activation_height = 4;
    const auto genesis = BuildCanonicalGenesis(Params()).header;
    HeaderChainSelector headers;
    CHECK(headers.AddHeader(genesis));
    const auto consensus = GetConsensusForCurrentNetwork();
    std::vector<std::unique_ptr<HeaderIndexEntry>> chain;
    chain.push_back(std::make_unique<HeaderIndexEntry>(genesis, nullptr));
    const auto check = [&](const BlockHeader& header, const BlockHeader& parent, uint32_t height, uint64_t now) {
        CheckOrchardHeaderUnderChainstateLock(header, parent, Context(header, height), headers, now);
    };
    for (uint32_t height = 1; height <= 6; ++height) {
        const auto* parent = chain.back().get();
        BlockHeader child{}; child.version = 1; child.prev_block_hash = parent->hash;
        child.timestamp = genesis.timestamp + height * 120;
        child.difficulty = GetNextWorkRequiredForCandidate(height, child.timestamp, consensus,
            nullptr, parent, static_cast<NoChainDb*>(nullptr));
        CHECK(child.difficulty != 0); child = Solve(child);
        auto selected = SelectedOrchardBlockContext(child, height);
        CHECK(selected && selected->height == height && selected->activation_height == 1);
        CHECK(selected->block_hash == child.GetHash() && selected->parent_hash == parent->hash);
        CHECK(selected->domain.network_code == 2 && selected->domain.branch_id == 1);
        CHECK(selected->domain.genesis_wire == Context(child, height).domain.genesis_wire);
        check(child, parent->header, height, child.timestamp);
        CHECK(headers.GetBestHeaderValue()->hash == parent->hash); // gate does not mutate fork choice

        auto wrong = Solve(child, false);
        Rejected(OrchardHeaderErrorCode::ProofOfWork, [&] { check(wrong, parent->header, height, child.timestamp); });
        wrong = child; wrong.difficulty = child.difficulty == 0x207fffff ? 0x203fffff : 0x207fffff;
        Rejected(OrchardHeaderErrorCode::Difficulty, [&] { check(wrong, parent->header, height, child.timestamp); });
        wrong = child; wrong.timestamp = parent->GetMedianTimePast();
        Rejected(OrchardHeaderErrorCode::TimeTooOld, [&] { check(wrong, parent->header, height, child.timestamp); });
        wrong = child; wrong.version = 0;
        Rejected(OrchardHeaderErrorCode::Shape, [&] { check(wrong, parent->header, height, child.timestamp); });
        wrong = child; wrong.reserved[11] = 1;
        Rejected(OrchardHeaderErrorCode::Shape, [&] { check(wrong, parent->header, height, child.timestamp); });
        Rejected(OrchardHeaderErrorCode::TimeTooNew, [&] { check(child, parent->header, height, child.timestamp - 7201); });
        check(child, parent->header, height, child.timestamp - 7200); // exact permitted boundary
        wrong = child; wrong.timestamp = UINT64_MAX;
        Rejected(OrchardHeaderErrorCode::TimeTooNew, [&] { check(wrong, parent->header, height, child.timestamp); });
        auto context = Context(child, height); context.domain.network_code = 0;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        context = Context(child, height); context.domain.genesis_wire[0] ^= 1;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        context = Context(child, height); context.activation_height = UINT32_MAX;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        context = Context(child, height); context.activation_height = height + 1;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        context = Context(child, height); context.domain.branch_id = 0;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        context = Context(child, height); context.domain.branch_id = 2;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        if (height >= 2) {
            context = Context(child, height); context.activation_height = 2;
            Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        }
        MutableParams().orchard_activation_height = height + 1;
        CHECK(!SelectedOrchardBlockContext(child, height));
        Rejected(OrchardHeaderErrorCode::Context, [&] { check(child, parent->header, height, child.timestamp); });
        MutableParams().orchard_activation_height = 1;
        MutableParams().orchard_branch_id = 0;
        LookupFailure([&] { check(child, parent->header, height, child.timestamp); });
        MutableParams().orchard_branch_id = 1;
        context = Context(child, height); context.block_hash = genesis.GetHash();
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        context = Context(child, height); context.height = 0;
        Rejected(OrchardHeaderErrorCode::Context, [&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, context, headers, child.timestamp); });
        HeaderChainSelector absent;
        LookupFailure([&] { CheckOrchardHeaderUnderChainstateLock(child, parent->header, Context(child, height), absent, child.timestamp); });
        LookupFailure([&] { check(child, parent->header, height + 1, child.timestamp); });
        LookupFailure([&] { check(child, parent->header, height, UINT64_MAX); });
        CHECK(headers.AddHeader(child));
        chain.push_back(std::make_unique<HeaderIndexEntry>(child, parent));
    }
    // A-1 differs on this side branch. The gate must use its own anchor, even
    // though the best header already extends past the 60-second transition.
    auto fork = chain[3]->header; fork.timestamp = genesis.timestamp + 480;
    fork.difficulty = GetNextWorkRequiredForCandidate(3, fork.timestamp, consensus,
        nullptr, chain[2].get(), static_cast<NoChainDb*>(nullptr));
    fork = Solve(fork); check(fork, chain[2]->header, 3, fork.timestamp);
    CHECK(headers.AddHeader(fork));
    HeaderIndexEntry fork_parent(fork, chain[2].get());
    BlockHeader fork_child{}; fork_child.version = 1; fork_child.prev_block_hash = fork.GetHash();
    fork_child.timestamp = fork.timestamp + 60;
    fork_child.difficulty = GetNextWorkRequiredForCandidate(4, fork_child.timestamp, consensus,
        nullptr, &fork_parent, static_cast<NoChainDb*>(nullptr));
    fork_child = Solve(fork_child); check(fork_child, fork, 4, fork_child.timestamp);
    auto wrong_anchor = fork_child;
    wrong_anchor.difficulty = GetNextWorkRequiredForCandidate(4, fork_child.timestamp, consensus,
        nullptr, chain[3].get(), static_cast<NoChainDb*>(nullptr));
    CHECK(wrong_anchor.difficulty != fork_child.difficulty);
    Rejected(OrchardHeaderErrorCode::Difficulty, [&] { check(wrong_anchor, fork, 4, fork_child.timestamp); });
    // Historical ordinary-regtest bypass is explicit; it never skips MTP,
    // framing, network, parent or temporary future-time checks.
    MutableParams().regtest_enforce_pow = false;
    auto unsolved = Solve(fork_child, false);
    check(unsolved, fork, 4, fork_child.timestamp);
    unsolved.timestamp = 0;
    Rejected(OrchardHeaderErrorCode::TimeTooOld, [&] { check(unsolved, fork, 4, fork_child.timestamp); });
    const auto prior_checkpoints = Params().vCheckpoints;
    MutableParams().vCheckpoints = {{3, chain[3]->hash.GetHex()}};
    check(chain[3]->header, chain[2]->header, 3, chain[3]->header.timestamp);
    check(chain[6]->header, chain[5]->header, 6, chain[6]->header.timestamp);
    Rejected(OrchardHeaderErrorCode::Checkpoint, [&] { check(fork, chain[2]->header, 3, fork.timestamp); });
    Rejected(OrchardHeaderErrorCode::Checkpoint, [&] { check(fork_child, fork, 4, fork_child.timestamp); });
    // The selector's best chain is not the authority for the candidate fork.
    MutableParams().vCheckpoints = {{3, fork.GetHash().GetHex()}};
    check(fork_child, fork, 4, fork_child.timestamp);
    Rejected(OrchardHeaderErrorCode::Checkpoint, [&] { check(chain[6]->header, chain[5]->header, 6, chain[6]->header.timestamp); });
    // A future checkpoint must not reject an earlier valid sync prefix.
    MutableParams().vCheckpoints = {{7, fork.GetHash().GetHex()}};
    check(fork_child, fork, 4, fork_child.timestamp);
    MutableParams().vCheckpoints = {{3, "invalid-config"}};
    LookupFailure([&] { check(fork_child, fork, 4, fork_child.timestamp); });
    MutableParams().vCheckpoints = prior_checkpoints;
    std::cout << "PASS: staged headers, branch ASERT, PoW, MTP/time, branch checkpoints and selected context\n";
}
