// Prepared-owner tests; the inherited graph is structural, not admission.
#include "daemon/prepared_pool_tip.h"
#include <thread>
namespace {
class OrchardTypedPoolTip : public OrchardPoolConflicts {
protected:
#ifdef DINERO_TEST_ORCHARD_BODY
    std::shared_ptr<network::BridgeNode> bridgeFor(const OutPoint& funding) {
        const std::vector<uint8_t> script{0x51};
        if(!coins.AddCoin(funding,{AmountUna::Una(2000),script,1,false}))
            throw std::runtime_error("fixture bridge funding");
        coins.MutateForestGuarded([&](consensus::UtreexoForest& forest) {
            forest.add(consensus::HashUTXOForCreationHeight(funding.txid.AsUint256(),funding.vout,2000,script,1,false));
        });
        auto provider=std::shared_ptr<consensus::IUTXOProvider>(std::shared_ptr<void>{},&coins);
        return std::make_shared<network::BridgeNode>(provider,&coins.GetForest(),nullptr,nullptr,nullptr,&coins);
    }
#endif
};
TEST_F(OrchardTypedPoolTip, HistoricalAdapterAndEffectsHaveSamePublication) {
    const auto parent=ordinary({},51),child=ordinary({{parent.GetTxid(),0}},52);
    const std::vector<MempoolTransaction> bodies{MempoolTransaction(parent),MempoolTransaction(child)};
    Mempool historical(&db,&coins),typed(&db,&coins);
    MempoolOrchardConflictTestPeer::Install(historical,bodies);MempoolOrchardConflictTestPeer::Install(typed,bodies);
    Block block;block.vtx={parent};ConnectedBlockEffects effects;effects.confirmed_txids={parent.GetTxid().AsUint256()};
    {auto prepared=PreparedPoolTip::ConnectEffects(typed,{}, {},effects,111,{2});EXPECT_THROW(prepared->RequestRefresh(),std::logic_error);}
    EXPECT_EQ(ids(typed),ids(historical));
    {auto prepared=PreparedPoolTip::Connect(historical,{}, {},block,111,{2});prepared->PublishAfterCommit();prepared->RequestRefresh();}
    {auto prepared=PreparedPoolTip::ConnectEffects(typed,{}, {},effects,111,{2});prepared->PublishAfterCommit();prepared->RequestRefresh();}
    EXPECT_EQ(ids(typed),ids(historical));EXPECT_EQ(typed.size(),1U);EXPECT_EQ(typed.getTotalFees(),historical.getTotalFees());
    EXPECT_EQ(typed.getStats().last_connected_height,historical.getStats().last_connected_height);
    EXPECT_EQ(typed.getMempoolEntry(child.GetTxid().AsUint256())->tx.Serialize(),child.Serialize());
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(OrchardTypedPoolTip, NullifierConflictAbandonsOrPublishesAllCacheOwners) {
    const auto body=MempoolTransaction::FromOrchard(envelope());const auto child=ordinary({{body.GetTxid(),0}},53);
    uint256 funding_id;funding_id.data[0]=203;const OutPoint funding{TxId(funding_id),0};
    const auto survivor=ordinary({funding},54);const auto id=survivor.GetTxid().AsUint256();
    auto bridge=bridgeFor(funding);Mempool pool(&db,&coins);
    MempoolOrchardConflictTestPeer::Install(pool,{body,MempoolTransaction(child),MempoolTransaction(survivor)});
    ASSERT_TRUE(pool.refreshProof(id,{1},110));ASSERT_TRUE(bridge->GenerateProofsForTransaction(survivor));
    ASSERT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);
    auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);size_t sent=0;
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});relay->RequestProofRefresh({id});ASSERT_EQ(sent,1U);
    const auto before=ids(pool);const auto effects=conflictEffects();
    {auto prepared=PreparedPoolTip::ConnectEffects(pool,bridge,relay,effects,111,{2});ASSERT_TRUE(prepared);}
    EXPECT_EQ(ids(pool),before);EXPECT_EQ(pool.getStaleCount(),0U);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));relay->RequestProofRefresh({id});EXPECT_EQ(sent,1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    auto prepared=PreparedPoolTip::ConnectEffects(pool,bridge,relay,effects,111,{2});prepared->PublishAfterCommit();
    EXPECT_EQ(ids(pool),std::vector<uint256>{id});EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    EXPECT_EQ(pool.getStaleCount(),1U);EXPECT_EQ(pool.getStats().refresh_attempted_total,1U);
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){
        ++sent;EXPECT_EQ(ids(pool),std::vector<uint256>{id});EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
        relay->CompleteRefresh(id);
    });
    prepared->RequestRefresh();EXPECT_EQ(sent,2U);EXPECT_THROW(prepared->RequestRefresh(),std::logic_error);
}
TEST_F(OrchardTypedPoolTip, PreparationFailureAndAmbiguousRefreshRetainCorrectState) {
    const auto body=MempoolTransaction::FromOrchard(envelope());
    uint256 funding_id;funding_id.data[0]=204;const OutPoint funding{TxId(funding_id),0};
    const auto survivor=ordinary({funding},55);const auto id=survivor.GetTxid().AsUint256();auto bridge=bridgeFor(funding);
    Logger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);
    MempoolOrchardConflictTestPeer::Install(pool,{body,MempoolTransaction(survivor)});
    ASSERT_TRUE(pool.refreshProof(id,{1},110));ASSERT_TRUE(bridge->GenerateProofsForTransaction(survivor));
    auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);const auto effects=conflictEffects();const auto before=ids(pool);
    logger.fail=true;EXPECT_THROW(PreparedPoolTip::ConnectEffects(pool,bridge,relay,effects,111,{2}),std::runtime_error);logger.fail=false;
    EXPECT_EQ(ids(pool),before);EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);EXPECT_EQ(pool.getStaleCount(),0U);
    size_t attempts=0;relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){
        ++attempts;EXPECT_EQ(ids(pool),std::vector<uint256>{id});EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
        throw std::runtime_error("fixture ambiguous outbound refresh");
    });
    auto prepared=PreparedPoolTip::ConnectEffects(pool,bridge,relay,effects,111,{2});prepared->PublishAfterCommit();
    EXPECT_THROW(prepared->RequestRefresh(),std::runtime_error);EXPECT_EQ(attempts,1U);
    EXPECT_EQ(ids(pool),std::vector<uint256>{id});EXPECT_EQ(pool.getStats().last_connected_height,111U);
    EXPECT_THROW(prepared->RequestRefresh(),std::logic_error);EXPECT_EQ(attempts,1U);
}
#endif
}

#include "typed_bridge_proof_owner_checks.h"
