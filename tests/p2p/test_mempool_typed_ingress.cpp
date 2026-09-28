// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "daemon/tx_relay_manager.h"
#include "daemon/interfaces/tx_ingress.h"
#include "mempool/tx_orphan_pool.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include <gtest/gtest.h>
#include <chrono>
#include <algorithm>
#include <type_traits>
#ifdef DINERO_TEST_ORCHARD_BODY
#include "orchard_transaction.h"
#include <fstream>
#include <iterator>
#endif
#include <filesystem>
namespace dinero {
class MempoolTypedIngressTestPeer {
public:
    static void SetEmptyView(Mempool& pool, std::unique_ptr<consensus::ChainStateView> view) {
        std::unique_lock lock(pool.m_mutex);
        if (!pool.m_transactions.empty()) throw std::logic_error("fixture requires empty pool");
        pool.chain_state_view_ = std::move(view);
        pool.coins_view_ = CoinsViewMemPool(pool.chain_state_view_.get());
    }
};
}
namespace {
using namespace dinero;
class MempoolTypedIngress : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_typed_ingress_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        secret.back()=70;int parity=0;std::array<uint8_t,32> output{};
        ASSERT_TRUE(TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));ASSERT_TRUE(TaprootKeys::ComputeTweakedPubkey(internal,output));
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
    }
    void TearDown() override {db.close();std::filesystem::remove_all(root);}
    OutPoint fund(uint8_t id) {
        uint256 hash;hash.data[0]=id;OutPoint out{TxId(hash),0};
        if(!coins.AddCoin(out,{AmountUna::Una(1000000),script,1,false}))throw std::runtime_error("fixture funding refused");return out;
    }
    Transaction spend(const OutPoint& out,uint64_t value,uint64_t fee) {
        Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=out.vout;tx.vin[0].sequence=0xfffffffd;
        tx.vout.emplace_back(AmountUna::Una(value-fee),script);
        CanonicalWalletUTXO coin;coin.txid=out.txid.AsUint256();coin.vout=out.vout;coin.value=AmountUna::Una(value);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});if(bytes.size()!=32)throw std::runtime_error("fixture sighash refused");
        std::array<uint8_t,32> hash{};std::copy(bytes.begin(),bytes.end(),hash.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,hash,secret,internal))throw std::runtime_error("fixture signature refused");tx.vin[0].witness.emplace_back(signature.begin(),signature.end());return tx;
    }
    Transaction join(const Transaction& first, const Transaction& second) {
        Transaction tx; tx.version=2;
        const std::array<Transaction,2> parents{first,second};
        std::vector<CanonicalWalletUTXO> prev;
        uint64_t total=0;
        for(const auto& parent:parents) {
            tx.vin.emplace_back(); auto& input=tx.vin.back();
            input.prevout.txid=parent.GetTxid(); input.prevout.vout=0; input.sequence=0xfffffffd;
            CanonicalWalletUTXO coin; coin.txid=parent.GetTxid().AsUint256(); coin.vout=0;
            coin.value=parent.vout[0].value; coin.spk=parent.vout[0].scriptPubKey;
            total+=coin.value.GetUna(); prev.push_back(coin);
        }
        tx.vout.emplace_back(AmountUna::Una(total-5000),script);
        for(size_t i=0;i<tx.vin.size();++i) {
            const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,i,prev);
            if(bytes.size()!=32)throw std::runtime_error("join sighash refused");
            std::array<uint8_t,32> hash{};std::copy(bytes.begin(),bytes.end(),hash.begin());
            std::array<uint8_t,64> signature{};
            if(!TaprootKeys::SignSchnorrWithInternalKey(signature,hash,secret,internal))throw std::runtime_error("join signature refused");
            tx.vin[i].witness.emplace_back(signature.begin(),signature.end());
        }
        return tx;
    }
};
TEST_F(MempoolTypedIngress, CapturedHistoricalAdmissionAndCallbackReplacement) {
    auto tx=spend(fund(11),1000000,1000); const auto original=tx;
    Mempool pool(&db,&coins); TxRelayManager relay(nullptr);size_t submitted=0,replacement=0;
    relay.SetSendMessageCallback([](const auto&,const auto&,const auto&){});
    relay.SetSubmitBodyCallback([&](const MempoolTransaction& body,const auto& peer) {
        ++submitted;EXPECT_EQ(body.Serialize(),original.Serialize());
        tx.vout[0].value=AmountUna::Una(2); // caller-owned object, same thread after capture
        relay.SetSubmitBodyCallback([&](const auto&,const auto&){++replacement;return TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,"replacement");});
        return pool.submitTransaction(body.Historical(),peer,false);
    });
    relay.HandleTx("source",tx);
    EXPECT_EQ(submitted,1U);EXPECT_EQ(replacement,0U);
    ASSERT_TRUE(pool.hasTransaction(original.GetTxid().AsUint256()));
    EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));
    EXPECT_TRUE(relay.IsTxSeen(original.GetTxid().AsUint256()));
    relay.HandleTx("source",tx);EXPECT_EQ(replacement,1U);
}
TEST_F(MempoolTypedIngress, MissingSecondParentRemainsRetryable) {
    const auto first=spend(fund(12),1000000,1000);
    const auto second=spend(fund(13),1000000,1000);
    const auto child=join(first,second);const auto child_id=child.GetTxid().AsUint256();
    Mempool pool(&db,&coins);TxOrphanPool orphans;TxRelayManager relay(nullptr);
    relay.SetOrphanPool(&orphans);relay.SetSendMessageCallback([](const auto&,const auto&,const auto&){});
    relay.SetSubmitBodyCallback([&](const MempoolTransaction& body,const auto& peer){return pool.submitTransaction(body.Historical(),peer,false);});
    const auto preflight=pool.submitTransactionTestOnly(child,"fixture-preflight");
    EXPECT_EQ(preflight.code,TxRejectCode::MISSING_INPUTS) << preflight.message;
    relay.HandleTx("source",child);ASSERT_TRUE(orphans.hasOrphan(child_id));EXPECT_FALSE(relay.IsTxSeen(child_id));
    relay.HandleTx("source",first);
    ASSERT_TRUE(pool.hasTransaction(first.GetTxid().AsUint256()));
    EXPECT_TRUE(orphans.hasOrphan(child_id));EXPECT_FALSE(pool.hasTransaction(child_id));
    EXPECT_EQ(orphans.totalBytes(),child.GetSize());
    relay.HandleTx("source",second);
    ASSERT_TRUE(pool.hasTransaction(child_id));EXPECT_TRUE(relay.IsTxSeen(child_id));
    EXPECT_EQ(orphans.size(),0U);EXPECT_EQ(orphans.totalBytes(),0U);
    EXPECT_TRUE(orphans.getOrphanBodiesForParent(first.GetTxid().AsUint256()).empty());
    EXPECT_TRUE(orphans.getOrphanBodiesForParent(second.GetTxid().AsUint256()).empty());
}
TEST_F(MempoolTypedIngress, HistoricalAdapterSharesCallbackOwner) {
    const auto tx=spend(fund(14),1000000,1000);TxRelayManager relay(nullptr);
    size_t historical=0,typed=0;
    relay.SetSubmitBodyCallback([&](const auto&,const auto&){++typed;return TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,"typed");});
    relay.SetSubmitTxCallback([&](const Transaction& actual,const auto&){++historical;EXPECT_EQ(actual.Serialize(),tx.Serialize());return TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,"historical");});
    relay.HandleTx("source",MempoolTransaction(tx));EXPECT_EQ(historical,1U);EXPECT_EQ(typed,0U);
    relay.SetSubmitBodyCallback([&](const auto&,const auto&){++typed;return TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,"typed");});
    relay.HandleTx("source",tx);EXPECT_EQ(historical,1U);EXPECT_EQ(typed,1U);
    relay.SetSubmitTxCallback({});relay.HandleTx("source",tx);relay.HandleTx("source",MempoolTransaction{});
    EXPECT_EQ(typed,1U);EXPECT_FALSE(relay.IsTxSeen(tx.GetTxid().AsUint256()));
    const auto parent=spend(fund(16),1000000,1000);
    const auto child=spend(OutPoint(parent.GetTxid(),0),999000,1000);
    const auto child_id=child.GetTxid().AsUint256();
    Mempool pool(&db,&coins);TxOrphanPool orphans;relay.SetOrphanPool(&orphans);
    ASSERT_TRUE(orphans.addOrphan(child,"retained-owner"));
    relay.SetSubmitBodyCallback([&](const MempoolTransaction& body,const auto& peer){
        if(body.GetTxid()==child.GetTxid())return TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,"temporary validator outage",child_id);
        return pool.submitTransaction(body.Historical(),peer,false);
    });
    relay.HandleTx("source",parent);
    ASSERT_TRUE(pool.hasTransaction(parent.GetTxid().AsUint256()));
    EXPECT_TRUE(orphans.hasOrphan(child_id));EXPECT_FALSE(relay.IsTxSeen(child_id));
    EXPECT_EQ(orphans.totalBytes(),child.GetSize());
    const auto pending=orphans.getOrphanBodiesForParent(parent.GetTxid().AsUint256());
    ASSERT_EQ(pending.size(),1U);EXPECT_EQ(pending[0].Serialize(),child.Serialize());
    const auto bad_index=spend(OutPoint(parent.GetTxid(),1),999000,1000);
    EXPECT_EQ(pool.submitTransactionTestOnly(bad_index,"known-parent-index").code,TxRejectCode::INVALID_TX);
    EXPECT_EQ(pool.size(),1U);
    struct UnavailableView final : consensus::ChainStateView {
        StatusOr<consensus::UTXOEntry> getCoin(const OutPoint&) const override { return Status::Invalid; }
        bool hasCoin(const OutPoint&) const override { return false; }
        uint32_t getHeight() const override { return 110; }
    };
    Mempool unavailable_pool(&db,&coins);
    MempoolTypedIngressTestPeer::SetEmptyView(unavailable_pool,std::make_unique<UnavailableView>());
    EXPECT_EQ(unavailable_pool.submitTransactionTestOnly(tx,"backend-refusal").code,TxRejectCode::UNAVAILABLE);
    EXPECT_EQ(unavailable_pool.size(),0U);
    Mempool no_chain(nullptr);
    EXPECT_EQ(no_chain.submitTransactionTestOnly(tx,"unavailable-fixture").code,TxRejectCode::UNAVAILABLE);
    EXPECT_EQ(no_chain.size(),0U);
    Mempool no_guard(&db,&coins);
    no_guard.setChainstateReadGuardFactory([]{return std::unique_ptr<Mempool::ChainstateReadGuard>{};});
    EXPECT_EQ(no_guard.submitTransactionTestOnly(tx,"unavailable-fixture").code,TxRejectCode::UNAVAILABLE);
    EXPECT_EQ(no_guard.size(),0U);
}
#ifdef DINERO_TEST_ORCHARD_BODY
MempoolTransaction OrchardBody() {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES)/"candidate-envelope.bin",std::ios::binary);
    if(!file.good())throw std::runtime_error("fixture unavailable");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    return MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(bytes));
}
struct HistoricalIngress final:ITxIngress {
    size_t submissions=0;
    TxAcceptResult Submit(const Transaction& tx,TxOrigin) override {++submissions;return TxAcceptResult::Accepted(tx.GetTxid().AsUint256());}
    bool HasTransaction(const uint256&)const override{return false;}
    std::shared_ptr<Transaction> GetTransaction(const uint256&)const override{return {};}
};
TEST_F(MempoolTypedIngress, OrchardUnavailableBeforeLegacyValidation) {
    const auto body=OrchardBody();HistoricalIngress ingress;
    const auto result=ingress.SubmitBody(body,TxOrigin::P2P);
    EXPECT_EQ(result.code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(ingress.submissions,0U);
    EXPECT_EQ(result.txid,body.GetTxid().AsUint256());
    TxRelayManager relay(nullptr);TxOrphanPool orphans;relay.SetOrphanPool(&orphans);
    size_t legacy=0,sent=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});
    relay.SetValidateTxCallback([&](const auto&,const auto&){++legacy;return true;});
    relay.SetSubmitTxCallback([&](const auto& tx,const auto&){++legacy;return TxAcceptResult::Accepted(tx.GetTxid().AsUint256());});
    relay.HandleTx("source",body);EXPECT_EQ(legacy,0U);EXPECT_EQ(sent,0U);EXPECT_EQ(orphans.size(),0U);
    relay.SetSubmitTxCallback({});relay.HandleTx("source",body);EXPECT_EQ(legacy,0U);
    EXPECT_FALSE(relay.IsTxSeen(body.GetTxid().AsUint256()));
    const auto historical=spend(fund(15),1000000,1000);
    EXPECT_TRUE(ingress.SubmitBody(MempoolTransaction(historical),TxOrigin::P2P).accepted());
    EXPECT_EQ(ingress.submissions,1U);
}
TEST_F(MempoolTypedIngress, MixedOrphanInventoryOwnsCanonicalBodies) {
    auto body=OrchardBody();ASSERT_FALSE(body.Inputs().empty());
    const auto wire=body.Serialize();const auto id=body.GetTxid().AsUint256();
    const auto parent=body.Inputs()[0].txid.AsUint256();
    const auto historical=spend(body.Inputs()[0],1000000,1000);
    TxOrphanPool orphans;ASSERT_TRUE(orphans.addOrphan(body,"orchard-owner"));
    ASSERT_TRUE(orphans.addOrphan(historical,"historical-owner"));
    body=MempoolTransaction{};
    const auto captured=orphans.getOrphanBodiesForParent(parent);ASSERT_EQ(captured.size(),2U);
    EXPECT_EQ(orphans.totalBytes(),wire.size()+historical.GetSize());
    EXPECT_THROW(orphans.getOrphansForParent(parent),std::logic_error);
    EXPECT_EQ(orphans.size(),2U);EXPECT_FALSE(orphans.addOrphan(MempoolTransaction{},"empty"));
    orphans.eraseOrphansForPeer("historical-owner");EXPECT_EQ(orphans.size(),1U);EXPECT_EQ(orphans.totalBytes(),wire.size());
    const auto remaining=orphans.getOrphanBodiesForParent(parent);ASSERT_EQ(remaining.size(),1U);EXPECT_EQ(remaining[0].Serialize(),wire);
    orphans.eraseOrphan(id);EXPECT_EQ(orphans.size(),0U);EXPECT_EQ(orphans.totalBytes(),0U);
    EXPECT_TRUE(orphans.getPeerOrphanCounts().empty());EXPECT_TRUE(orphans.getOrphanBodiesForParent(parent).empty());
    size_t found=0;for(const auto& saved:captured)if(saved.IsOrchard()){EXPECT_EQ(saved.Serialize(),wire);++found;}
    EXPECT_EQ(found,1U);
}
#endif
}
