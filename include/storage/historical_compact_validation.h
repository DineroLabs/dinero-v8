#pragma once
#include "storage/historical_catalog_state.h"
#include "storage/orchard_storage_mode.h"
#include "storage/chain_db.h"
#include "consensus/chainparams.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/utreexo_stump.h"
#include "consensus/shielded/shielded_root.h"
#include <set>
#include <algorithm>
#include <stdexcept>

namespace dinero::storage {
// Rebind a previously independently authenticated historical selection to its
// current durable planes. This check cannot authenticate a caller-supplied
// catalog, recover absent rows or enroll an owner; reconstruction does that.
inline void CheckHistoricalCompactSelection(const ChainDB& db,const catalog::HistoricalState& state) {
    const auto require=[](bool ok){if(!ok)throw std::runtime_error("Historical compact selected storage mismatch");};
    const auto read=[&]<class T>(StatusOr<T> value)->T {require(value.ok());return std::move(*value);};
    state.Validate();const auto& params=Params();
    const uint8_t network=params.name=="mainnet"?0:params.name=="testnet"?1:params.name=="regtest"?2:0xff;
    uint256 genesis;
    require(network!=0xff && uint256::FromHex(params.genesis_hash,genesis) && state.network==network &&
        state.genesis==genesis&&state.branch==params.orchard_branch_id&&state.activation==params.orchard_activation_height&&
        state.leaf_activation==consensus::GetUtreexoMaturityLeafActivationHeight());
    const auto mode=ReadOrchardCompactStorageBinding(db);
    require(mode&&*mode==EncodeOrchardCompactStorageBinding(state));
    require(read(db.getHistoricalCompactCatalogState(state.block))==state.Encode());
    const auto tip=read(db.getTip()),validated=read(db.getValidatedTip());
    require(tip.height==int(state.height)&&tip.hash==state.block&&tip.work==state.work&&
        validated.height==tip.height&&validated.hash==tip.hash&&
        read(db.getBlockHashByHeight(tip.height))==tip.hash&&read(db.getBlockWork(tip.hash))==tip.work);
    const auto header=read(db.getHeader(state.block));
    require(header.GetHash()==state.block&&header.prev_block_hash==state.parent&&
        read(db.getBlockHeight(state.block))==int(state.height));
    const auto forest=read(db.getForestTipMarker());
    const auto stump=consensus::UtreexoStump::deserialize(state.stump);
    require(forest.height==int(state.height)&&forest.block_hash==state.block&&forest.forest_root==header.utreexo_root&&
        stump.getCommitment()==std::vector<uint8_t>(header.utreexo_root.begin(),header.utreexo_root.end()));
    require(db.hasSeparatedShieldedState()&&db.getOrchardState().status()==Status::NotFound&&
        db.getLegacyRetirementState().status()==Status::NotFound);
    const auto marker=read(db.getShieldedTipMarker());
    require(marker.height==int(state.height)&&marker.block_hash==state.block&&marker.shielded_root==state.tree_root&&
        marker.tree_size==state.tree_size&&marker.nullifier_count==state.nullifier_count);
    const auto frontier=read(db.getShieldedState(ChainDB::ShieldedStateRecord::Frontier));
    const auto anchors=read(db.getShieldedState(ChainDB::ShieldedStateRecord::AnchorHistory));
    consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory history;
    require(tree.DeserializeFrontier(reinterpret_cast<const uint8_t*>(frontier.data()),frontier.size())&&
        history.DeserializePersistenceBytes({anchors.begin(),anchors.end()})==consensus::shielded::AnchorHistory::IoResult::Ok&&
        tree.Size()==state.tree_size);
    const auto root=tree.Root();require(std::equal(root.begin(),root.end(),state.tree_root.begin()));
    std::vector<consensus::shielded::NullifierEntry> entries;std::set<consensus::shielded::Hash> unique;
    bool valid=true;
    require(db.forEachShieldedNullifier([&](uint32_t height,const uint8_t* bytes) {
        consensus::shielded::NullifierEntry entry;entry.height=height;std::copy_n(bytes,32,entry.nullifier.begin());
        if(height<state.legacy_epoch||height>state.height||entries.size()>=state.nullifier_count||
            !unique.insert(entry.nullifier).second){valid=false;return false;}
        entries.push_back(entry);return true;
    })==Status::Ok&&valid&&entries.size()==state.nullifier_count);
    const auto composite=consensus::shielded::ComputeShieldedRootFromParts({root.begin(),root.end()},tree.Size(),
        consensus::shielded::ComputeNullifierAccumulator(std::move(entries)),history.SerializeBytes());
    require(composite&&*composite==state.legacy_state_root);
    bool present=false;
    require(db.forEachUTXO([&](const auto&,uint32_t,const auto&){present=true;return false;})==Status::Ok&&!present);
    require(db.forEachPreBaseCoin([&](const auto&,uint32_t,const auto&){present=true;return false;})==Status::Ok&&!present);
    require(db.getPreBaseCoinSetBase().status()==Status::NotFound);
}
}
