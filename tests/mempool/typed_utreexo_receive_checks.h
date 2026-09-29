#include "daemon/utreexo_tx_reader.h"
namespace {
class TypedUtreexoReceive : public TypedBridgeProofOwner {
protected:
    consensus::UtreexoStump stump() {
        auto guard=coins.LockForestShared();return consensus::UtreexoStump::fromForest(coins.GetForest());
    }
    static void little(std::vector<uint8_t>& bytes,uint64_t value,size_t width) {
        for(size_t i=0;i<width;++i)bytes.push_back(static_cast<uint8_t>(value>>(8*i)));
    }
    std::vector<uint8_t> legacyPayload(const MempoolTransaction& body) {
        const auto capture=bridge->CaptureInputProofs(body.GetTxid().AsUint256(),body.Inputs());
        if(!capture)throw std::runtime_error("fixture legacy proof capture");
        std::vector<uint8_t> wire{1};const auto id=body.GetTxid().AsUint256();wire.insert(wire.end(),id.begin(),id.end());
        const auto tx=body.Serialize();little(wire,tx.size(),4);wire.insert(wire.end(),tx.begin(),tx.end());little(wire,capture->proofs.size(),4);
        for(const auto& [proof,spent]:capture->proofs) {
            const auto bytes=proof.serialize();little(wire,bytes.size(),4);wire.insert(wire.end(),bytes.begin(),bytes.end());
            little(wire,spent.value,8);little(wire,spent.scriptPubKey.size(),4);wire.insert(wire.end(),spent.scriptPubKey.begin(),spent.scriptPubKey.end());
        }
        wire.insert(wire.end(),capture->root.begin(),capture->root.end());return wire;
    }
};
TEST_F(TypedUtreexoReceive, ExactHistoricalAndLegacyWirePublishesWholeCache) {
    const MempoolTransaction body(ordinary({input(221),input(222)},71));install(body);
    const auto bytes=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(bytes);
    const auto decoded=UtreexoTransactionPayload::Decode(*bytes,RelayTransactionReadMode::AvailableFamilies);
    const auto verified=decoded.VerifyInputs(stump(),110);ASSERT_TRUE(verified);
    EXPECT_EQ(verified->Body().Serialize(),body.Serialize());EXPECT_EQ(verified->Wire(),*bytes);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});
    EXPECT_FALSE(pool.publishProofPayload(*verified)); // no acknowledged pool tip yet
    pool.onBlockConnected(ConnectedBlockEffects{},110,verified->Root());
    ASSERT_TRUE(pool.publishProofPayload(*verified));
    const auto entry=pool.getMempoolEntry(body.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_EQ(entry->validated_at_root,verified->Root());EXPECT_EQ(entry->validated_at_height,110U);
    EXPECT_FALSE(entry->is_proof_stale);EXPECT_EQ(entry->cached_utxotx_payload,*bytes);
    EXPECT_EQ(pool.getCachedUtxoTxPayload(body.GetTxid().AsUint256()),bytes);
    const auto v1=legacyPayload(body);const auto old=UtreexoTransactionPayload::Decode(v1,RelayTransactionReadMode::HistoricalOnly);
    const auto old_verified=old.VerifyInputs(stump(),110);ASSERT_TRUE(old_verified);ASSERT_TRUE(pool.publishProofPayload(*old_verified));
    EXPECT_EQ(pool.getCachedUtxoTxPayload(body.GetTxid().AsUint256()),std::optional<std::vector<uint8_t>>(v1));
}
TEST_F(TypedUtreexoReceive, StaleSnapshotAndDifferentWitnessKeepPriorState) {
    auto tx=ordinary({input(223)},72);tx.vin[0].witness={{1,2,3}};
    const MempoolTransaction body(tx);install(body);const auto bytes=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(bytes);
    const auto decoded=UtreexoTransactionPayload::Decode(*bytes,RelayTransactionReadMode::AvailableFamilies);
    const auto verified=decoded.VerifyInputs(stump(),110);ASSERT_TRUE(verified);
    auto replacement=tx;replacement.vin[0].witness={{4,5,6}};ASSERT_EQ(replacement.GetTxid(),tx.GetTxid());
    Mempool mismatch(&db,&coins);MempoolOrchardConflictTestPeer::Install(mismatch,{MempoolTransaction(replacement)});
    mismatch.onBlockConnected(ConnectedBlockEffects{},110,verified->Root());
    EXPECT_FALSE(mismatch.publishProofPayload(*verified));
    const auto wrong=mismatch.getMempoolEntry(tx.GetTxid().AsUint256());ASSERT_TRUE(wrong);
    EXPECT_TRUE(wrong->cached_utxotx_payload.empty());EXPECT_TRUE(wrong->validated_at_root.empty());
    EXPECT_EQ(wrong->tx.Serialize(),replacement.Serialize());
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});pool.onBlockConnected(ConnectedBlockEffects{},110,verified->Root());ASSERT_TRUE(pool.publishProofPayload(*verified));
    uint256 extra;extra.data[0]=224;coins.MutateForestGuarded([&](consensus::UtreexoForest& f){f.add(consensus::HashUTXOForCreationHeight(extra,0,3000,{0x51},111,false));});
    const auto next=stump();pool.onBlockConnected(ConnectedBlockEffects{},111,next.getCommitment());
    EXPECT_FALSE(decoded.VerifyInputs(next,111));EXPECT_FALSE(pool.publishProofPayload(*verified));
    const auto stale=pool.getMempoolEntry(tx.GetTxid().AsUint256());ASSERT_TRUE(stale);EXPECT_TRUE(stale->is_proof_stale);EXPECT_TRUE(stale->cached_utxotx_payload.empty());
    const auto fresh=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(fresh);
    const auto fresh_verified=UtreexoTransactionPayload::Decode(*fresh,RelayTransactionReadMode::AvailableFamilies).VerifyInputs(next,111);ASSERT_TRUE(fresh_verified);
    ASSERT_TRUE(pool.publishProofPayload(*fresh_verified));EXPECT_EQ(pool.getCachedUtxoTxPayload(tx.GetTxid().AsUint256()),fresh);
}
TEST_F(TypedUtreexoReceive, WholeMessageAndProofRefusalLeavePoolUntouched) {
    const MempoolTransaction body(ordinary({input(225)},73));install(body);const auto wire=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(wire);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});pool.onBlockConnected(ConnectedBlockEffects{},110,stump().getCommitment());
    for(size_t length:std::vector<size_t>{0,32,wire->size()-1}) {
        const std::vector<uint8_t> shorter(wire->begin(),wire->begin()+length);
        EXPECT_THROW(UtreexoTransactionPayload::Decode(shorter,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
    }
    auto extra=*wire;extra.push_back(0);EXPECT_THROW(UtreexoTransactionPayload::Decode(extra,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
    auto wrong=*wire;wrong[1]^=1;EXPECT_THROW(UtreexoTransactionPayload::Decode(wrong,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
    auto version=*wire;version[0]=3;EXPECT_THROW(UtreexoTransactionPayload::Decode(version,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
    auto root=*wire;root.back()^=1;const auto bad=UtreexoTransactionPayload::Decode(root,RelayTransactionReadMode::AvailableFamilies);EXPECT_FALSE(bad.VerifyInputs(stump(),110));
    const auto decoded=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies);
    EXPECT_FALSE(decoded.VerifyInputs(stump(),UINT32_MAX));EXPECT_FALSE(decoded.VerifyInputs(stump(),0));
    const auto entry=pool.getMempoolEntry(body.GetTxid().AsUint256());ASSERT_TRUE(entry);EXPECT_TRUE(entry->validated_at_root.empty());EXPECT_TRUE(entry->cached_utxotx_payload.empty());EXPECT_EQ(entry->tx.Serialize(),body.Serialize());
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(TypedUtreexoReceive, OrchardInputProofReceiptPreservesFamilyWithoutAdmission) {
    const auto body=MempoolTransaction::FromOrchard(envelope());install(body);const auto wire=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(wire);
    const auto decoded=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies);ASSERT_TRUE(decoded.Body().IsOrchard());
    const auto verified=decoded.VerifyInputs(stump(),110);ASSERT_TRUE(verified);EXPECT_EQ(verified->Body().Serialize(),body.Serialize());
    EXPECT_THROW(UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::HistoricalOnly),std::invalid_argument);
    // Explicit structural pool fixture; no admission/proving/signature claim.
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});pool.onBlockConnected(ConnectedBlockEffects{},110,verified->Root());
    ASSERT_TRUE(pool.publishProofPayload(*verified));const auto entry=pool.getMempoolEntry(body.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_TRUE(entry->tx.IsOrchard());EXPECT_EQ(entry->cached_utxotx_payload,*wire);EXPECT_THROW(entry->tx.Historical(),std::logic_error);
}
#endif
}
