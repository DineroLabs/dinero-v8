#pragma once
// Healthy prepared rollback owners and immutable proof receipts; no unsafe originals.
namespace {
class PoolParentRoot : public TypedUtreexoReceive {};
TEST_F(PoolParentRoot, AbandonAndPublishKeepExactParentProofContext) {
    const MempoolTransaction body(ordinary({input(231)},81));install(body);
    const auto parent_wire=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(parent_wire);
    const auto parent_stump=stump();const auto parent_receipt=UtreexoTransactionPayload::Decode(*parent_wire,RelayTransactionReadMode::AvailableFamilies).VerifyInputs(parent_stump,110);ASSERT_TRUE(parent_receipt);
    const auto parent_forest=[&]{auto guard=coins.LockForestShared();return coins.GetForest();}();
    uint256 extra;extra.data[0]=232;
    coins.MutateForestGuarded([&](auto& f){f.add(consensus::HashUTXOForCreationHeight(extra,0,3000,{0x51},111,false));});
    const auto child_wire=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(child_wire);
    const auto child_receipt=UtreexoTransactionPayload::Decode(*child_wire,RelayTransactionReadMode::AvailableFamilies).VerifyInputs(stump(),111);ASSERT_TRUE(child_receipt);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});pool.onBlockConnected(ConnectedBlockEffects{},111,child_receipt->Root());ASSERT_TRUE(pool.publishProofPayload(*child_receipt));
    auto shared_bridge=std::shared_ptr<network::BridgeNode>(std::move(bridge));
    {auto prepared=PreparedPoolTip::DisconnectToParent(pool,shared_bridge,{},111,parent_receipt->Root());ASSERT_TRUE(prepared);}
    EXPECT_EQ(pool.getCachedUtxoTxPayload(body.GetTxid().AsUint256()),child_wire);EXPECT_TRUE(pool.publishProofPayload(*child_receipt));EXPECT_FALSE(pool.publishProofPayload(*parent_receipt));EXPECT_EQ(shared_bridge->GetCacheSnapshot().tx_entries,1U);EXPECT_EQ(pool.getStats().last_connected_height,111U);
    coins.MutateForestGuarded([&](auto& f){f=parent_forest;});
    {auto prepared=PreparedPoolTip::DisconnectToParent(pool,shared_bridge,{},111,parent_receipt->Root());prepared->PublishAfterCommit();prepared->RequestRefresh();}
    EXPECT_FALSE(pool.getCachedUtxoTxPayload(body.GetTxid().AsUint256()));EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(shared_bridge->GetCacheSnapshot().tx_entries,0U);EXPECT_EQ(pool.getStats().last_connected_height,110U);
    EXPECT_FALSE(pool.publishProofPayload(*child_receipt));ASSERT_TRUE(pool.publishProofPayload(*parent_receipt));
    EXPECT_EQ(pool.getCachedUtxoTxPayload(body.GetTxid().AsUint256()),parent_wire);EXPECT_EQ(pool.getMempoolEntry(body.GetTxid().AsUint256())->validated_at_height,110U);EXPECT_EQ(pool.getMempoolEntry(body.GetTxid().AsUint256())->tx.Serialize(),body.Serialize());
}
TEST_F(PoolParentRoot, InvalidContextAndCacheOnlyEntryPreserveOwnerRules) {
    const MempoolTransaction body(ordinary({input(233)},82));install(body);const auto wire=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(wire);
    const auto receipt=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies).VerifyInputs(stump(),109);ASSERT_TRUE(receipt);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});pool.onBlockConnected(ConnectedBlockEffects{},110,receipt->Root());
    const auto id=body.GetTxid().AsUint256();ASSERT_TRUE(pool.setCachedUtxoTxPayload(id,{4,5,6}));ASSERT_TRUE(pool.getMempoolEntry(id)->validated_at_root.empty());
    EXPECT_THROW(pool.prepareBlockDisconnectedToParent(0,receipt->Root()),std::invalid_argument);
    EXPECT_THROW(pool.prepareBlockDisconnectedToParent(110,{1,2,3}),std::invalid_argument);
    EXPECT_EQ(pool.getCachedUtxoTxPayload(id),std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{4,5,6}));EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_FALSE(pool.publishProofPayload(*receipt));
    {auto prepared=pool.prepareBlockDisconnectedToParent(110,receipt->Root());} // no publication
    EXPECT_TRUE(pool.getCachedUtxoTxPayload(id));EXPECT_EQ(pool.getStaleCount(),0U);
    {auto prepared=pool.prepareBlockDisconnectedToParent(110,receipt->Root());prepared->PublishAfterCommit();}
    EXPECT_FALSE(pool.getCachedUtxoTxPayload(id));EXPECT_EQ(pool.getStaleCount(),1U);ASSERT_TRUE(pool.publishProofPayload(*receipt));EXPECT_EQ(pool.getCachedUtxoTxPayload(id),wire);
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(PoolParentRoot, TypedFamilyAndRefreshFailureRetainPublishedParent) {
    const auto body=MempoolTransaction::FromOrchard(envelope());install(body);const auto wire=CaptureUtreexoTransactionPayload(body,*bridge);ASSERT_TRUE(wire);
    const auto receipt=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies).VerifyInputs(stump(),109);ASSERT_TRUE(receipt);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});pool.onBlockConnected(ConnectedBlockEffects{},110,receipt->Root());ASSERT_TRUE(pool.refreshProof(body.GetTxid().AsUint256(),receipt->Root(),110));
    auto shared_bridge=std::shared_ptr<network::BridgeNode>(std::move(bridge));auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);unsigned attempts=0;
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++attempts;EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(shared_bridge->GetCacheSnapshot().tx_entries,0U);throw std::runtime_error("fixture ambiguous refresh");});
    auto prepared=PreparedPoolTip::DisconnectToParent(pool,shared_bridge,relay,110,receipt->Root());prepared->PublishAfterCommit();EXPECT_THROW(prepared->RequestRefresh(),std::runtime_error);EXPECT_EQ(attempts,1U);
    ASSERT_TRUE(pool.publishProofPayload(*receipt));const auto entry=pool.getMempoolEntry(body.GetTxid().AsUint256());ASSERT_TRUE(entry);EXPECT_TRUE(entry->tx.IsOrchard());EXPECT_EQ(entry->tx.Serialize(),body.Serialize());EXPECT_EQ(entry->validated_at_height,109U);EXPECT_EQ(entry->cached_utxotx_payload,*wire);
    EXPECT_THROW(prepared->RequestRefresh(),std::logic_error);EXPECT_EQ(attempts,1U);
}
#endif
}
