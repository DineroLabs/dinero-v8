// Benign job policy selection checks on the patched implementation only.
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/block_validation.h"
#include "daemon/mempool.h"
#include "daemon/block_relay_manager.h"
#include "mining/block_assembler.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include <gtest/gtest.h>
#include <set>
#include <filesystem>
#include <chrono>
namespace {
using namespace dinero;
class MiningJobPolicy : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::unique_ptr<consensus::BlockValidator> validator;
    static constexpr const char* address="din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44";
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mining_job_policy_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);
        const auto genesis=BuildCanonicalGenesis(Params());Block block{};block.header=genesis.header;
        const auto hash=block.GetHash();auto token=ChainWriteToken::CreateForTesting();
        ASSERT_EQ(db.putBlock(token,hash,block),Status::Ok);
        ASSERT_EQ(db.putHeader(token,hash,genesis.header,0,arith_uint256(0)),Status::Ok);
        ASSERT_EQ(db.setTip(token,hash,0,arith_uint256(0)),Status::Ok);
        coins.SetBestBlock(hash,0);validator=std::make_unique<consensus::BlockValidator>(&coins);
    }
    void TearDown() override {validator.reset();db.close();std::filesystem::remove_all(root);}
    void wire(BlockAssembler& assembler) {
        assembler.SetConsensusUTXOSet(&coins);assembler.SetBlockValidator(validator.get());
        assembler.SetUTXOProvider(std::shared_ptr<consensus::IUTXOProvider>(&coins,[](auto*){}));
        assembler.SetMiningAddress(address);
    }
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    Transaction payment(uint8_t id,uint64_t fee,unsigned extra=0) {
        secret.back()=67;int parity=0;std::array<uint8_t,32> output{};
        if(!TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity)||!TaprootKeys::ComputeTweakedPubkey(internal,output))throw std::runtime_error("fixture key");
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        uint256 hash;hash.data[0]=id;OutPoint out{TxId(hash),0};
        if(!coins.AddCoin(out,{AmountUna::Una(1000000),script,0,false}))throw std::runtime_error("fixture funding");
        if(coins.GetForest().add(consensus::HashUTXOForCreationHeight(hash,0,1000000,script,0,false))==UINT64_MAX)throw std::runtime_error("fixture forest funding");
        Coin record;record.amount=1000000;for(auto c:script){static constexpr char h[]="0123456789abcdef";record.script_pubkey+=h[c>>4];record.script_pubkey+=h[c&15];}record.height=0;record.coinbase=false;
        if(db.putCoin(ChainWriteToken::CreateForTesting(),hash,0,record)!=Status::Ok)throw std::runtime_error("fixture durable funding");
        Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=0;tx.vin[0].sequence=0xfffffffd;
        tx.vout.emplace_back(AmountUna::Una(1000000-fee-10000*extra),script);
        for(unsigned i=0;i<extra;++i)tx.vout.emplace_back(AmountUna::Una(10000),script);
        CanonicalWalletUTXO coin;coin.txid=hash;coin.vout=0;coin.value=AmountUna::Una(1000000);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});if(bytes.size()!=32)throw std::runtime_error("fixture sighash");
        std::array<uint8_t,32> digest{};std::copy(bytes.begin(),bytes.end(),digest.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,digest,secret,internal))throw std::runtime_error("fixture signature");
        tx.vin[0].witness.emplace_back(signature.begin(),signature.end());return tx;
    }
    Transaction child(const Transaction& parent,uint32_t index,uint64_t fee) {
        const auto value=parent.vout.at(index).value.GetUna();
        Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=parent.GetTxid();tx.vin[0].prevout.vout=index;tx.vin[0].sequence=0xfffffffd;
        tx.vout.emplace_back(AmountUna::Una(value-fee),script);
        CanonicalWalletUTXO coin;coin.txid=parent.GetTxid().AsUint256();coin.vout=index;coin.value=AmountUna::Una(value);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});if(bytes.size()!=32)throw std::runtime_error("fixture child sighash");
        std::array<uint8_t,32> digest{};std::copy(bytes.begin(),bytes.end(),digest.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,digest,secret,internal))throw std::runtime_error("fixture child signature");
        tx.vin[0].witness.emplace_back(signature.begin(),signature.end());return tx;
    }
};
TEST_F(MiningJobPolicy, JobAppliesParentPolicyAndRetry) {
    Mempool pool(&db,&coins);const auto parent=payment(41,10000,20),descendant=child(parent,0,50000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    ASSERT_TRUE(pool.submitTransaction(descendant,"fixture",false).accepted());
    const auto captured=pool.CaptureBlockSelection(1000000,4000000,1);
    const auto child_vwu=captured.metadata.at(descendant.GetTxid().AsUint256()).vwu;
    ASSERT_GT(captured.metadata.at(parent.GetTxid().AsUint256()).vwu,child_vwu);
    BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
    BlockRelayManager relay(nullptr);assembler.SetBlockRelayManager(&relay);
    for(bool intelligent:{false,true}) {
        assembler.SetIntelligentSelection(intelligent);assembler.SetMaxBlockVWU(child_vwu);
        auto job=assembler.CreateJob();ASSERT_NE(job,nullptr);EXPECT_EQ(job->transactions.size(),1U);
        EXPECT_EQ(job->total_fees,0U);EXPECT_EQ(pool.size(),2U);
        assembler.SetMaxBlockVWU(0);job=assembler.CreateJob();ASSERT_NE(job,nullptr);ASSERT_EQ(job->transactions.size(),3U);
        EXPECT_EQ(job->transactions[1].GetTxid(),parent.GetTxid());EXPECT_EQ(job->transactions[2].GetTxid(),descendant.GetTxid());
        EXPECT_EQ(job->total_fees,60000U);EXPECT_EQ(pool.size(),2U);
    }
}
TEST_F(MiningJobPolicy, JobIncludesSharedParentExactlyOnce) {
    Mempool pool(&db,&coins);const auto parent=payment(42,1000,1),first=child(parent,0,5000),second=child(parent,1,1000);
    for(const auto& tx:{parent,first,second})ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
    BlockRelayManager relay(nullptr);assembler.SetBlockRelayManager(&relay);assembler.SetIntelligentSelection(true);
    auto job=assembler.CreateJob();ASSERT_NE(job,nullptr);ASSERT_EQ(job->transactions.size(),4U);
    EXPECT_EQ(job->transactions[1].GetTxid(),parent.GetTxid());std::set<TxId> ids;
    for(size_t i=1;i<job->transactions.size();++i)ids.insert(job->transactions[i].GetTxid());
    EXPECT_EQ(ids.size(),3U);EXPECT_EQ(job->total_fees,7000U);EXPECT_EQ(pool.size(),3U);
}
TEST_F(MiningJobPolicy, JobHonorsExactWeightBoundary) {
    Mempool pool(&db,&coins);const auto parent=payment(43,1000),descendant=child(parent,0,10000);
    ASSERT_TRUE(pool.submitTransaction(parent,"fixture",false).accepted());
    ASSERT_TRUE(pool.submitTransaction(descendant,"fixture",false).accepted());
    BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
    BlockRelayManager relay(nullptr);assembler.SetBlockRelayManager(&relay);assembler.SetIntelligentSelection(true);
    const auto weight=parent.GetWeight()+descendant.GetWeight();assembler.SetMaxBlockWeight(weight);
    auto job=assembler.CreateJob();ASSERT_NE(job,nullptr);ASSERT_EQ(job->transactions.size(),3U);
    EXPECT_EQ(job->total_fees,11000U);EXPECT_EQ(job->transactions[1].GetTxid(),parent.GetTxid());EXPECT_EQ(job->transactions[2].GetTxid(),descendant.GetTxid());
    assembler.SetMaxBlockWeight(weight-1);job=assembler.CreateJob();ASSERT_NE(job,nullptr);
    uint64_t selected_weight=0;for(size_t i=1;i<job->transactions.size();++i){selected_weight+=job->transactions[i].GetWeight();EXPECT_NE(job->transactions[i].GetTxid(),descendant.GetTxid());}
    EXPECT_LT(selected_weight,weight);EXPECT_EQ(pool.size(),2U);
}
}
