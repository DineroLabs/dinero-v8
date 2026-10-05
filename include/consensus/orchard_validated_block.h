#pragma once
#include "consensus/orchard_block_coins.h"
#include "consensus/chainparams.h"
#include "storage/legacy_retirement.h"
#include "crypto/sha256.h"
#include <map>
#include <sstream>

namespace dinero { class OrchardBranchReplay; class ChainstateService; }

namespace dinero::consensus {
// Created by complete private branch verification or the service-owned
// extension completion after detached proofs and checked state commitment.
// The branch verifier does not
// expose a block result until every block through its requested target passes.
// Reuse still requires canonical before-image, profile, parent and exact-wire
// checks under the selected writer. Possessing this object is not permission
// to skip those checks or to publish a partially verified replacement branch.
class ValidatedOrchardBlock final {
public:
    const auto& Context() const noexcept { return context_; }
    const auto& ParentHeader() const noexcept { return parent_header_; }
    const auto& Coins() const noexcept { return coins_; }
    const auto& State() const noexcept { return state_; }
    const auto& Retirement() const noexcept { return retirement_; }
    const auto& ParentMembership() const noexcept { return parent_membership_; }
    const auto& ParentForestRoot() const noexcept { return parent_forest_root_; }
    const auto& BranchMtp() const noexcept { return branch_mtp_; }
    bool WitnessRequired() const noexcept { return witness_; }
    bool MatchesProfile() const { return profile_ == CaptureProfile(); }
    bool MatchesWire(const OrchardBlockCandidate& block) const {
        return block.WireBytes().size() == wire_size_ && WireDigest(block) == wire_digest_;
    }
    ValidatedOrchardBlock(const ValidatedOrchardBlock&) = delete;
    ValidatedOrchardBlock& operator=(const ValidatedOrchardBlock&) = delete;
private:
    friend class ::dinero::OrchardBranchReplay;
    friend class ::dinero::ChainstateService; // Private captured-extension completion only.
    // Include validation controls omitted by the operator-facing checksum.
    // Network parameters remain immutable during a process; this also refuses
    // reuse across serialized test/profile changes. It does not authorize
    // concurrent mutation of the global ChainParams object.
    static std::string CaptureProfile() {
        const auto& p = Params();
        std::ostringstream out;
        out << ConsensusChecksum(p) << '\n' << p.name << '\n' << p.network_id << '\n'
            << p.genesis_hash << '\n' << p.orchard_activation_height << '\n'
            << p.orchard_branch_id << '\n' << p.release_v8113_activation_height << '\n'
            << p.max_block_size << '\n' << p.coinbase_maturity << '\n'
            << p.allow_min_difficulty << '\n' << p.require_standard_txs << '\n'
            << p.mine_blocks_on_demand << '\n' << p.regtest_enforce_pow << '\n'
            << p.enable_witness_magic_translation << '\n' << p.witness_magic_translation_height << '\n'
            << p.disable_confidential_transactions << '\n' << p.confidential_activation_height << '\n'
            << p.nMinimumChainWork << '\n' << p.defaultAssumeValid << '\n' << p.assumeValidHeight << '\n';
        for (const auto& [height, hash] : p.vCheckpoints) out << height << ':' << hash << '\n';
        return out.str();
    }
    static uint256 WireDigest(const OrchardBlockCandidate& block) {
        uint256 digest;
        crypto::CSHA256().Write(block.WireBytes().data(),block.WireBytes().size()).Finalize(digest.data);
        return digest;
    }
    ValidatedOrchardBlock(std::string profile, const OrchardBlockCandidate& block,
        OrchardBlockContext context, BlockHeader parent, PreparedOrchardBlockCoins coins,
        PreparedOrchardState state, storage::LegacyRetirementRecord retirement,
        storage::OrchardCommitmentSets parent_membership, uint256 parent_forest_root,
        std::map<uint32_t, uint64_t> branch_mtp, bool witness)
        : profile_(std::move(profile)), wire_digest_(WireDigest(block)), wire_size_(block.WireBytes().size()),
          context_(std::move(context)), parent_header_(std::move(parent)), coins_(std::move(coins)),
          state_(std::move(state)), retirement_(std::move(retirement)),
          parent_membership_(std::move(parent_membership)), parent_forest_root_(parent_forest_root),
          branch_mtp_(std::move(branch_mtp)), witness_(witness) {}
    const std::string profile_;
    const uint256 wire_digest_;
    const size_t wire_size_;
    const OrchardBlockContext context_;
    const BlockHeader parent_header_;
    const PreparedOrchardBlockCoins coins_;
    const PreparedOrchardState state_;
    const storage::LegacyRetirementRecord retirement_;
    const storage::OrchardCommitmentSets parent_membership_;
    const uint256 parent_forest_root_;
    const std::map<uint32_t, uint64_t> branch_mtp_;
    const bool witness_;
};
} // namespace dinero::consensus
