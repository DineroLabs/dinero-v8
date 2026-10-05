#pragma once

#include "daemon/services/assumeutxo_replay.h"
#include "consensus/chainparams.h"
#include "consensus/chainwork.h"
#include "consensus/orchard_legacy_accounting.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_state_transition.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/state_commitment.h"
#include "storage/legacy_retirement.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace dinero {

// Owns independent genesis-to-activation-parent validation and legacy monetary
// accounting. Input bodies belong to one hash-linked candidate branch; no
// canonical height index, transaction index, ChainDB, service or source callback
// is consulted. The caller must run this outside every selected/wallet lock.
//
// Completion is evidence for this branch only. Before activating, the selected
// writer must still compare ALL live and durable coins, forest, frozen shielded
// state and domain to this owner and check absence of prior retirement/Orchard
// state. This type neither chooses the branch nor acknowledges any consumer.
//
// Single-threaded. Any failed Append/Finish poisons the owner, including failures
// after the replay validator has partially changed its private state. No partial
// state is exposed. Existing local work limits are explicit unavailable-policy
// bounds; this implementation makes no general-history or resident-memory claim.
class OrchardParentReplay final {
public:
    struct Target {
        uint32_t height;
        uint256 hash;
        arith_uint256 chainwork;
    };
    struct Limits {
        uint32_t blocks;
        size_t serialized_bytes;
    };

    OrchardParentReplay(Target target, Limits limits)
        : target_(std::move(target)), limits_(limits), profile_(Profile()),
          thread_(std::this_thread::get_id()) {
        const auto& params = Params();
        Require(consensus::OrchardProfileConfigurationValid(params) &&
            params.orchard_activation_height != UINT32_MAX &&
            params.orchard_activation_height != 0 &&
            uint64_t(target_.height) + 1 == params.orchard_activation_height &&
            params.shielded_activation_height < params.orchard_activation_height &&
            !target_.hash.IsNull(), "Parent replay profile unavailable");
        Require(limits_.blocks && uint64_t(target_.height) + 1 <= limits_.blocks &&
            limits_.serialized_bytes, "Parent replay exceeds local work policy");
        epoch_ = params.shielded_activation_height;
        for (uint32_t boundary : {params.shielded_epoch_reset_height,
                                  params.shielded_spend_auth_epoch_reset_height}) {
            if (boundary < params.orchard_activation_height && boundary >= epoch_) {
                epoch_ = boundary;
                reset_epoch_ = true;
            }
        }
        accounting_ = {epoch_, target_.height, target_.hash, 0, 0, 0};
    }
    OrchardParentReplay(const OrchardParentReplay&) = delete;
    OrchardParentReplay& operator=(const OrchardParentReplay&) = delete;
    OrchardParentReplay(OrchardParentReplay&&) = delete;
    OrchardParentReplay& operator=(OrchardParentReplay&&) = delete;

    // stored_work is captured with this exact block's header/height. It is only
    // a comparison input: accumulated work is independently calculated here.
    void Append(const Block& block, uint32_t height, const arith_uint256& stored_work) {
        CheckThread();
        const bool usable = !poisoned_ && !finished_;
        poisoned_ = true;
        Require(usable && Profile() == profile_, "Parent replay owner unavailable");
        Require(height == next_height_ && height <= target_.height &&
            block.header.prev_block_hash == previous_ && !block.vtx.empty(),
            "Parent replay input is not contiguous");
        const auto bytes = block.Serialize().size();
        Require(bytes <= limits_.serialized_bytes - material_, "Parent replay material limit");
        auto next_work = work_ + GetBlockProof(block.header.difficulty);
        Require(next_work >= work_ && next_work == stored_work, "Parent replay work mismatch");
        const auto hash = block.GetHash();
        if (height == target_.height)
            Require(hash == target_.hash && next_work == target_.chainwork,
                "Parent replay target mismatch");

        // Accounting is speculative until the SAME complete block passes the
        // normal stateful validator (scripts, maturity, fees, proofs, roots).
        auto next_accounting = accounting_;
        if (height >= epoch_) AccountBlock(block, height, next_accounting);
        std::string error;
        const bool valid = height == 0 ? replay_.SeedGenesis(block, error)
            : replay_.ConnectAndAdvance(block, height, hash, error);
        Require(valid, "Parent replay validation failed: " + error);
        if (height == target_.height) {
            parent_coinbase_ = block.vtx.front();
        }
        accounting_ = next_accounting;
        previous_ = hash;
        work_ = next_work;
        material_ += bytes;
        ++next_height_;
        poisoned_ = false;
    }

    void Finish() {
        CheckThread();
        const bool usable = !poisoned_ && !finished_;
        poisoned_ = true;
        Require(usable && Profile() == profile_ &&
            next_height_ == uint64_t(target_.height) + 1 && previous_ == target_.hash &&
            replay_.Height() == target_.height && work_ == target_.chainwork &&
            accounting_.blocks_read == uint64_t(target_.height) + 1 - epoch_,
            "Parent replay is incomplete");
        const auto& params = Params();
        storage::LegacyRetirementRecord result;
        result.network_code = params.name == "mainnet" ? 0 : params.name == "testnet" ? 1 :
            params.name == "regtest" ? 2 : 0xff;
        Require(result.network_code != 0xff && uint256::FromHex(params.genesis_hash, result.genesis)
            && !result.genesis.IsNull(), "Parent replay domain unavailable");
        result.branch_id = params.orchard_branch_id;
        result.activation_height = params.orchard_activation_height;
        result.legacy_epoch_height = epoch_;
        result.boundary_parent = target_.hash;
        result.retired_value = accounting_.value_una;
        const auto root = consensus::shielded::ComputeShieldedRoot(*replay_.ShieldedTree(),
            *replay_.ShieldedNullifiers(), *replay_.ShieldedAnchors());
        Require(bool(root), "Parent replay shielded root unavailable");
        result.legacy_state_root = *root;
        const auto tree_root = replay_.ShieldedTree()->Root();
        std::copy(tree_root.begin(), tree_root.end(), result.tree_root.begin());
        result.tree_size = replay_.ShieldedTree()->Size();
        const auto count = replay_.ShieldedNullifiers()->TryCount();
        Require(bool(count), "Parent replay nullifier count unavailable");
        uint64_t enumerated = 0;
        Require(replay_.ShieldedNullifiers()->ForEach([&](uint32_t height, const uint8_t*) {
            if (height < epoch_ || height > target_.height || enumerated >= *count) return false;
            ++enumerated;
            return true;
        }) && enumerated == *count, "Parent replay nullifier inventory mismatch");
        result.nullifier_count = enumerated;
        if (consensus::IsStateCommitmentActive(target_.height, params.state_commitment_activation_height)) {
            const auto commitment = consensus::FindStateCommitment(parent_coinbase_);
            Require(commitment.status == consensus::StateCommitmentStatus::Ok &&
                commitment.root == result.legacy_state_root, "Parent replay frozen commitment mismatch");
        }
        record_ = result;
        finished_ = true;
        poisoned_ = false;
    }

    const storage::LegacyRetirementRecord& Record() const { CheckFinished(); return record_; }
    const consensus::SelectedLegacyPoolAccounting& Accounting() const { CheckFinished(); return accounting_; }
    const assumeutxo::AssumeUtxoReplayEngine& ProvenState() const { CheckFinished(); return replay_; }
    const Target& ValidatedTarget() const { CheckFinished(); return target_; }

private:
    // Parameters are immutable after network selection. Check the owning
    // release/epoch domain as well so sequential test/network changes refuse.
    // This is not permission for concurrent mutation of process ChainParams.
    using ProfileIdentity = std::tuple<std::string, std::string, std::string,
        uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static ProfileIdentity Profile() {
        const auto& p = Params();
        return std::make_tuple(p.name, p.network_id, p.genesis_hash,
            p.orchard_activation_height, p.orchard_branch_id, p.release_v8113_activation_height,
            p.shielded_activation_height, p.shielded_epoch_reset_height,
            p.shielded_spend_auth_epoch_reset_height, p.state_commitment_activation_height);
    }
    static void Require(bool condition, const std::string& reason) {
        if (!condition) throw std::runtime_error(reason);
    }
    void CheckThread() const {
        Require(thread_ == std::this_thread::get_id(), "Parent replay belongs to another thread");
    }
    void CheckFinished() const {
        CheckThread();
        Require(finished_ && !poisoned_ && Profile() == profile_, "Parent replay has no completed result");
    }
    static uint64_t Add(uint64_t left, uint64_t right) {
        Require(left <= orchard::kMaxMoneyUna && right <= orchard::kMaxMoneyUna - left,
            "Parent replay monetary overflow");
        return left + right;
    }
    template<class Coin> static uint64_t Amount(const Coin& coin) {
        Require(!coin.is_confidential && coin.commitment.empty(), "Parent replay amount is confidential");
        return Add(0, coin.value.GetUna());
    }
    void AccountBlock(const Block& block, uint32_t height,
                      consensus::SelectedLegacyPoolAccounting& result) const {
        // Include only earlier outputs from this same immutable block. Prefix
        // inputs are copied from the independently validated private UTXO set,
        // never located through the current canonical transaction index.
        std::map<OutPoint, TxOutput> earlier_outputs;
        std::set<OutPoint> spent;
        for (size_t index = 0; index < block.vtx.size(); ++index) {
            const auto& tx = block.vtx[index];
            Require(tx.IsCoinbase() == (index == 0), "Parent replay coinbase position mismatch");
            if (Transaction::IsShieldedVersion(tx.version)) {
                Require(index && !(reset_epoch_ && height == epoch_), "Parent replay invalid epoch transaction");
                Require(tx.ShieldedBundleCommitsToTxid() && tx.HasExplicitFee(),
                    "Parent replay unauthenticated historical amount");
                uint64_t inputs = 0, outputs_and_fee = Add(0, tx.GetExplicitFee());
                for (const auto& input : tx.vin) {
                    const OutPoint point(input.prevout.txid, input.prevout.vout);
                    Require(!spent.count(point), "Parent replay duplicate spend");
                    const auto earlier = earlier_outputs.find(point);
                    if (earlier != earlier_outputs.end()) inputs = Add(inputs, Amount(earlier->second));
                    else {
                        const auto coin = replay_.CopyPrefixCoin(point);
                        Require(bool(coin), "Parent replay funding coin unavailable");
                        inputs = Add(inputs, Amount(*coin));
                    }
                    spent.insert(point);
                }
                for (const auto& output : tx.vout) outputs_and_fee = Add(outputs_and_fee, Amount(output));
                if (inputs >= outputs_and_fee) result.value_una = Add(result.value_una, inputs - outputs_and_fee);
                else {
                    Require(outputs_and_fee - inputs <= result.value_una, "Parent replay negative legacy pool");
                    result.value_una -= outputs_and_fee - inputs;
                }
                ++result.shielded_transactions;
            } else if (index) {
                for (const auto& input : tx.vin)
                    Require(spent.emplace(input.prevout.txid, input.prevout.vout).second,
                        "Parent replay duplicate spend");
            }
            const auto txid = tx.GetTxid();
            Require(tx.vout.size() <= UINT32_MAX, "Parent replay output index overflow");
            for (size_t n = 0; n < tx.vout.size(); ++n)
                Require(earlier_outputs.emplace(OutPoint(txid, uint32_t(n)), tx.vout[n]).second,
                    "Parent replay duplicate output");
        }
        ++result.blocks_read;
    }

    const Target target_;
    const Limits limits_;
    const ProfileIdentity profile_;
    const std::thread::id thread_;
    assumeutxo::AssumeUtxoReplayEngine replay_;
    uint32_t epoch_ = 0;
    bool reset_epoch_ = false;
    uint64_t next_height_ = 0;
    size_t material_ = 0;
    uint256 previous_;
    arith_uint256 work_{0};
    consensus::SelectedLegacyPoolAccounting accounting_{};
    Transaction parent_coinbase_;
    storage::LegacyRetirementRecord record_;
    bool poisoned_ = false;
    bool finished_ = false;
};
} // namespace dinero
