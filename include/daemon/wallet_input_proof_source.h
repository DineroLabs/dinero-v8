#pragma once
#include "p2p/wallet_input_proof.h"
#include "daemon/services/chainstate_service.h"
#include "util/hex.h"
namespace dinero {
// One exact independently backed full-source observation; no bridge cache,
// assumed coin promotion or compact/full-forest fallback. Returned bytes are
// still DATA and require the recipient's current catalog/stump check.
inline std::optional<p2p::WalletInputProof::Reply> ServeWalletInputProof(
        ChainstateService& service,const p2p::WalletInputProof::Request& request){
    request.CheckProfile();const auto captured=service.CaptureUtreexoProofInputs({&request.point,1});
    if(!captured.ok()||captured->snapshot.compact||captured->snapshot.block_hash!=request.tip||
       captured->snapshot.height!=request.height||captured->snapshot.commitment.size()!=32||
       !std::equal(captured->snapshot.commitment.begin(),captured->snapshot.commitment.end(),request.root.begin())||
       captured->inputs.size()!=1||!captured->inputs[0].canonical||!captured->inputs[0].coin)return {};
    const auto& coin=*captured->inputs[0].coin;p2p::WalletInputProof::Reply reply;reply.request=request;
    if(coin.height<0||coin.height>int(request.height)||coin.is_confidential||!coin.commitment.empty())return {};
    reply.value=coin.amount;reply.created=uint32_t(coin.height);reply.coinbase=coin.coinbase;
    if(!util::unhex(coin.script_pubkey,reply.script))return {};
    const auto leaf=consensus::HashUTXOForCreationHeight(request.point.txid.AsUint256(),request.point.vout,reply.value,reply.script,reply.created,reply.coinbase);
    const auto position=captured->forest.findLeafPosition(leaf);const auto path=position?captured->forest.prove(*position):std::nullopt;
    if(!path||captured->forest.isDeleted(*position))return {};reply.path=*path;
    if(!reply.VerifyPath(consensus::UtreexoStump::fromForest(captured->forest)))return {};
    (void)reply.Encode();return reply;
}
}
