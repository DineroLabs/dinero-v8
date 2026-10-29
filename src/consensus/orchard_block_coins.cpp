#include "consensus/orchard_block_coins.h"
#include "consensus/orchard_candidate_coin_view.h"
#include "consensus/orchard_catalog_coin_view.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/orchard_resources.h"
#include "consensus/block_reward.h"
#include "consensus/chainparams.h"
#include "consensus/contextual_locks.h"
#include "consensus/covenants.h"
#include "consensus/script_validation.h"
#include <map>
#include <set>

namespace dinero::consensus {
namespace {
using Error = OrchardBlockCoinErrorCode;
[[noreturn]] void Reject(Error code) { throw OrchardBlockCoinError(code); }
void AddAmount(uint64_t amount, uint64_t& total) {
    if (amount > orchard::kMaxMoneyUna || total > orchard::kMaxMoneyUna - amount) Reject(Error::Amount);
    total += amount;
}
OutPoint Point(const orchard::EnvelopeInput& input) {
    uint256 hash; std::copy(input.txid_wire.begin(), input.txid_wire.end(), hash.begin());
    return {TxId(hash), input.output_index};
}
// Null entries are tombstones. Never fall through to the parent after a spend.
class OrderedView final : public ChainStateView {
public:
    explicit OrderedView(const ChainStateView& parent) : parent_(parent), height_(parent.getHeight()) {}
    StatusOr<UTXOEntry> getCoin(const OutPoint& point) const override {
        auto it = current_.find(point);
        if (it == current_.end()) {
            const auto result = parent_.getCoin(point);
            if (!result.ok() && result.status() != Status::NotFound) throw OrchardCoinLookupError(result.status());
            const auto value = result.ok() ? std::optional<UTXOEntry>(result.value()) : std::nullopt;
            before_.emplace(point, value);
            it = current_.emplace(point, value).first;
        }
        if (!it->second) return Status::NotFound;
        return *it->second;
    }
    bool hasCoin(const OutPoint& point) const override { return getCoin(point).ok(); }
    uint32_t getHeight() const override { return height_; }
    void Spend(const OutPoint& point) {
        if (!getCoin(point).ok()) Reject(Error::MissingCoin);
        current_.at(point).reset(); touched_.insert(point);
    }
    void Create(const OutPoint& point, const UTXOEntry& coin) {
        if (getCoin(point).ok() || before_.at(point)) Reject(Error::OutputCollision);
        current_.at(point) = coin; touched_.insert(point);
    }
    std::vector<OrchardCoinChange> Changes() const {
        std::vector<OrchardCoinChange> result;
        for (const auto& point : touched_) {
            const auto& before = before_.at(point);
            const auto& after = current_.at(point);
            // An output created and spent in this block has no persistent row.
            // Its ordered spend/create records remain available for the forest.
            if (before || after) result.push_back({point, before, after});
        }
        return result;
    }
private:
    const ChainStateView& parent_;
    const uint32_t height_;
    mutable std::map<OutPoint, std::optional<UTXOEntry>> before_, current_;
    std::set<OutPoint> touched_;
};
std::vector<UTXOEntry> Resolve(const Transaction& tx, const OrderedView& view, uint32_t height) {
    std::vector<UTXOEntry> coins;
    std::set<OutPoint> seen;
    uint64_t total = 0;
    for (const auto& input : tx.vin) {
        const OutPoint point(input.prevout.txid, input.prevout.vout);
        if (!seen.insert(point).second) Reject(Error::DuplicateInput);
        const auto result = view.getCoin(point);
        if (!result.ok()) Reject(Error::MissingCoin);
        const auto& coin = *result;
        if (coin.height > height || (coin.isCoinbase && height - coin.height < 100)) Reject(Error::ImmatureCoinbase);
        if (coin.is_confidential || !coin.commitment.empty()) Reject(Error::Amount);
        AddAmount(coin.value.GetUna(), total);
        coins.push_back(coin);
    }
    return coins;
}
struct CheckedSequence {
    std::vector<OrchardTransactionCoins> transactions;
    std::vector<OrchardCoinChange> changes;
    std::vector<VerifiedOrchardAuthorizations> authorizations;
    uint64_t fees;
    OrchardResourceUsage resources;
};
CheckedSequence CheckSequence(std::span<const ParsedTransaction> transactions,
    const OrchardTransactionContext& context, const ChainStateView& parent,
    const OrchardBranchMtpLookup& mtp, bool with_coinbase) {
    if (context.activation_height == UINT32_MAX || context.activation_height == 0 ||
        context.height < context.activation_height || context.height == 0 ||
        context.height > INT32_MAX || parent.getHeight() != context.height - 1)
        Reject(Error::Context);
    std::string error;
    OrchardResourceUsage resources;
    for (const auto& parsed : transactions)
        AccumulateOrchardTransactionResources(parsed, resources);
    OrderedView view(parent);
    std::set<TxId> ids;
    std::vector<OrchardTransactionCoins> records;
    std::vector<VerifiedOrchardAuthorizations> authorizations;
    uint64_t total_fees = 0;
    for (size_t index = 0; index < transactions.size(); ++index) {
        const auto& parsed = transactions[index];
        OrchardTransactionCoins record; record.txid = parsed.GetTxid(); record.coinbase = with_coinbase && index == 0;
        if (!ids.insert(record.txid).second) Reject(Error::DuplicateTransaction);
        if (parsed.IsOrchard()) {
            try {
                const auto snapshot = OrchardCoinSnapshot::ResolveUnderChainstateLock(parsed.Orchard(), view);
                AccumulateOrchardInputResources(parsed, snapshot.Coins(), resources);
                authorizations.push_back(VerifyOrchardAuthorizations(snapshot, context.domain, context.height, mtp));
            } catch (const OrchardCoinLookupError& e) {
                if (e.SourceStatus() == Status::NotFound) Reject(Error::MissingCoin);
                throw;
            }
            const auto& auth = authorizations.back();
            const auto& snapshot = auth.Transparent().Snapshot();
            const auto flow = GetOrchardValueFlow(auth.Transparent());
            record.fee = flow.fee;
            for (size_t i = 0; i < parsed.Orchard().Inputs().size(); ++i)
                record.spent.emplace_back(Point(parsed.Orchard().Inputs()[i]), snapshot.Coins()[i]);
            for (size_t i = 0; i < parsed.Orchard().Outputs().size(); ++i) {
                const auto& output = parsed.Orchard().Outputs()[i];
                record.created.emplace_back(OutPoint(record.txid, static_cast<uint32_t>(i)),
                    UTXOEntry(AmountUna::Una(output.amount_una), output.script_pub_key, context.height, false));
            }
        } else {
            const auto& tx = parsed.Historical();
            if (Transaction::IsShieldedVersion(tx.version) || !tx.shielded_bundle_bytes.empty()) Reject(Error::RetiredPool);
            if (tx.IsCoinbase() != record.coinbase || tx.vin.empty() || tx.vout.empty()) Reject(Error::Body);
            if (tx.HasConfidentialOutputs()) Reject(Error::Amount);
            uint64_t output_total = 0;
            for (size_t i = 0; i < tx.vout.size(); ++i) {
                const auto& output = tx.vout[i];
                if (!output.commitment.empty()) Reject(Error::Amount);
                AddAmount(output.value.GetUna(), output_total);
                record.created.emplace_back(OutPoint(record.txid, static_cast<uint32_t>(i)),
                    UTXOEntry(output.value, output.scriptPubKey, context.height, record.coinbase));
            }
            if (!record.coinbase) {
                const auto coins = Resolve(tx, view, context.height);
                AccumulateOrchardInputResources(parsed, coins, resources);
                uint64_t input_total = 0;
                std::vector<std::optional<uint32_t>> heights;
                for (const auto& coin : coins) { AddAmount(coin.value.GetUna(), input_total); heights.push_back(coin.height); }
                if (!CheckContextualLocks(tx, context.height, Params().contextual_locks_activation_height, heights, mtp, error))
                    Reject(Error::Locks);
                const PrecomputedTransactionData precomputed(tx, coins);
                for (size_t i = 0; i < tx.vin.size(); ++i) {
                    if (ValidateSpend(tx, i, coins[i], context.height, coins, &precomputed) != ScriptValidationResult::OK)
                        Reject(Error::Script);
                    record.spent.emplace_back(OutPoint(tx.vin[i].prevout.txid, tx.vin[i].prevout.vout), coins[i]);
                }
                if (!reward_detail::ComputeValidatedTransactionFee(tx, coins, input_total, output_total, record.fee, error))
                    Reject(Error::Amount);
                // Ordinary transparent transactions contribute zero to the pool.
                // Their actual validated fee, not any declaration, enters reward.
            }
        }
        AddAmount(record.fee, total_fees);
        for (const auto& [point, coin] : record.spent) view.Spend(point);
        for (const auto& [point, coin] : record.created) view.Create(point, coin);
        records.push_back(std::move(record));
    }
    if (parent.getHeight() != context.height - 1)
        throw OrchardCoinLookupError(Status::Internal);
    return {std::move(records), view.Changes(), std::move(authorizations),
            total_fees, resources};
}
} // namespace

CheckedOrchardTransactionCoins CheckOrchardTransactionCoinsUnderChainstateLock(
    std::span<const ParsedTransaction> transactions, const OrchardTransactionContext& context,
    const ChainStateView& parent, const OrchardBranchMtpLookup& mtp) {
    auto checked = CheckSequence(transactions, context, parent, mtp, false);
    return CheckedOrchardTransactionCoins(std::move(checked.transactions),
        std::move(checked.changes), std::move(checked.authorizations),
        checked.fees, checked.resources);
}

PreparedOrchardBlockCoins PrepareOrchardBlockCoinsUnderChainstateLock(
    const OrchardBlockCandidate& block, const OrchardBlockContext& context,
    const ChainStateView& parent, const OrchardBranchMtpLookup& mtp, bool require_witness_commitment) {
    if (context.activation_height == UINT32_MAX || context.activation_height == 0 ||
        context.height < context.activation_height || context.height == 0 ||
        context.height > INT32_MAX || parent.getHeight() != context.height - 1 ||
        block.Header().GetHash() != context.block_hash || block.Header().prev_block_hash != context.parent_hash)
        Reject(Error::Context);
    std::string error;
    if (!block.Header().IsReservedValid() || !block.CheckSizeLimits(error) ||
        !block.CheckIdentityCommitments(require_witness_commitment, error) ||
        !block.CheckCoinbaseHeight(context.height, error))
        Reject(Error::Body);
    const OrchardTransactionContext transaction_context{
        context.height, context.parent_hash, context.activation_height, context.domain};
    auto checked = CheckSequence(block.Transactions(), transaction_context, parent, mtp, true);
    if (!reward_detail::CheckCoinbaseReward(block.Transactions().front().Historical(),
                                            context.height, checked.fees, error))
        Reject(Error::Reward);
    return PreparedOrchardBlockCoins(context.block_hash, context.parent_hash, context.height,
        std::move(checked.transactions), std::move(checked.changes),
        std::move(checked.authorizations), checked.fees, checked.resources);
}
} // namespace dinero::consensus

namespace dinero::consensus {
namespace {
using CaptureError = OrchardCandidateCoinErrorCode;
[[noreturn]] void RejectCapture(CaptureError code) { throw OrchardCandidateCoinError(code); }
}
OrchardCandidateCoinView OrchardCandidateCoinView::Capture(
    const OrchardBlockCandidate& block, const OrchardBlockContext& context,
    const BlockHeader& header, const UtreexoStump& stump,
    const ChainStateView& parent, const TransactionMembership& transactions) {
    const auto root = stump.getCommitment();
    std::string error;
    if (!context.height || context.height > INT32_MAX || context.activation_height == UINT32_MAX ||
        !context.activation_height || context.height < context.activation_height ||
        parent.getHeight() != context.height - 1 || !transactions ||
        header.GetHash() != context.parent_hash || block.Header().prev_block_hash != context.parent_hash ||
        block.Header().GetHash() != context.block_hash || !header.IsReservedValid() ||
        root.size() != 32 || !std::equal(root.begin(), root.end(), header.utreexo_root.begin()) ||
        !block.CheckSizeLimits(error)) RejectCapture(CaptureError::Context);
    if (!block.Utreexo()) RejectCapture(CaptureError::Proof);
    const auto& data = *block.Utreexo();
    const auto& proof = data.spend_proof;
    if (data.accumulator_root_before != root || proof.numLeaves != stump.getNumLeaves() ||
        proof.format_version != GetUtreexoProofFormatVersion(context.height) || !proof.isValid())
        RejectCapture(CaptureError::Proof);
    OrchardCandidateCoinView result(context.height - 1, context.parent_hash, context.block_hash);
    struct Parts {
        std::vector<OutPoint> spent;
        std::vector<std::pair<OutPoint, UTXOEntry>> created;
    };
    std::vector<Parts> ordered;
    std::set<TxId> ids;
    for (const auto& parsed : block.Transactions()) {
        const auto id = parsed.GetTxid();
        if (!ids.insert(id).second) RejectCapture(CaptureError::DuplicateTransaction);
        const auto present = transactions(id);
        if (!present.ok()) throw OrchardCoinLookupError(present.status());
        if (*present) RejectCapture(CaptureError::HistoricalTransaction);
        Parts part;
        if (parsed.IsOrchard()) {
            for (const auto& input : parsed.Orchard().Inputs()) {
                uint256 hash;
                std::copy(input.txid_wire.begin(), input.txid_wire.end(), hash.begin());
                part.spent.emplace_back(TxId(hash), input.output_index);
            }
            uint32_t index = 0;
            for (const auto& output : parsed.Orchard().Outputs())
                part.created.emplace_back(OutPoint(id, index++), UTXOEntry(
                    AmountUna::Una(output.amount_una), output.script_pub_key, context.height, false));
        } else {
            const auto& tx = parsed.Historical();
            if (!tx.IsCoinbase()) for (const auto& input : tx.vin)
                part.spent.emplace_back(input.prevout.txid, input.prevout.vout);
            for (uint32_t index = 0; index < tx.vout.size(); ++index) {
                const auto& output = tx.vout[index];
                part.created.emplace_back(OutPoint(id, index), UTXOEntry(output.value, output.scriptPubKey,
                    context.height, tx.IsCoinbase(), output.is_confidential, output.commitment));
            }
        }
        for (const auto& [point, coin] : part.created) {
            const auto existing = parent.getCoin(point);
            if (existing.ok()) RejectCapture(CaptureError::OutputCollision);
            if (existing.status() != Status::NotFound) throw OrchardCoinLookupError(existing.status());
            if (!result.absent_.insert(point).second) RejectCapture(CaptureError::DuplicateTransaction);
        }
        ordered.push_back(std::move(part));
    }
    std::map<OutPoint, UTXOEntry> prior_outputs;
    std::set<OutPoint> spent;
    std::set<UtreexoHash> expected;
    size_t index = 0;
    for (const auto& tx : ordered) {
        for (const auto& point : tx.spent) {
            if (!spent.insert(point).second) RejectCapture(CaptureError::DuplicateInput);
            const auto internal = prior_outputs.find(point);
            const bool same_block = internal != prior_outputs.end();
            const auto coin = [&]() {
                if (same_block) return internal->second;
                if (result.absent_.contains(point)) RejectCapture(CaptureError::InputOrder);
                const auto found = parent.getCoin(point);
                if (!found.ok()) throw OrchardCoinLookupError(found.status());
                if (found->height > context.height - 1) throw OrchardCoinLookupError(Status::Corruption);
                return *found;
            }();
            if (index == data.spent_outputs.size()) RejectCapture(CaptureError::Metadata);
            const auto& metadata = data.spent_outputs[index++];
            if (metadata.value != coin.value.GetUna() || metadata.scriptPubKey != coin.scriptPubKey ||
                metadata.created_height != coin.height || metadata.is_coinbase != coin.isCoinbase ||
                metadata.is_confidential != coin.is_confidential || metadata.commitment != coin.commitment)
                RejectCapture(CaptureError::Metadata);
            if (!same_block) {
                result.inputs_.emplace(point, coin);
                if (!expected.insert(HashUTXOForCreationHeight(point.txid.AsUint256(), point.vout,
                    coin.value.GetUna(), coin.scriptPubKey, coin.height, coin.isCoinbase)).second)
                    RejectCapture(CaptureError::Proof);
            }
        }
        for (const auto& [point, coin] : tx.created)
            if (!prior_outputs.emplace(point, coin).second) RejectCapture(CaptureError::DuplicateTransaction);
    }
    if (index != data.spent_outputs.size() || expected.size() != proof.targets.size() ||
        proof.positions.size() != proof.targets.size()) RejectCapture(CaptureError::Proof);
    std::set<UtreexoHash> provided;
    std::set<uint64_t> positions;
    size_t siblings = 0;
    for (size_t i = 0; i < proof.targets.size(); ++i) {
        if (proof.targets[i].size() != 32 || !provided.insert(proof.targets[i]).second ||
            proof.positions[i] >= proof.numLeaves || !positions.insert(proof.positions[i]).second)
            RejectCapture(CaptureError::Proof);
        uint64_t start = 0;
        for (int height = 63; height >= 0; --height) if ((proof.numLeaves >> height) & 1) {
            const uint64_t size = uint64_t(1) << height;
            if (proof.positions[i] >= start && proof.positions[i] - start < size) {
                siblings += size_t(height); break;
            }
            start += size;
        }
    }
    if (provided != expected || siblings != proof.proof_hashes.size()) RejectCapture(CaptureError::Proof);
    if (expected.empty()) {
        if (!proof.isEmpty()) RejectCapture(CaptureError::Proof);
    } else if (!stump.verifyBlockProof(proof)) RejectCapture(CaptureError::Proof);
    if (parent.getHeight() != result.height_) throw OrchardCoinLookupError(Status::Internal);
    return result;
}
StatusOr<UTXOEntry> OrchardCandidateCoinView::getCoin(const OutPoint& point) const {
    if (const auto found = inputs_.find(point); found != inputs_.end()) return found->second;
    if (absent_.contains(point)) return Status::NotFound;
    return Status::Internal;
}
bool OrchardCandidateCoinView::hasCoin(const OutPoint& point) const {
    const auto result = getCoin(point);
    if (!result.ok() && result.status() != Status::NotFound) throw OrchardCoinLookupError(result.status());
    return result.ok();
}

OrchardCandidateCoinView CaptureOrchardCatalogCoins(
    const OrchardBlockCandidate& block, const OrchardBlockContext& context,
    const BlockHeader& header, const storage::catalog::State& catalog,
    const storage::catalog::Tree::Read& read) {
    namespace c=storage::catalog;
    catalog.Validate();
    if (!read || !context.height || catalog.height!=context.height-1 ||
        catalog.block!=context.parent_hash || catalog.block!=header.GetHash() ||
        catalog.parent!=header.prev_block_hash || catalog.activation!=context.activation_height ||
        catalog.network!=context.domain.network_code || catalog.branch!=context.domain.branch_id ||
        !std::equal(catalog.genesis.begin(),catalog.genesis.end(),context.domain.genesis_wire.begin()) ||
        catalog.leaf_activation!=GetUtreexoMaturityLeafActivationHeight()) RejectCapture(CaptureError::Context);
    if (!block.Utreexo()) RejectCapture(CaptureError::Proof);
    const auto stump=UtreexoStump::deserialize(catalog.stump);
    c::Tree transactions(c::Kind::Transactions,read),legacy(c::Kind::LegacyCoins,read);
    c::Tree nontransparent(c::Kind::NonTransparentCoins,read);
    const auto membership=[&](const TxId& id)->StatusOr<bool> {
        return transactions.Find(catalog.transactions,c::TransactionKey(id)).has_value();
    };
    struct Provisional final:ChainStateView {
        uint32_t height=0;
        std::map<OutPoint,std::optional<UTXOEntry>> rows;
        StatusOr<UTXOEntry> getCoin(const OutPoint& point)const override {
            const auto i=rows.find(point);
            if(i==rows.end())return Status::Internal;
            if(!i->second)return Status::NotFound;
            return *i->second;
        }
        bool hasCoin(const OutPoint& point)const override {
            const auto v=getCoin(point);
            if(!v.ok()&&v.status()!=Status::NotFound)throw OrchardCoinLookupError(v.status());
            return v.ok();
        }
        uint32_t getHeight()const override{return height;}
    } provisional;
    provisional.height=catalog.height;
    std::vector<OutPoint> inputs;
    std::set<TxId> ids;
    for(const auto& tx:block.Transactions()) {
        const auto id=tx.GetTxid();
        if(!ids.insert(id).second)RejectCapture(CaptureError::DuplicateTransaction);
        if(*membership(id))RejectCapture(CaptureError::HistoricalTransaction);
        const auto outputs=tx.IsOrchard()?tx.Orchard().Outputs().size():tx.Historical().vout.size();
        for(size_t i=0;i<outputs;++i)provisional.rows.emplace(OutPoint(id,uint32_t(i)),std::nullopt);
        if(tx.IsOrchard())for(const auto& input:tx.Orchard().Inputs())inputs.push_back(Point(input));
        else if(!tx.Historical().IsCoinbase())for(const auto& input:tx.Historical().vin)
            inputs.emplace_back(input.prevout.txid,input.prevout.vout);
    }
    const auto& metadata=block.Utreexo()->spent_outputs;
    if(metadata.size()!=inputs.size())RejectCapture(CaptureError::Metadata);
    for(size_t i=0;i<inputs.size();++i) {
        const auto& point=inputs[i];const auto& claimed=metadata[i];
        // Candidate outputs are resolved in order by Capture below, including
        // forward-reference refusal and exact same-block metadata comparison.
        if(ids.contains(point.txid))continue;
        const auto bytes=c::OutpointBytes(point);
        const auto special=nontransparent.Find(catalog.nontransparent,c::NonTransparentKey(bytes));
        if(special || claimed.is_confidential || !claimed.commitment.empty())
            RejectCapture(CaptureError::Metadata);
        // Query legacy ownership independently of the claimed height. Legacy
        // leaves do not bind either creation height or coinbase status.
        const auto old=legacy.Find(catalog.legacy,c::LegacyKey(bytes));
        uint32_t height=claimed.created_height;bool coinbase=claimed.is_coinbase;
        if(old) {
            if(old->size()!=41 || old->compare(0,36,bytes)!=0 || uint8_t((*old)[40])>1)
                throw OrchardCoinLookupError(Status::Corruption);
            height=uint32_t(c::Number(*old,36,4));coinbase=uint8_t((*old)[40])!=0;
            if(height>=catalog.leaf_activation || height>catalog.height)
                throw OrchardCoinLookupError(Status::Corruption);
            if(height!=claimed.created_height || coinbase!=claimed.is_coinbase)
                RejectCapture(CaptureError::Metadata);
        } else if(height<catalog.leaf_activation)RejectCapture(CaptureError::Metadata);
        if(height>catalog.height || claimed.value>orchard::kMaxMoneyUna)
            RejectCapture(CaptureError::Metadata);
        provisional.rows.emplace(point,UTXOEntry(AmountUna::Una(claimed.value),claimed.scriptPubKey,height,coinbase));
    }
    // Modern metadata is authoritative only after this exact proof succeeds.
    // Nothing in provisional is returned or published on a partial failure.
    return OrchardCandidateCoinView::Capture(block,context,header,stump,provisional,membership);
}
} // namespace dinero::consensus
