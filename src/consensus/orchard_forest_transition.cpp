#include "consensus/orchard_forest_transition.h"
#include <set>

namespace dinero::consensus {
namespace {
using Error=OrchardForestErrorCode;
[[noreturn]] void Reject(Error error){throw OrchardForestError(error);}
uint256 Root(const UtreexoForest& forest) {
    const auto bytes=forest.getCommitment();
    if(bytes.size()!=32)Reject(Error::ParentCommitment);
    uint256 result;std::copy(bytes.begin(),bytes.end(),result.begin());return result;
}
UtreexoHash Leaf(const OutPoint& point,const UTXOEntry& coin) {
    return HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,coin.value.GetUna(),
                                    coin.scriptPubKey,coin.height,coin.isCoinbase);
}
}
PreparedOrchardForest PrepareOrchardForestTransition(const PreparedOrchardBlockCoins& coins,
    const BlockHeader& parent,const UtreexoForest& forest) {
    if(parent.GetHash()!=coins.ParentHash() || !parent.IsReservedValid())Reject(Error::Context);
    if(Root(forest)!=parent.utreexo_root)Reject(Error::ParentCommitment);
    auto after=std::make_unique<UtreexoForest>(forest.cloneForHeight(coins.Height()));
    UtreexoDelta delta;delta.numLeavesBefore=forest.getNumLeaves();
    std::set<OutPoint> created,spent;
    for(const auto& tx:coins.Transactions()) {
        for(const auto& [point,coin]:tx.created)created.insert(point);
        for(const auto& [point,coin]:tx.spent)spent.insert(point);
    }
    std::vector<std::pair<uint64_t,UtreexoHash>> removals;
    // Same two-pass forest ordering as the canonical full-node implementation:
    // remove external inputs, then add surviving outputs in transaction order.
    for(const auto& tx:coins.Transactions())for(const auto& [point,coin]:tx.spent) {
        if(created.contains(point))continue;
        auto hash=Leaf(point,coin);const auto position=after->findLeafPosition(hash);
        if(!position)Reject(Error::MissingLeaf);
        delta.recordDelete(*position,hash);removals.emplace_back(*position,std::move(hash));
    }
    if(!after->removeAtKnownPositions(removals))Reject(Error::Delete);
    for(const auto& tx:coins.Transactions())for(const auto& [point,coin]:tx.created) {
        if(spent.contains(point))continue;
        auto hash=Leaf(point,coin);const auto position=after->add(hash);
        if(position==UINT64_MAX)Reject(Error::Add);
        delta.recordAdd(hash,position);
    }
    const auto root=Root(*after);
    return PreparedOrchardForest(coins.BlockHash(),root,parent.utreexo_root,forest.isCanonicalEmptyRoots(),
                                 std::move(delta),std::move(after));
}
UtreexoForest UndoOrchardForestTransition(const UtreexoForest& current,const PreparedOrchardForest& transition) {
    if(Root(current)!=transition.root_ || current.getNumLeaves()!=transition.after_->getNumLeaves() ||
        current.isCanonicalEmptyRoots()!=transition.after_->isCanonicalEmptyRoots())Reject(Error::Undo);
    auto restored=current.clone();
    if(!restored.removeLastNLeaves(transition.delta_.addedLeaves.size()))Reject(Error::Undo);
    for(auto it=transition.delta_.deletedLeaves.rbegin();it!=transition.delta_.deletedLeaves.rend();++it)
        if(!restored.restoreDeletedLeaf(it->position,it->leafHash))Reject(Error::Undo);
    if(restored.isCanonicalEmptyRoots()!=transition.parent_canonical_) {
        restored.setCanonicalEmptyRoots(transition.parent_canonical_);restored.rebuildRoots();
    }
    if(restored.getNumLeaves()!=transition.delta_.numLeavesBefore || Root(restored)!=transition.parent_root_)
        Reject(Error::Undo);
    return restored;
}
} // namespace dinero::consensus
