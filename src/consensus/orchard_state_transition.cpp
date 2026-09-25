#include "consensus/orchard_state_transition.h"
#include <algorithm>
#include <limits>
#include <set>

namespace dinero::consensus {
namespace {
using Error = OrchardStateErrorCode;
[[noreturn]] void Fail(Error code) { throw OrchardStateError(code); }
uint256 WireHash(std::span<const uint8_t, 32> bytes) {
    uint256 hash; std::copy(bytes.begin(), bytes.end(), hash.begin()); return hash;
}
bool Lookup(const std::function<StatusOr<bool>(const uint256&)>& fn, const uint256& key) {
    if (!fn) throw OrchardStateLookupError(Status::Internal);
    const auto result = fn(key);
    if (!result.ok()) throw OrchardStateLookupError(result.status());
    return result.value();
}
} // namespace
PreparedOrchardState PrepareOrchardStateTransition(const OrchardBlockContext& context,
    const std::optional<storage::OrchardStoredState>& parent,
    std::span<const VerifiedOrchardAuthorizations> transactions, const OrchardStateLookups& lookups) {
    if (context.activation_height == UINT32_MAX || context.activation_height == 0 ||
        context.height < context.activation_height) Fail(Error::Inactive);
    if (context.height == 0 || context.height > uint32_t(std::numeric_limits<int32_t>::max()) ||
        context.block_hash.IsNull() || context.parent_hash.IsNull() ||
        context.block_hash == context.parent_hash) Fail(Error::Context);
    // Validate even empty-block signing domains; no default implicit mainnet.
    (void)orchard::SigningContext::Create(context.domain, 0, {}, {}, 0);
    if (transactions.size() > storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT) Fail(Error::ResourceLimit);
    auto frontier = std::make_unique<orchard::OrchardFrontier>(orchard::OrchardFrontier::Empty());
    const auto empty_root = WireHash(frontier->Root());
    uint64_t balance = 0;
    if (context.height == context.activation_height) {
        if (parent) Fail(Error::ParentState);
    } else {
        if (!parent || parent->height != context.height - 1 || parent->block_hash != context.parent_hash ||
            parent->pool_balance > orchard::kMaxMoneyUna) Fail(Error::ParentState);
        try {
            frontier = std::make_unique<orchard::OrchardFrontier>(orchard::OrchardFrontier::Decode(
                {reinterpret_cast<const uint8_t*>(parent->frontier.data()), parent->frontier.size()}));
        } catch (const orchard::BackendError& e) {
            if (e.Status() == DINERO_ORCHARD_PANIC) throw;
            Fail(Error::ParentState);
        }
        if (frontier->Size() != parent->tree_size || WireHash(frontier->Root()) != parent->anchor)
            Fail(Error::ParentState);
        balance = parent->pool_balance;
    }
    std::vector<OrchardValueFlow> flows;
    std::vector<uint256> nullifiers;
    std::set<uint256> seen_nullifiers;
    std::set<orchard::Hash> seen_transactions;
    std::set<std::pair<orchard::Hash, uint32_t>> seen_inputs;
    uint64_t fees = 0;
    for (const auto& verified : transactions) {
        const auto& transparent = verified.Transparent();
        const auto& tx = verified.Transaction();
        if (transparent.CandidateHeight() != context.height ||
            transparent.Snapshot().ViewHeight() != context.height - 1 ||
            transparent.Snapshot().SigningDigest(context.domain) != transparent.OrchardIntent())
            Fail(Error::Context);
        if (!seen_transactions.insert(verified.Orchard().Txid()).second) Fail(Error::DuplicateTransaction);
        for (const auto& input : tx.Inputs())
            if (!seen_inputs.emplace(input.txid_wire, input.output_index).second) Fail(Error::DuplicateInput);
        const auto& facts = verified.Orchard().Orchard().Facts();
        if (facts.action_count == 0 || facts.action_count > orchard::kMaxActionsV1 ||
            facts.action_count > storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT - nullifiers.size())
            Fail(Error::ResourceLimit);
        const auto anchor = WireHash(facts.anchor);
        // Draft eligibility: empty root or a root of the selected active parent
        // history. Never accept roots produced earlier inside this block.
        constexpr uint8_t spends_enabled_flag = 0x01; // Orchard flag-byte bit 0.
        if ((facts.flags & spends_enabled_flag) && anchor != empty_root &&
            !Lookup(lookups.active_anchor, anchor)) Fail(Error::Anchor);
        std::vector<orchard::Hash> commitments;
        for (size_t i = 0; i < facts.action_count; ++i) {
            const auto nf = WireHash(facts.nullifiers[i]);
            if (!seen_nullifiers.insert(nf).second) Fail(Error::DuplicateNullifier);
            if (Lookup(lookups.spent_nullifier, nf)) Fail(Error::SpentNullifier);
            nullifiers.push_back(nf);
            orchard::Hash cmx;
            std::copy(std::begin(facts.commitments[i]), std::end(facts.commitments[i]), cmx.begin());
            commitments.push_back(cmx);
        }
        const auto flow = GetOrchardValueFlow(transparent);
        const auto updated = ApplyOrchardValueFlows(balance, {flow});
        if (!updated.ok()) Fail(Error::PoolBalance);
        if (flow.fee > orchard::kMaxMoneyUna - fees) Fail(Error::ResourceLimit);
        fees += flow.fee;
        balance = updated.value(); flows.push_back(flow);
        frontier = std::make_unique<orchard::OrchardFrontier>(frontier->Append(commitments));
    }
    storage::OrchardStoredState next{context.height, context.block_hash, WireHash(frontier->Root()),
        balance, frontier->Size(), std::string(frontier->Bytes().begin(), frontier->Bytes().end())};
    return PreparedOrchardState(parent, std::move(next), std::move(nullifiers), std::move(flows), fees);
}
} // namespace dinero::consensus
