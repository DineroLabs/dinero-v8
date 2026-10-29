#pragma once
#include "daemon/relay_transaction_reader.h"
#include "consensus/utreexo_stump.h"
#include "consensus/chain_state_view.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include <limits>
#include <algorithm>
#include <set>

namespace dinero {
class VerifiedUtreexoTransaction;
class OrchardPoolCoinView;
// Whole-message parsing and inclusion proof validation do not grant admission.
class UtreexoTransactionPayload {
    struct Data {
        MempoolTransaction body;
        std::vector<std::pair<consensus::UtreexoProof,consensus::SpentOutputData>> proofs;
        consensus::UtreexoHash root;
        std::vector<uint8_t> wire;
    };
    std::shared_ptr<const Data> data_;
    explicit UtreexoTransactionPayload(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    friend class VerifiedUtreexoTransaction;
    friend class OrchardPoolCoinView; // Raw claims remain private until catalog/proof capture.
public:
    UtreexoTransactionPayload(const UtreexoTransactionPayload&)=default;
    UtreexoTransactionPayload& operator=(const UtreexoTransactionPayload&)=default;
    const MempoolTransaction& Body() const noexcept { return data_->body; }
    const consensus::UtreexoHash& Root() const noexcept { return data_->root; }
    const std::vector<uint8_t>& Wire() const noexcept { return data_->wire; }
    static UtreexoTransactionPayload Decode(std::span<const uint8_t> bytes,
                                           RelayTransactionReadMode mode) {
        constexpr size_t max_payload=2*1024*1024,max_tx=1024*1024;
        constexpr size_t max_proof=256*1024,max_script=10*1024,max_inputs=1000;
        if(bytes.size()<73 || bytes.size()>max_payload)
            throw std::invalid_argument("Invalid Utreexo transaction payload size");
        size_t pos=0;
        auto take=[&](size_t n) {
            if(n>bytes.size()-pos) throw std::invalid_argument("Incomplete Utreexo transaction payload");
            const auto part=bytes.subspan(pos,n);pos+=n;return part;
        };
        auto integer=[&](size_t n) {
            const auto part=take(n);uint64_t value=0;
            for(size_t i=0;i<n;++i)value|=uint64_t(part[i])<<(8*i);
            return value;
        };
        const auto version=integer(1);
        if(version!=1 && version!=2) throw std::invalid_argument("Unknown Utreexo transaction payload version");
        const auto id_bytes=take(32);uint256 claimed;
        std::copy(id_bytes.begin(),id_bytes.end(),claimed.begin());
        const auto tx_size=integer(4);
        if(tx_size==0 || tx_size>max_tx) throw std::invalid_argument("Invalid Utreexo transaction body size");
        const auto tx_bytes=take(tx_size);
        auto data=std::make_shared<Data>();data->body=DecodeRelayTransaction(tx_bytes,mode);
        if(data->body.GetTxid().AsUint256()!=claimed || data->body.Serialize()!=std::vector<uint8_t>(tx_bytes.begin(),tx_bytes.end()))
            throw std::invalid_argument("Utreexo transaction body identity mismatch");
        const auto count=integer(4);
        if(count>max_inputs || count!=data->body.Inputs().size())
            throw std::invalid_argument("Utreexo transaction input proof count mismatch");
        for(const auto& input:data->body.Inputs())
            if(input.txid.IsNull()) throw std::invalid_argument("Coinbase has no mempool Utreexo proof payload");
        data->proofs.reserve(count);
        for(size_t i=0;i<count;++i) {
            const auto size=integer(4);
            if(size<20 || size>max_proof) throw std::invalid_argument("Invalid Utreexo input proof size");
            const auto part=take(size);const std::vector<uint8_t> proof_bytes(part.begin(),part.end());
            auto proof=consensus::UtreexoProof::deserialize(proof_bytes);
            if(proof.serialize()!=proof_bytes) throw std::invalid_argument("Noncanonical Utreexo input proof");
            consensus::SpentOutputData spent;spent.value=integer(8);
            const auto script_size=integer(4);
            if(script_size>max_script) throw std::invalid_argument("Invalid Utreexo input script size");
            const auto script=take(script_size);spent.scriptPubKey.assign(script.begin(),script.end());
            if(version==2) {
                spent.created_height=static_cast<uint32_t>(integer(4));const auto flags=integer(1);
                if(flags>1) throw std::invalid_argument("Unknown Utreexo input flags");
                spent.is_coinbase=flags!=0;
            }
            data->proofs.emplace_back(std::move(proof),std::move(spent));
        }
        const auto root=take(32);data->root.assign(root.begin(),root.end());
        if(pos!=bytes.size()) throw std::invalid_argument("Trailing Utreexo transaction payload bytes");
        data->wire.assign(bytes.begin(),bytes.end());
        return UtreexoTransactionPayload(std::move(data));
    }
    // Caller captures this stump and parent height under selected-chain/forest
    // ownership, and retains selected-chain ownership through pool publication.
    std::optional<VerifiedUtreexoTransaction> VerifyInputs(
        const consensus::UtreexoStump& selected_stump,uint32_t parent_height) const;
    // Additional independently authenticated metadata is required for legacy
    // leaves, which do not commit to creation height or coinbase status.
    std::optional<VerifiedUtreexoTransaction> VerifyInputs(
        const consensus::UtreexoStump& selected_stump,uint32_t parent_height,
        const consensus::ChainStateView& authenticated_inputs) const;

};
class VerifiedUtreexoTransaction {
    UtreexoTransactionPayload payload_;
    uint32_t height_;
    VerifiedUtreexoTransaction(UtreexoTransactionPayload payload,uint32_t height)
        : payload_(std::move(payload)),height_(height) {}
    friend class UtreexoTransactionPayload;
public:
    const MempoolTransaction& Body() const noexcept { return payload_.Body(); }
    const consensus::UtreexoHash& Root() const noexcept { return payload_.Root(); }
    const std::vector<uint8_t>& Wire() const noexcept { return payload_.Wire(); }
    uint32_t Height() const noexcept { return height_; }
};
inline std::optional<VerifiedUtreexoTransaction> UtreexoTransactionPayload::VerifyInputs(
    const consensus::UtreexoStump& selected_stump,uint32_t parent_height) const {
    if(parent_height==std::numeric_limits<uint32_t>::max() ||
       Root()!=selected_stump.getCommitment()) return std::nullopt;
    const auto roots=selected_stump.getRoots();
    for(size_t i=0;i<data_->proofs.size();++i) {
        const auto& [proof,spent]=data_->proofs[i];const auto& input=Body().Inputs()[i];
        if(proof.numLeaves!=selected_stump.getNumLeaves() || spent.created_height>parent_height)
            return std::nullopt;
        const uint32_t height=parent_height+1;
        const auto maturity=consensus::EvaluateUtreexoStatelessMaturity(height,spent.created_height,spent.is_coinbase);
        if(maturity==consensus::UtreexoStatelessMaturityStatus::IMMATURE_COINBASE ||
           (maturity==consensus::UtreexoStatelessMaturityStatus::LEGACY_DEFERRED &&
            height>=consensus::GetUtreexoMaturityLeafActivationHeight())) return std::nullopt;
        const auto leaf=consensus::HashUTXOForCreationHeight(input.txid.AsUint256(),input.vout,
            spent.value,spent.scriptPubKey,spent.created_height,spent.is_coinbase);
        if(!proof.verify(leaf,roots)) return std::nullopt;
    }
    return VerifiedUtreexoTransaction(*this,parent_height);
}
inline std::optional<VerifiedUtreexoTransaction> UtreexoTransactionPayload::VerifyInputs(
    const consensus::UtreexoStump& stump,uint32_t parent_height,
    const consensus::ChainStateView& authenticated_inputs) const {
    if(parent_height==std::numeric_limits<uint32_t>::max() ||
       authenticated_inputs.getHeight()!=parent_height || Root()!=stump.getCommitment())return std::nullopt;
    const auto roots=stump.getRoots();std::set<OutPoint> seen;
    for(size_t i=0;i<data_->proofs.size();++i) {
        const auto& point=Body().Inputs()[i];const auto& [proof,spent]=data_->proofs[i];
        if(!seen.insert(point).second || proof.numLeaves!=stump.getNumLeaves())return std::nullopt;
        const auto coin=authenticated_inputs.getCoin(point);
        if(!coin.ok() || coin->is_confidential || !coin->commitment.empty() ||
           coin->height>parent_height || coin->height!=spent.created_height ||
           coin->isCoinbase!=spent.is_coinbase || coin->value.GetUna()!=spent.value ||
           coin->scriptPubKey!=spent.scriptPubKey)return std::nullopt;
        if(coin->isCoinbase && uint64_t(parent_height)+1-coin->height<
            consensus::UTREEXO_STATELESS_COINBASE_MATURITY)return std::nullopt;
        const auto leaf=consensus::HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,
            spent.value,spent.scriptPubKey,coin->height,coin->isCoinbase);
        if(!proof.verify(leaf,roots))return std::nullopt;
    }
    return VerifiedUtreexoTransaction(*this,parent_height);
}
} // namespace dinero
