#include "consensus/orchard_block_filter.h"
#include "consensus/filter_commitment.h"
namespace dinero::consensus {
GCSFilter BuildOrchardBlockFilter(const PreparedOrchardBlockCoins& coins) {
    std::vector<std::vector<uint8_t>> scripts;
    for (const auto& tx : coins.Transactions()) {
        for (const auto& [point, coin] : tx.created)
            if (!coin.scriptPubKey.empty() && coin.scriptPubKey.front() != 0x6a)
                scripts.push_back(coin.scriptPubKey);
        for (const auto& [point, coin] : tx.spent)
            if (!coin.scriptPubKey.empty()) scripts.push_back(coin.scriptPubKey);
    }
    return GCSFilter::Build(scripts, coins.ParentHash());
}
GCSFilter CheckOrchardBlockFilter(const OrchardBlockCandidate& block,
    const PreparedOrchardBlockCoins& coins) {
    if (coins.BlockHash() != block.Header().GetHash() ||
        coins.ParentHash() != block.Header().prev_block_hash ||
        coins.Transactions().size() != block.Transactions().size())
        throw OrchardBlockCoinError(OrchardBlockCoinErrorCode::Context);
    for (size_t i = 0; i < coins.Transactions().size(); ++i)
        if (coins.Transactions()[i].txid != block.Transactions()[i].GetTxid())
            throw OrchardBlockCoinError(OrchardBlockCoinErrorCode::Context);
    if (block.Transactions().empty() || block.Transactions()[0].IsOrchard())
        throw OrchardBlockCoinError(OrchardBlockCoinErrorCode::Body);
    auto filter = BuildOrchardBlockFilter(coins);
    std::string error;
    if (!ValidateFilterCommitment(block.Transactions()[0].Historical(),
            filter.GetHash(), coins.Height(), error))
        throw OrchardBlockCoinError(OrchardBlockCoinErrorCode::Filter);
    return filter;
}
} // namespace dinero::consensus
