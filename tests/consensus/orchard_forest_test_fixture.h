#pragma once
#include "orchard_block_coin_test_fixture.h"
#include "consensus/orchard_forest_transition.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include <set>

static BlockUtreexoData MixedProof(const PreparedOrchardBlockCoins& coins,const UtreexoForest& forest) {
    BlockUtreexoData data;data.accumulator_root_before=forest.getCommitment();
    data.spend_proof.numLeaves=forest.getNumLeaves();
    data.spend_proof.format_version=GetUtreexoProofFormatVersion(coins.Height());
    std::set<OutPoint> created;
    for(const auto& tx:coins.Transactions())for(const auto& item:tx.created)created.insert(item.first);
    for(const auto& tx:coins.Transactions())for(const auto& [point,coin]:tx.spent) {
        data.spent_outputs.emplace_back(coin.value.GetUna(),coin.scriptPubKey,coin.height,
            coin.isCoinbase,coin.is_confidential,coin.commitment);
        if(created.contains(point))continue;
        const auto leaf=HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,coin.value.GetUna(),
            coin.scriptPubKey,coin.height,coin.isCoinbase);
        const auto position=forest.findLeafPosition(leaf);Require(position.has_value());
        data.spend_proof.targets.push_back(leaf);data.spend_proof.positions.push_back(*position);
    }
    data.spend_proof=forest.generateBlockProof(data.spend_proof.targets,GetUtreexoProofFormatVersion(coins.Height()));
    return data;
}
// Only fixture candidates without a suffix are accepted by this helper.
static OrchardBlockCandidate WithProof(const OrchardBlockCandidate& block,const BlockUtreexoData& proof) {
    Require(!block.Utreexo());auto wire=block.WireBytes();Require(wire.back()==0);wire.back()=1;
    const auto bytes=proof.serialize();wire.insert(wire.end(),bytes.begin(),bytes.end());
    return OrchardBlockCandidate::DecodeExact(wire);
}
