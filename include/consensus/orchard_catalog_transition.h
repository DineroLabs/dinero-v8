#pragma once

#include "consensus/orchard_block_coins.h"
#include "consensus/undo.h"
#include "storage/orchard_catalog_state.h"

namespace dinero::consensus {
// The same conventional undo bytes are bound by the per-block catalog and by
// canonical storage. The argument must come from complete coin validation.
inline UndoRecord BuildOrchardCoinUndo(const PreparedOrchardBlockCoins& coins) {
    UndoRecord undo;
    for(const auto& change:coins.Changes()) {
        if(change.before) {
            const auto& coin=*change.before;
            undo.spent.emplace_back(change.outpoint.txid.AsUint256(),change.outpoint.vout,
                coin.value.GetUna(),coin.scriptPubKey,coin.isCoinbase,coin.height,
                coin.is_confidential,coin.commitment);
        }
        if(change.after)undo.created.emplace_back(change.outpoint.txid.AsUint256(),change.outpoint.vout);
    }
    return undo;
}

// Pure persistent-tree update shared by canonical writers and snapshot
// verification. Does not establish parent authority, validate the block, or
// publish storage. All intermediate writes remain in the caller's private
// staging area until the complete transition and selected view are checked.
inline storage::catalog::State AdvanceOrchardCatalog(
    storage::catalog::State state,const PreparedOrchardBlockCoins& coins,
    const UtreexoStump& after,const arith_uint256& work,
    const std::vector<uint8_t>& undo,
    const storage::catalog::Tree::Read& read,const storage::catalog::Tree::Write& write) {
    namespace c=storage::catalog;
    state.Validate();c::Require(state.block==coins.ParentHash()&&uint64_t(state.height)+1==coins.Height()&&work>state.work);
    const auto parent_bytes=state.Encode();
    c::Tree transactions(c::Kind::Transactions,read,write),legacy(c::Kind::LegacyCoins,read,write);
    c::Tree nontransparent(c::Kind::NonTransparentCoins,read,write);
    for(const auto& tx:coins.Transactions()) {
        c::Require(state.transaction_count!=UINT64_MAX);
        state.transactions=transactions.Insert(state.transactions,c::TransactionKey(tx.txid),{});++state.transaction_count;
    }
    for(const auto& change:coins.Changes()) {
        const auto point=c::OutpointBytes(change.outpoint);const auto key=c::LegacyKey(point);
        const auto found=legacy.Find(state.legacy,key);
        const auto special_key=c::NonTransparentKey(point);
        const auto special=nontransparent.Find(state.nontransparent,special_key);
        const bool was_special=change.before&&(change.before->is_confidential||!change.before->commitment.empty());
        c::Require(special.has_value()==was_special);
        if(special) {
            c::Require(*special==point&&state.nontransparent_count>0);
            state.nontransparent=nontransparent.Erase(state.nontransparent,special_key);--state.nontransparent_count;
        }
        if(change.after&&(change.after->is_confidential||!change.after->commitment.empty())) {
            c::Require(change.after->height==coins.Height()&&state.nontransparent_count!=UINT64_MAX);
            state.nontransparent=nontransparent.Insert(state.nontransparent,special_key,point);++state.nontransparent_count;
        }
        const auto metadata=[&](const UTXOEntry& coin){auto bytes=point;c::Number(bytes,coin.height,4);bytes.push_back(coin.isCoinbase?1:0);return bytes;};
        if(change.before&&change.before->height<state.leaf_activation) {
            c::Require(found&&*found==metadata(*change.before)&&state.legacy_count>0);
            state.legacy=legacy.Erase(state.legacy,key);--state.legacy_count;
        } else c::Require(!found);
        if(change.after&&change.after->height<state.leaf_activation) {
            c::Require(change.after->height==coins.Height()&&state.legacy_count!=UINT64_MAX);
            state.legacy=legacy.Insert(state.legacy,key,metadata(*change.after));++state.legacy_count;
        }
    }
    state.previous_record=c::Hash(parent_bytes);state.undo=c::Hash(std::string(undo.begin(),undo.end()));
    state.height=coins.Height();state.block=coins.BlockHash();state.parent=coins.ParentHash();state.work=work;
    state.stump=after.serialize();state.Validate();return state;
}
} // namespace dinero::consensus
