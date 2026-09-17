/**
 * Header Proof-of-Work Verification Test
 *
 * Regression for the header-PoW gap: HeaderChainSelector::ValidateHeader used to
 * accept headers without checking hash <= target. Because fork-choice credits
 * chainwork = GetBlockProof(header.difficulty) from the *claimed* bits, a peer
 * could submit headers claiming arbitrarily hard difficulty with NO real work,
 * win UpdateBestHeader(), and steer block download toward a forged chain — a
 * zero-cost sync-stall / eclipse vector. Full blocks are still rejected at
 * connect (block_acceptor), so impact is availability, not theft.
 *
 * This test runs on MAINNET (where PoW enforcement is active; regtest skips it,
 * matching block_acceptor's PATH A) and asserts:
 *   1. The real genesis (valid PoW, nonce 813915426) is accepted.
 *   2. A child claiming hard difficulty with no real work is REJECTED.
 *   3. The forged header does NOT become the best header (no forged-chainwork
 *      takeover of fork-choice).
 */

#include "consensus/header_chain.h"
#include "consensus/pow.h"
#include "consensus/pow.hpp"
#include "daemon/regtest_pow_profile.h"
#include <filesystem>
#include <chrono>
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include <iostream>
#include <cstdlib>

// Consensus checks must run even when an ad-hoc Release build defines NDEBUG.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ \
                  << ": " #condition << std::endl; \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

using namespace dinero;
using namespace dinero::consensus;

int main() {
    // Mainnet: header PoW enforcement is active (regtest would skip it).
    SelectParams(Chain::MAINNET);
    std::cout << "=== Header PoW Verification Test (mainnet) ===" << std::endl;

    HeaderChainSelector selector;

    // ------------------------------------------------------------------
    // 1. The real genesis carries valid PoW and MUST be accepted.
    //    (It flows through ValidateHeader at startup seed time, so a
    //     too-strict PoW check here would brick the node — verify it passes.)
    // ------------------------------------------------------------------
    BlockHeader genesis = BuildCanonicalGenesis(Params()).header;
    bool genesis_ok = selector.AddHeader(genesis);
    CHECK(genesis_ok && "real genesis (valid PoW) must be accepted");
    std::cout << "  [1] real genesis accepted: OK" << std::endl;

    const auto best_after_genesis = selector.GetBestHeaderValue();
    CHECK(best_after_genesis.has_value() &&
           best_after_genesis->hash == genesis.GetHash());

    // ------------------------------------------------------------------
    // 2. Forged child: claims a HARDER target than genesis (0x1d00ffff →
    //    large GetBlockProof chainwork) but is not mined (nonce=1, arbitrary
    //    content) so its hash will not meet that target. Must be rejected.
    // ------------------------------------------------------------------
    BlockHeader forged{};
    forged.version = 1;
    forged.prev_block_hash = genesis.GetHash();
    forged.merkle_root = uint256();
    forged.utreexo_root = uint256();
    forged.timestamp = genesis.timestamp + 120;  // > parent median-time-past
    forged.difficulty = 0x1d00ffff;              // harder than genesis (0x1d31ffce)
    forged.nonce = 1;                            // not mined → hash won't meet target

    bool forged_added = selector.AddHeader(forged);
    CHECK(!forged_added &&
           "forged hard-difficulty header with no PoW must be rejected");
    std::cout << "  [2] forged no-PoW child rejected: OK" << std::endl;

    // ------------------------------------------------------------------
    // 3. Best header must remain genesis — the forged header (which claimed
    //    huge chainwork) did NOT win fork-choice.
    // ------------------------------------------------------------------
    const auto best_after_forge = selector.GetBestHeaderValue();
    CHECK(best_after_forge.has_value() &&
           best_after_forge->hash == genesis.GetHash() &&
           "forged header must not become best (no forged-chainwork takeover)");
    std::cout << "  [3] best header unchanged (no forged-chainwork takeover): OK"
              << std::endl;

    // The test-only switch must remove both the PoW and fixed-difficulty
    // bypasses, without changing ordinary regtest or other network defaults.
    SelectParams(Chain::REGTEST);
    const auto original = Params();
    const auto original_checksum = ConsensusChecksum(Params());
    CHECK(Params().SkipProofOfWork());
    MutableParams().regtest_enforce_pow = true;
    MutableParams().sixty_second_activation_height = 4;
    CHECK(!Params().SkipProofOfWork());
    CHECK(ConsensusChecksum(Params()) != original_checksum);
    HeaderChainSelector enforced;
    CHECK(enforced.AddHeader(BuildCanonicalGenesis(Params()).header));
    const auto consensus = GetConsensusForCurrentNetwork();
    std::vector<std::unique_ptr<HeaderIndexEntry>> ancestry;
    ancestry.push_back(std::make_unique<HeaderIndexEntry>(genesis, nullptr));
    const auto solve = [](BlockHeader header, bool valid) {
        for (uint32_t nonce = 0; nonce < 4'000'000; ++nonce) {
            header.nonce = nonce;
            if (CheckProofOfWork(header, false) == valid) return header;
        }
        throw std::runtime_error("test nonce search exhausted");
    };
    for (uint32_t height = 1; height <= 6; ++height) {
        const auto* parent = ancestry.back().get();
        CHECK(enforced.GetBestHeaderValue()->hash == parent->hash);
        BlockHeader child{};
        child.version = 1;
        child.prev_block_hash = parent->hash;
        child.timestamp = genesis.timestamp + height * 120;
        child.difficulty = GetNextWorkRequiredForCandidate(
            height, child.timestamp, consensus, nullptr, parent,
            static_cast<NoChainDb*>(nullptr));
        CHECK(child.difficulty != 0);
        CHECK(!enforced.AddHeader(solve(child, false)));
        auto wrong = child;
        wrong.difficulty = child.difficulty == 0x207fffff ? 0x203fffff : 0x207fffff;
        CHECK(!enforced.AddHeader(solve(wrong, true)));
        CHECK(enforced.GetBestHeaderValue()->hash == parent->hash);
        child = solve(child, true);
        CHECK(enforced.AddHeader(child));
        CHECK(enforced.GetBestHeaderValue()->hash == child.GetHash());
        ancestry.push_back(std::make_unique<HeaderIndexEntry>(child, parent));
    }
    // A competing branch has its own A-1 time AND bits. Validate it against
    // that anchor even while the active best chain already extends past A.
    BlockHeader fork = ancestry[3]->header;
    fork.timestamp = genesis.timestamp + 480;
    fork.difficulty = GetNextWorkRequiredForCandidate(3, fork.timestamp, consensus,
        nullptr, ancestry[2].get(), static_cast<NoChainDb*>(nullptr));
    fork = solve(fork, true);
    CHECK(enforced.AddHeader(fork));
    HeaderIndexEntry fork_parent(fork, ancestry[2].get());
    BlockHeader fork_child{};
    fork_child.version = 1;
    fork_child.prev_block_hash = fork.GetHash();
    fork_child.timestamp = fork.timestamp + 60;
    fork_child.difficulty = GetNextWorkRequiredForCandidate(4, fork_child.timestamp,
        consensus, nullptr, &fork_parent, static_cast<NoChainDb*>(nullptr));
    auto wrong_anchor = fork_child;
    wrong_anchor.difficulty = GetNextWorkRequiredForCandidate(4, fork_child.timestamp,
        consensus, nullptr, ancestry[3].get(), static_cast<NoChainDb*>(nullptr));
    CHECK(wrong_anchor.difficulty != fork_child.difficulty);
    CHECK(!enforced.AddHeader(solve(wrong_anchor, true)));
    fork_child = solve(fork_child, true);
    CHECK(enforced.AddHeader(fork_child));
    CHECK(enforced.GetHeaderValue(fork_child.GetHash()).has_value());

    // Profile state cannot be silently reused with different parameters or
    // ordinary regtest. Malformed markers fail closed; old datadirs are refused.
    const auto directory = std::filesystem::temp_directory_path() /
        ("pow-profile-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto must_throw = [](auto function) {
        bool rejected = false;
        try { function(); } catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected);
    };
    dinero::daemon::CheckRegtestPowDatadir(directory, true);
    const auto profile = ConsensusChecksum(Params());
    dinero::daemon::BindRegtestPowProfile(directory, profile);
    dinero::daemon::BindRegtestPowProfile(directory, profile);
    must_throw([&] { dinero::daemon::BindRegtestPowProfile(directory, original_checksum); });
    must_throw([&] { dinero::daemon::CheckRegtestPowDatadir(directory, false); });
    std::ofstream(directory / "regtest-pow-profile") << "truncated";
    must_throw([&] { dinero::daemon::BindRegtestPowProfile(directory, profile); });
    std::filesystem::remove(directory / "regtest-pow-profile");
    std::ofstream(directory / "old-chain") << "do not migrate";
    must_throw([&] { dinero::daemon::CheckRegtestPowDatadir(directory, true); });
    std::filesystem::remove_all(directory);
    MutableParams() = original;
    CHECK(Params().SkipProofOfWork());
    CHECK(ConsensusChecksum(Params()) == original_checksum);

    std::cout << "✅ Header PoW verification enforced" << std::endl;
    return 0;
}
