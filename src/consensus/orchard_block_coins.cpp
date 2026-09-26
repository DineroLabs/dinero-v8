#include "consensus/orchard_block_coins.h"
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
} // namespace

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
    OrchardResourceUsage resources;
    for (const auto& parsed : block.Transactions())
        AccumulateOrchardTransactionResources(parsed, resources);
    OrderedView view(parent);
    std::set<TxId> ids;
    std::vector<OrchardTransactionCoins> records;
    std::vector<VerifiedOrchardAuthorizations> authorizations;
    uint64_t total_fees = 0;
    for (size_t index = 0; index < block.Transactions().size(); ++index) {
        const auto& parsed = block.Transactions()[index];
        OrchardTransactionCoins record; record.txid = parsed.GetTxid(); record.coinbase = index == 0;
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
    if (!reward_detail::CheckCoinbaseReward(block.Transactions().front().Historical(), context.height, total_fees, error))
        Reject(Error::Reward);
    if (parent.getHeight() != context.height - 1) throw OrchardCoinLookupError(Status::Internal);
    return PreparedOrchardBlockCoins(context.block_hash, context.parent_hash, context.height, std::move(records), view.Changes(),
                                    std::move(authorizations), total_fees, resources);
}
} // namespace dinero::consensus
