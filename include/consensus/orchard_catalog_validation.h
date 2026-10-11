#pragma once
#include "consensus/orchard_state_transition.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "storage/chain_db.h"
#include "storage/orchard_catalog_state.h"

namespace dinero::consensus {
inline void RequireOrchardCatalog(bool ok) {
    if(!ok)throw OrchardStateLookupError(Status::Corruption,"catalog/binding");
}
inline std::optional<std::string> ReadOrchardCatalogRecord(const ChainDB& db,const uint256& hash) {
    const auto value=db.getOrchardCatalogState(hash);
    if(value.ok())return *value;
    if(value.status()==Status::NotFound)return {};
    throw OrchardStateLookupError(value.status(),"catalog/read");
}
// Local checked-state comparison under the caller's selected writer lock.
// It neither produces a historical catalog nor certifies missing history.
inline void CheckOrchardCatalogStump(const ChainDB& db,const storage::catalog::State& state,
    const OrchardBlockContext& context,const BlockHeader& header,uint32_t height,
    const UtreexoStump& stump) {
    state.Validate();const auto work=db.getBlockWork(state.block);
    if(!work.ok())throw OrchardStateLookupError(work.status(),"catalog/work");
    const auto root=stump.getCommitment();
    RequireOrchardCatalog(state.network==context.domain.network_code&&state.branch==context.domain.branch_id&&
        std::equal(state.genesis.begin(),state.genesis.end(),context.domain.genesis_wire.begin())&&
        state.activation==context.activation_height&&state.leaf_activation==GetUtreexoMaturityLeafActivationHeight()&&
        state.height==height&&state.block==header.GetHash()&&state.parent==header.prev_block_hash&&state.work==*work&&
        state.stump==stump.serialize()&&root.size()==32&&
        std::equal(root.begin(),root.end(),header.utreexo_root.begin()));
}
inline void CheckOrchardCatalogState(const ChainDB& db,const storage::catalog::State& state,
    const OrchardBlockContext& context,const BlockHeader& header,uint32_t height,
    const UtreexoForest& forest) {
    CheckOrchardCatalogStump(db,state,context,header,height,UtreexoStump::fromForest(forest));
}
inline void CheckOrchardCatalogUndo(const ChainDB& db,const OrchardBlockContext& context,
    const BlockHeader& block,const BlockHeader& parent,const UtreexoForest& before,const UtreexoForest& after,
    const std::vector<uint8_t>& undo,const std::optional<std::string>& current,
    const std::optional<std::string>& previous,bool required) {
    if(!current){RequireOrchardCatalog(!required&&!previous);return;}
    RequireOrchardCatalog(bool(previous));
    const auto state=storage::catalog::State::Decode(*current),prior=storage::catalog::State::Decode(*previous);
    CheckOrchardCatalogState(db,state,context,block,context.height,before);
    CheckOrchardCatalogState(db,prior,context,parent,context.height-1,after);
    RequireOrchardCatalog(state.previous_record==storage::catalog::Hash(*previous)&&
        state.undo==storage::catalog::Hash(std::string(undo.begin(),undo.end())));
}
} // namespace dinero::consensus
