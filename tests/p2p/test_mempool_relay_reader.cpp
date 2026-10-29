// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "daemon/tx_relay_manager.h"
#include "daemon/relay_transaction_reader.h"
#include "daemon/interfaces/tx_ingress.h"
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
namespace {
using namespace dinero;
class MempoolRelayReader : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_relay_reader_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
};
TEST_F(MempoolRelayReader, SignedHistoricalRoundTripAndActualAdmission) {
    const auto tx=spend(fund(20),1000000,1000);auto wire=tx.Serialize();
    const auto historical=DecodeRelayTransaction(wire,RelayTransactionReadMode::HistoricalOnly);
    auto available=DecodeRelayTransaction(wire,RelayTransactionReadMode::AvailableFamilies);
    wire.clear();wire.shrink_to_fit();
    EXPECT_FALSE(historical.IsOrchard());EXPECT_EQ(historical.Serialize(),tx.Serialize());
    EXPECT_EQ(available.Serialize(),tx.Serialize());EXPECT_EQ(available.GetTxid(),tx.GetTxid());
    EXPECT_EQ(available.GetWeight(),tx.GetWeight());
    Mempool pool(&db,&coins);TxRelayManager relay(nullptr);
    relay.SetSubmitBodyCallback([&](const MempoolTransaction& body,const auto& peer){return pool.submitTransaction(body.Historical(),peer,false);});
    relay.HandleTx("decoded-peer",available);available=MempoolTransaction{};
    ASSERT_TRUE(pool.hasTransaction(tx.GetTxid().AsUint256()));
    const auto entry=pool.getMempoolEntry(tx.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_EQ(entry->tx.Serialize(),tx.Serialize());EXPECT_TRUE(relay.IsTxSeen(tx.GetTxid().AsUint256()));
}
TEST_F(MempoolRelayReader, HistoricalBoundsAndExactConsumption) {
    const auto tx=spend(fund(21),1000000,1000);auto wire=tx.Serialize();
    EXPECT_THROW(DecodeRelayTransaction({},RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
    EXPECT_THROW(DecodeRelayTransaction(wire,static_cast<RelayTransactionReadMode>(99)),std::invalid_argument);
    wire.push_back(0);
    EXPECT_THROW(DecodeRelayTransaction(wire,RelayTransactionReadMode::HistoricalOnly),std::invalid_argument);
    EXPECT_THROW(DecodeRelayTransaction(wire,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
    const std::vector<uint8_t> claimed_family{7,0,0,0,0,0};
    EXPECT_THROW(DecodeRelayTransaction(claimed_family,RelayTransactionReadMode::HistoricalOnly),std::invalid_argument);
    EXPECT_THROW(DecodeRelayTransaction(claimed_family,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
}
#ifdef DINERO_TEST_ORCHARD_BODY
std::vector<uint8_t> CanonicalRelayFixture() {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES)/"candidate-envelope.bin",std::ios::binary);
    if(!file.good())throw std::runtime_error("fixture unavailable");
    return {std::istreambuf_iterator<char>(file),{}};
}
struct ReaderHistoricalIngress final:ITxIngress {
    size_t submissions=0;
    TxAcceptResult Submit(const Transaction& tx,TxOrigin)override{++submissions;return TxAcceptResult::Accepted(tx.GetTxid().AsUint256());}
    bool HasTransaction(const uint256&)const override{return false;}
    std::shared_ptr<Transaction> GetTransaction(const uint256&)const override{return {};}
};
TEST_F(MempoolRelayReader, OrchardWireAndUnavailableIngress) {
    auto wire=CanonicalRelayFixture();const auto expected=wire;
    auto body=DecodeRelayTransaction(wire,RelayTransactionReadMode::AvailableFamilies);
    wire.clear();wire.shrink_to_fit();ASSERT_TRUE(body.IsOrchard());EXPECT_EQ(body.Serialize(),expected);
    EXPECT_THROW(body.Historical(),std::logic_error);
    ReaderHistoricalIngress ingress;TxRelayManager relay(nullptr);size_t sent=0,unavailable=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});
    relay.SetSubmitBodyCallback([&](const MempoolTransaction& captured,const auto&){
        EXPECT_EQ(captured.Serialize(),expected);const auto result=ingress.SubmitBody(captured,TxOrigin::P2P);
        EXPECT_EQ(result.code,TxRejectCode::UNAVAILABLE);++unavailable;return result;
    });
    relay.HandleTx("decoded-peer",body);EXPECT_EQ(unavailable,1U);EXPECT_EQ(ingress.submissions,0U);EXPECT_EQ(sent,0U);
    EXPECT_FALSE(relay.IsTxSeen(body.GetTxid().AsUint256()));
}
TEST_F(MempoolRelayReader, ExplicitFamilyPermissionAndWholeMessage) {
    auto wire=CanonicalRelayFixture();
    EXPECT_THROW(DecodeRelayTransaction(wire,RelayTransactionReadMode::HistoricalOnly),std::invalid_argument);
    const auto body=DecodeRelayTransaction(wire,RelayTransactionReadMode::AvailableFamilies);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(wire);
    EXPECT_EQ(body.Orchard().Txid(),envelope.Txid());EXPECT_EQ(body.Orchard().Wtxid(),envelope.Wtxid());
    wire.push_back(0);
    EXPECT_THROW(DecodeRelayTransaction(wire,RelayTransactionReadMode::AvailableFamilies),std::invalid_argument);
}
#endif
}
