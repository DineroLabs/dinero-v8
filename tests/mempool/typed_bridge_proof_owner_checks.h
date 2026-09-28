// Healthy isolated-forest proof serving; structural Orchard bodies do not imply admission.
#include "daemon/utreexo_tx_payload.h"
namespace {
class TypedBridgeProofOwner : public OrchardPoolConflicts {
protected:
    std::unique_ptr<network::BridgeNode> bridge;
    std::vector<consensus::UtreexoHash> leaves;
    void install(const MempoolTransaction& body, bool confidential=false) {
        for(size_t i=0;i<body.Inputs().size();++i) {
            const auto& out=body.Inputs()[i];const uint64_t amount=2000+i;
            consensus::UTXOEntry coin{AmountUna::Una(amount),{0x51},uint32_t(1+i),false};
            coin.is_confidential=confidential;
            if(!coins.AddCoin(out,coin)) throw std::runtime_error("fixture proof funding");
            leaves.push_back(consensus::HashUTXOForCreationHeight(out.txid.AsUint256(),out.vout,amount,{0x51},uint32_t(1+i),false));
        }
        coins.MutateForestGuarded([&](consensus::UtreexoForest& forest) {for(const auto& leaf:leaves)forest.add(leaf);});
        auto provider=std::shared_ptr<consensus::IUTXOProvider>(std::shared_ptr<void>{},&coins);
        bridge=std::make_unique<network::BridgeNode>(provider,&coins.GetForest(),nullptr,nullptr,nullptr,&coins);
    }
    static OutPoint input(uint8_t byte) {uint256 id;id.data[0]=byte;return {TxId(id),0};}
    void checkPayload(const MempoolTransaction& body,const std::vector<uint8_t>& payload) {
        size_t offset=0;
        auto integer=[&](size_t size) {
            if(size>payload.size()-offset)throw std::runtime_error("truncated fixture payload");
            uint64_t value=0;for(size_t i=0;i<size;++i)value|=uint64_t(payload[offset++])<<(8*i);return value;
        };
        auto bytes=[&](size_t size) {
            if(size>payload.size()-offset)throw std::runtime_error("truncated fixture bytes");
            std::vector<uint8_t> value(payload.begin()+offset,payload.begin()+offset+size);offset+=size;return value;
        };
        EXPECT_EQ(integer(1),2U);const auto id=bytes(32);const auto expected=body.GetTxid().AsUint256();
        EXPECT_TRUE(std::equal(id.begin(),id.end(),expected.begin()));
        EXPECT_EQ(bytes(integer(4)),body.Serialize());ASSERT_EQ(integer(4),body.Inputs().size());
        auto owner=coins.LockForestShared();const auto roots=coins.GetForest().getRoots();
        for(size_t i=0;i<body.Inputs().size();++i) {
            const auto proof=consensus::UtreexoProof::deserialize(bytes(integer(4)));
            const auto value=integer(8);const auto script=bytes(integer(4));const auto height=integer(4);const auto flag=integer(1);
            EXPECT_EQ(value,2000+i);EXPECT_EQ(script,std::vector<uint8_t>{0x51});EXPECT_EQ(height,1+i);EXPECT_EQ(flag,0U);
            EXPECT_TRUE(proof.verify(leaves.at(i),roots));
        }
        EXPECT_EQ(bytes(32),coins.GetForest().getCommitment());EXPECT_EQ(offset,payload.size());
    }
};
TEST_F(TypedBridgeProofOwner, HistoricalProofsOwnOneRootAndPayload) {
    const MempoolTransaction body(ordinary({input(211),input(212)},61));install(body);
    const auto capture=bridge->CaptureInputProofs(body.GetTxid().AsUint256(),body.Inputs());ASSERT_TRUE(capture);ASSERT_EQ(capture->proofs.size(),2U);
    const auto payload=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(payload);checkPayload(body,*payload);
    const auto legacy=bridge->GenerateProofsForTransaction(body.Historical());ASSERT_TRUE(legacy);ASSERT_EQ(legacy->size(),2U);
    for(size_t i=0;i<2;++i)EXPECT_EQ(legacy->at(i).first.serialize(),capture->proofs[i].first.serialize());
    const auto old_root=capture->root;std::vector<consensus::UtreexoHash> roots;
    {auto owner=coins.LockForestShared();roots=coins.GetForest().getRoots();}
    uint256 more;more.data[0]=215;
    coins.MutateForestGuarded([&](consensus::UtreexoForest& forest){forest.add(consensus::HashUTXOForCreationHeight(more,0,3000,{0x51},2,false));});
    EXPECT_EQ(capture->root,old_root);EXPECT_NE(capture->root,bridge->GetCurrentForestCommitment());
    for(size_t i=0;i<2;++i)EXPECT_TRUE(capture->proofs[i].first.verify(leaves[i],roots));
    const auto newer=bridge->CaptureInputProofs(body.GetTxid().AsUint256(),body.Inputs());ASSERT_TRUE(newer);EXPECT_NE(newer->root,old_root);
}
TEST_F(TypedBridgeProofOwner, MissingInputAndUnrepresentableMetadataRefuseWholePayload) {
    const MempoolTransaction body(ordinary({input(213)},62));install(body);
    ASSERT_TRUE(CaptureUtreexoTransactionPayload(body,*bridge));
    const auto id=body.GetTxid().AsUint256();auto incomplete=body.Inputs();incomplete.push_back(input(214));
    EXPECT_FALSE(bridge->CaptureInputProofs(id,incomplete));EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    EXPECT_FALSE(CaptureUtreexoTransactionPayload(MempoolTransaction{},*bridge));
    ASSERT_TRUE(bridge->CaptureInputProofs(id,body.Inputs()));
    bridge->InvalidateTxProofCache();ASSERT_TRUE(coins.DeleteCoin(body.Inputs()[0]));
    EXPECT_FALSE(CaptureUtreexoTransactionPayload(body,*bridge));
    consensus::UTXOEntry coin{AmountUna::Una(2000),{0x51},1,false};coin.is_confidential=true;
    ASSERT_TRUE(coins.AddCoin(body.Inputs()[0],coin));EXPECT_FALSE(CaptureUtreexoTransactionPayload(body,*bridge));
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(TypedBridgeProofOwner, OrchardCanonicalBytesAndInputProofsNeedNoHistoricalConversion) {
    auto body=MempoolTransaction::FromOrchard(envelope());ASSERT_GT(body.Inputs().size(),0U);install(body);
    const auto owned=body;const auto payload=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(payload);
    body=MempoolTransaction{};checkPayload(owned,*payload);EXPECT_THROW(owned.Historical(),std::logic_error);
    const auto repeated=CaptureUtreexoTransactionPayload(owned,*bridge);ASSERT_TRUE(repeated);EXPECT_EQ(*repeated,*payload);
}
#endif
}
