#include "consensus/orchard_forest_transition.h"
#include <set>
#include "consensus/utreexo_canonical_roots_activation.h"

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
UtreexoForest UndoOrchardForestDelta(const UtreexoForest& current,const UtreexoDelta& delta,
    const BlockHeader& parent,const BlockHeader& header,uint32_t height) {
    if(height==0 || header.prev_block_hash!=parent.GetHash() || !parent.IsReservedValid() ||
        !header.IsReservedValid() || Root(current)!=header.utreexo_root ||
        current.isCanonicalEmptyRoots()!=IsUtreexoCanonicalRootsActive(height) ||
        delta.addedLeaves.size()>UINT64_MAX-delta.numLeavesBefore ||
        current.getNumLeaves()!=delta.numLeavesBefore+delta.addedLeaves.size())Reject(Error::Undo);
    std::set<uint64_t> positions;
    for(const auto& leaf:delta.deletedLeaves)
        if(leaf.leafHash.size()!=32 || leaf.position>=delta.numLeavesBefore ||
            !positions.insert(leaf.position).second)Reject(Error::Undo);
    for(size_t i=0;i<delta.addedLeaves.size();++i) {
        const auto& leaf=delta.addedLeaves[i];
        if(leaf.hash.size()!=32 || leaf.position!=delta.numLeavesBefore+i ||
            current.findLeafPosition(leaf.hash)!=std::optional<uint64_t>(leaf.position))Reject(Error::Undo);
    }
    auto restored=current.clone();
    if(!restored.removeLastNLeaves(delta.addedLeaves.size()))Reject(Error::Undo);
    for(auto it=delta.deletedLeaves.rbegin();it!=delta.deletedLeaves.rend();++it)
        if(!restored.restoreDeletedLeaf(it->position,it->leafHash))Reject(Error::Undo);
    const bool canonical=IsUtreexoCanonicalRootsActive(height-1);
    if(restored.isCanonicalEmptyRoots()!=canonical) {
        restored.setCanonicalEmptyRoots(canonical);restored.rebuildRoots();
    }
    if(restored.getNumLeaves()!=delta.numLeavesBefore || Root(restored)!=parent.utreexo_root)
        Reject(Error::Undo);
    return restored;
}
} // namespace dinero::consensus
