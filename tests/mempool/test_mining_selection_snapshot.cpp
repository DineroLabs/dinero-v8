// Benign selection and fee snapshot checks on the patched implementation only.
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/block_validation.h"
#include "daemon/mempool.h"
#include "mining/block_assembler.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <chrono>
namespace {
using namespace dinero;
class MiningSelectionSnapshot : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::unique_ptr<consensus::BlockValidator> validator;
    static constexpr const char* address="din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44";
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mining_selection_snapshot_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    Transaction payment(uint8_t id,uint64_t fee) {
        secret.back()=67;int parity=0;std::array<uint8_t,32> output{};
        if(!TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity)||!TaprootKeys::ComputeTweakedPubkey(internal,output))throw std::runtime_error("fixture key");
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        uint256 hash;hash.data[0]=id;OutPoint out{TxId(hash),0};
        if(!coins.AddCoin(out,{AmountUna::Una(1000000),script,0,false}))throw std::runtime_error("fixture funding");
        if(coins.GetForest().add(consensus::HashUTXOForCreationHeight(hash,0,1000000,script,0,false))==UINT64_MAX)throw std::runtime_error("fixture forest funding");
        Coin record;record.amount=1000000;for(auto c:script){static constexpr char h[]="0123456789abcdef";record.script_pubkey+=h[c>>4];record.script_pubkey+=h[c&15];}record.height=0;record.coinbase=false;
        if(db.putCoin(ChainWriteToken::CreateForTesting(),hash,0,record)!=Status::Ok)throw std::runtime_error("fixture durable funding");
        Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=0;tx.vin[0].sequence=0xfffffffd;
        tx.vout.emplace_back(AmountUna::Una(1000000-fee),script);
        CanonicalWalletUTXO coin;coin.txid=hash;coin.vout=0;coin.value=AmountUna::Una(1000000);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});if(bytes.size()!=32)throw std::runtime_error("fixture sighash");
        std::array<uint8_t,32> digest{};std::copy(bytes.begin(),bytes.end(),digest.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,digest,secret,internal))throw std::runtime_error("fixture signature");
        tx.vin[0].witness.emplace_back(signature.begin(),signature.end());return tx;
    }
};
struct ClearAfterCapture final : Mempool::ChainstateReadGuard {
    Mempool& pool;
    explicit ClearAfterCapture(Mempool& p):pool(p){}
    ~ClearAfterCapture() override {pool.clear();}
};
TEST_F(MiningSelectionSnapshot, CapturedMetadataSurvivesPoolClear) {
    Mempool pool(&db,&coins);const auto first=payment(21,1000),second=payment(22,2000);
    ASSERT_TRUE(pool.submitTransaction(first,"fixture",false).accepted());
    ASSERT_TRUE(pool.submitTransaction(second,"fixture",false).accepted());
    const auto entry=pool.getMempoolEntry(first.GetTxid().AsUint256());ASSERT_TRUE(entry);
    auto capture=pool.CaptureBlockSelection(1000000,4000000,1);
    ASSERT_TRUE(capture.available);ASSERT_EQ(capture.transactions.size(),2U);ASSERT_EQ(capture.metadata.size(),2U);
    EXPECT_EQ(capture.pool_size,2U);EXPECT_EQ(capture.pool_bytes,first.GetSize()+second.GetSize());
    pool.clear();EXPECT_EQ(pool.size(),0U);
    EXPECT_EQ(capture.metadata.at(first.GetTxid().AsUint256()).fee,1000U);
    EXPECT_EQ(capture.metadata.at(second.GetTxid().AsUint256()).fee,2000U);
    EXPECT_EQ(capture.metadata.at(first.GetTxid().AsUint256()).vwu,entry->vwu);
    EXPECT_EQ(capture.metadata.at(first.GetTxid().AsUint256()).entered,entry->time);
    for(const auto& tx:capture.transactions)EXPECT_EQ(tx.Serialize(),tx.GetTxid()==first.GetTxid()?first.Serialize():second.Serialize());
}
TEST_F(MiningSelectionSnapshot, JobAndRpcTemplateKeepCapturedFees) {
    Mempool pool(&db,&coins);const auto tx=payment(23,3000);
    ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
    pool.setChainstateReadGuardFactory([&]{return std::make_unique<ClearAfterCapture>(pool);});
    auto job=assembler.CreateJob();ASSERT_NE(job,nullptr);EXPECT_EQ(pool.size(),0U);
    ASSERT_EQ(job->transactions.size(),2U);EXPECT_EQ(job->total_fees,3000U);EXPECT_EQ(job->transactions[1].Serialize(),tx.Serialize());
    EXPECT_EQ(job->transactions[0].vout[0].value.GetUna(),job->block_reward+3000);
    pool.setChainstateReadGuardFactory({});ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    pool.setChainstateReadGuardFactory([&]{return std::make_unique<ClearAfterCapture>(pool);});
    auto block=assembler.CreateNewBlock(address);ASSERT_NE(block,nullptr);EXPECT_EQ(pool.size(),0U);
    ASSERT_EQ(block->vtx.size(),2U);EXPECT_EQ(block->vtx[1].Serialize(),tx.Serialize());
    EXPECT_EQ(assembler.getBlockTemplateStats().total_fees,3000U);EXPECT_EQ(assembler.getBlockTemplateStats().mempool_size,1U);
    EXPECT_EQ(assembler.getBlockTemplateStats().rejected_txs,0U);
    EXPECT_EQ(block->vtx[0].vout[0].value.GetUna(),job->block_reward+3000);
}
TEST_F(MiningSelectionSnapshot, UnavailableChainCaptureRefusesMining) {
    Mempool pool(&db,&coins);BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
    pool.setChainstateReadGuardFactory([]{return std::unique_ptr<Mempool::ChainstateReadGuard>{};});
    EXPECT_FALSE(pool.CaptureBlockSelection(1000000,4000000,1).available);
    EXPECT_THROW(assembler.CreateJob(),std::runtime_error);
    EXPECT_THROW(assembler.CreateNewBlock(address),std::runtime_error);
    pool.setChainstateReadGuardFactory({});EXPECT_TRUE(pool.CaptureBlockSelection(1000000,4000000,1).available);
    EXPECT_NE(assembler.CreateNewBlock(address),nullptr);
}
}
