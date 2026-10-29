#include "consensus/orchard_coin_snapshot.h"
#include <algorithm>

namespace dinero::consensus {
OrchardCoinSnapshot::OrchardCoinSnapshot(orchard::TransactionEnvelope transaction,
    std::vector<UTXOEntry> coins, uint32_t view_height)
    : transaction_(std::move(transaction)), coins_(std::move(coins)), view_height_(view_height) {}

OrchardCoinSnapshot OrchardCoinSnapshot::ResolveUnderChainstateLock(
    const orchard::TransactionEnvelope& transaction, const ChainStateView& view) {
    const auto height = view.getHeight();
    std::vector<UTXOEntry> coins;
    coins.reserve(transaction.Inputs().size());
    uint64_t total = 0;
    for (const auto& input : transaction.Inputs()) {
        uint256 txid;
        std::copy(input.txid_wire.begin(), input.txid_wire.end(), txid.begin());
        // One authoritative read: a separate hasCoin probe could disagree.
        const auto result = view.getCoin(OutPoint(TxId(txid), input.output_index));
        if (!result.ok()) throw OrchardCoinLookupError(result.status());
        const auto& coin = result.value();
        // The draft Orchard context supports explicit transparent amounts only.
        // Never reinterpret a confidential placeholder as an authenticated value.
        if (coin.is_confidential || !coin.commitment.empty())
            throw std::invalid_argument("Orchard input has confidential value metadata");
        const auto amount = coin.value.GetUna();
        if (amount > orchard::kMaxMoneyUna || total > orchard::kMaxMoneyUna - amount)
            throw std::invalid_argument("Orchard input amount exceeds money bound");
        total += amount;
        coins.push_back(coin); // Includes height/coinbase metadata for later spend checks.
    }
    // Detect an obvious broken caller contract. Same-height reorgs still require
    // the external lock; this is deliberately not advertised as snapshot locking.
    if (view.getHeight() != height) throw OrchardCoinLookupError(Status::Internal);
    return OrchardCoinSnapshot(transaction, std::move(coins), height);
}

std::vector<orchard::PreviousOutput> OrchardCoinSnapshot::PreviousOutputs() const {
    std::vector<orchard::PreviousOutput> previous;
    previous.reserve(coins_.size());
    for (size_t i = 0; i < coins_.size(); ++i) {
        const auto& input = transaction_.Inputs()[i];
        const auto& coin = coins_[i];
        previous.push_back({input.txid_wire, input.output_index,
                            coin.value.GetUna(), coin.scriptPubKey});
    }
    return previous;
}
orchard::Hash OrchardCoinSnapshot::SigningDigest(orchard::SigningDomain domain) const {
    return transaction_.SigningDigest(domain, PreviousOutputs());
}
orchard::VerifiedEnvelopeAuthorization OrchardCoinSnapshot::VerifyOrchardAuthorization(
    orchard::SigningDomain domain) const {
    return transaction_.VerifyAuthorization(domain, PreviousOutputs());
}
} // namespace dinero::consensus
