#pragma once
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include "util/hex.h"

namespace dinero {
struct PreBaseLookupTestAccess {
    static void Initialize(ChainstateService& service,ChainDB& db) {
        service.chain_db_=&db;
        service.consensus_utxo_set_=std::make_unique<consensus::ConsensusUTXOSet>();
    }
    static void Active(ChainstateService& service,const uint256& hash,uint32_t height) {
        service.assumeutxo_active_=true;service.assumeutxo_base_block_=hash;
        service.assumeutxo_base_height_=height;service.promoted_base_height_=0;
    }
    static void Promoted(ChainstateService& service,uint32_t height) {
        service.assumeutxo_active_=false;service.assumeutxo_base_block_.SetNull();
        service.assumeutxo_base_height_=0;service.promoted_base_height_=height;
    }
};
}
namespace {
class PreBaseLookupStatus:public MiningChainGuard {
protected:
    static uint256 Hash(uint8_t n){uint256 h;h.data[0]=n;return h;}
    static OutPoint Point(){return {TxId(Hash(183)),0};}
    static Coin Frozen(){Coin c;c.amount=1000000;c.script_pubkey="51";c.height=5;c.coinbase=false;return c;}
    static consensus::UtreexoHash Leaf(const Coin& c){return consensus::HashUTXOForCreationHeight(Point().txid.AsUint256(),0,c.amount,{0x51},c.height,c.coinbase);}
    void Store(const uint256& base,const Coin& coin) {
        ASSERT_EQ(db.replacePreBaseCoins(ChainWriteToken::CreateForTesting(),base,10,{{Point().txid.AsUint256(),0,coin}}),Status::Ok);
    }
};
TEST_F(PreBaseLookupStatus, AbsentAndUnavailableAreDistinct) {
    ChainstateService service;
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::Internal);
    PreBaseLookupTestAccess::Initialize(service,db);
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::NotFound);
    EXPECT_FALSE(service.ResolveLivePreBaseCoin(Point()));
    db.close();EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::Internal);
    ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::NotFound);
    PreBaseLookupTestAccess::Active(service,Hash(184),10);
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::Corruption);
    EXPECT_EQ(db.getPreBaseCoinSetBase().status(),Status::NotFound);
    service.setChainDB(nullptr);
}
TEST_F(PreBaseLookupStatus, LiveFrozenAndPromotedBasePreserveScope) {
    ChainstateService service;PreBaseLookupTestAccess::Initialize(service,db);
    const auto base=Hash(184);const auto coin=Frozen();Store(base,coin);
    PreBaseLookupTestAccess::Active(service,base,10);
    auto* live=service.GetConsensusUTXOSet();uint64_t position=0;
    live->MutateForestGuarded([&](auto& forest){position=forest.add(Leaf(coin));});
    auto found=service.ResolveLivePreBaseCoinChecked(Point());ASSERT_TRUE(found.ok());
    EXPECT_EQ(found->height,5U);EXPECT_EQ(found->value.GetUna(),coin.amount);
    const auto root_before=live->SnapshotForestCommitment();
    db.close();ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);
    ASSERT_TRUE(service.ResolveLivePreBaseCoinChecked(Point()).ok());
    EXPECT_EQ(live->SnapshotForestCommitment(),root_before);
    bool removed=false;live->MutateForestGuarded([&](auto& forest){removed=forest.removeAtKnownPosition(position,Leaf(coin));});ASSERT_TRUE(removed);
    ASSERT_EQ(db.putCoin(ChainWriteToken::CreateForTesting(),Point().txid.AsUint256(),0,coin),Status::Ok);
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::NotFound);
    EXPECT_FALSE(service.ResolveLivePreBaseCoin(Point())); // stale auxiliary row is not authority
    ASSERT_TRUE(db.getPreBaseCoin(Point().txid.AsUint256(),0).ok());
    live->MutateForestGuarded([&](auto& forest){forest.add(Leaf(coin));});
    PreBaseLookupTestAccess::Promoted(service,10);
    ASSERT_EQ(db.putHeightIndex(ChainWriteToken::CreateForTesting(),10,base),Status::Ok);
    ASSERT_TRUE(service.ResolveLivePreBaseCoinChecked(Point()).ok());
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point())->height,5U);
    ASSERT_EQ(db.putHeightIndex(ChainWriteToken::CreateForTesting(),10,Hash(185)),Status::Ok);
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::Corruption);
    ASSERT_EQ(db.putHeightIndex(ChainWriteToken::CreateForTesting(),10,base),Status::Ok);
    auto malformed=coin;malformed.script_pubkey="invalid-hex";Store(base,malformed);
    EXPECT_EQ(service.ResolveLivePreBaseCoinChecked(Point()).status(),Status::Corruption);
    Store(base,coin);EXPECT_TRUE(service.ResolveLivePreBaseCoinChecked(Point()).ok());
    service.setChainDB(nullptr);
}
TEST_F(PreBaseLookupStatus, CanonicalAdmissionKeepsMissingAndUnavailableDistinct) {
    ChainstateService service;PreBaseLookupTestAccess::Initialize(service,db);
    const auto out=Point();std::array<uint8_t,32> secret{},internal{},output{};secret.back()=73;int parity=0;
    ASSERT_TRUE(TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));ASSERT_TRUE(TaprootKeys::ComputeTweakedPubkey(internal,output));
    std::vector<uint8_t> script{0x51,0x20};script.insert(script.end(),output.begin(),output.end());
    Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=out.vout;
    tx.vout.emplace_back(AmountUna::Una(990000),script);
    CanonicalWalletUTXO input;input.txid=out.txid.AsUint256();input.vout=0;input.value=AmountUna::Una(1000000);input.spk=script;
    const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{input});ASSERT_EQ(bytes.size(),32U);
    std::array<uint8_t,32> hash{};std::copy(bytes.begin(),bytes.end(),hash.begin());std::array<uint8_t,64> signature{};
    ASSERT_TRUE(TaprootKeys::SignSchnorrWithInternalKey(signature,hash,secret,internal));tx.vin[0].witness.emplace_back(signature.begin(),signature.end());
    Mempool pool(&db,&coins);unsigned reads=0;
    pool.setPreBaseCoinStatusResolver([&](const OutPoint& point){++reads;return service.ResolveLivePreBaseCoinChecked(point);});
    pool.setPreBaseCoinResolver([](const OutPoint&)->std::optional<consensus::UTXOEntry>{ADD_FAILURE()<<"checked result must not fall back";return std::nullopt;});
    auto result=pool.submitTransaction(tx,"checked fixture",false);
    EXPECT_EQ(result.code,TxRejectCode::MISSING_INPUTS);EXPECT_NE(result.message.find("UTXO not found"),std::string::npos);EXPECT_EQ(reads,1U);
    EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));
    service.setChainDB(nullptr);result=pool.submitTransaction(tx,"checked fixture",false);
    EXPECT_EQ(result.code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(reads,2U);EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));
    service.setChainDB(&db);ASSERT_TRUE(coins.AddCoin(out,{AmountUna::Una(1000000),script,0,false}));
    result=pool.submitTransaction(tx,"checked fixture",false);ASSERT_TRUE(result.accepted())<<result.message;
    EXPECT_EQ(reads,2U);EXPECT_TRUE(pool.hasTransaction(tx.GetTxid().AsUint256()));
    Block confirmed;confirmed.vtx.push_back(tx);pool.onBlockConnected(confirmed,1,{});ASSERT_TRUE(coins.SpendCoin(out));
    result=pool.submitTransaction(tx,"checked fixture",false);
    EXPECT_EQ(result.code,TxRejectCode::MISSING_INPUTS);EXPECT_NE(result.message.find("UTXO not found"),std::string::npos);
    EXPECT_FALSE(pool.hasTransaction(tx.GetTxid().AsUint256()));service.setChainDB(nullptr);
}
}
