// Structural conflict fixtures only. These bodies are not admitted, mined or
// claimed to have passed proof/signature checks for this synthetic context.
#include "common/ilogger.h"
#include <algorithm>
#include <set>
#ifdef DINERO_TEST_ORCHARD_BODY
#include "daemon/orchard_connected_block_effects.h"
#include "consensus/merkle_root.h"
#endif
namespace dinero {
class MempoolOrchardConflictTestPeer {
public:
    static void Install(Mempool& pool, const std::vector<MempoolTransaction>& bodies) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        if (!pool.m_transactions.empty()) throw std::logic_error("conflict fixture requires empty pool");
        for (const auto& body : bodies) {
            if (!pool.m_transactions.emplace(body.GetTxid().AsUint256(),
                    MempoolEntry(body, body.ExplicitFee().value_or(1000), 110)).second)
                throw std::logic_error("duplicate structural conflict identity");
        }
        for (auto& [id, entry] : pool.m_transactions) {
            std::set<uint256> parents;
            for (const auto& input : entry.tx.Inputs()) {
                pool.m_spent_outputs[input].insert(id);
                if (pool.m_transactions.count(input.txid.AsUint256())) parents.insert(input.txid.AsUint256());
            }
            entry.depends.assign(parents.begin(), parents.end());
            for (const auto& parent : parents) pool.m_children_index[parent].insert(id);
        }
        for (auto& [id, entry] : pool.m_transactions) {
            pool.recalcAncestorMetrics(entry);
            pool.m_fee_index.insert({entry.ancestor_adjusted_feerate, id});
            pool.m_time_index.insert({entry.time, id});
        }
        pool.rebuildCoinsViewLocked();
        pool.m_total_tx_added.store(bodies.size());
    }
};
}
namespace {
using namespace dinero;
class OrchardPoolConflicts : public ::testing::Test {
protected:
    ChainDB db;
    consensus::ConsensusUTXOSet coins;
    std::filesystem::path root;
    void SetUp() override {
        SelectParams(Chain::MAINNET);
        root=std::filesystem::temp_directory_path()/("orchard_pool_conflicts_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);
        coins.SetBestBlock(uint256{},110);
    }
    void TearDown() override { db.close();std::filesystem::remove_all(root); }
    static Transaction ordinary(const std::vector<OutPoint>& inputs, uint32_t n) {
        Transaction tx;tx.version=2;tx.lockTime=n;
        for(const auto& out:inputs) {tx.vin.emplace_back();tx.vin.back().prevout.txid=out.txid;tx.vin.back().prevout.vout=out.vout;}
        tx.vout.emplace_back(AmountUna::Una(1000),std::vector<uint8_t>{0x51});return tx;
    }
    static std::vector<uint256> ids(const Mempool& pool) {
        auto result=pool.getTransactionIds();std::sort(result.begin(),result.end());return result;
    }
#ifdef DINERO_TEST_ORCHARD_BODY
    static std::vector<uint8_t> load(const char* name) {
        std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES)/name,std::ios::binary);
        if(!file.good())throw std::runtime_error("missing conflict fixture");
        return {std::istreambuf_iterator<char>(file),{}};
    }
    static orchard::TransactionEnvelope envelope() {
        return orchard::TransactionEnvelope::DecodeExact(load("candidate-envelope.bin"));
    }
    static OrchardBlockCandidate block(const orchard::TransactionEnvelope& tx) {
        auto cb=ordinary({},0);cb.vin.emplace_back();cb.vin[0].prevout.vout=UINT32_MAX;cb.vin[0].scriptSig={1,1};
        const auto body=MempoolTransaction::FromOrchard(tx);
        const std::vector<TxId> transaction_ids{cb.GetTxid(),body.GetTxid()};
        BlockHeader header{};header.merkle_root=consensus::ComputeTransactionMerkleRoot(transaction_ids);
        auto bytes=header.SerializeForHash();std::vector<uint8_t> wire(bytes.begin(),bytes.end());wire.push_back(2);
        const auto old=cb.Serialize();wire.insert(wire.end(),old.begin(),old.end());
        wire.insert(wire.end(),tx.CanonicalBytes().begin(),tx.CanonicalBytes().end());wire.push_back(0);
        return OrchardBlockCandidate::DecodeExact(wire);
    }
    static ConnectedBlockEffects conflictEffects() {
        const auto tx=envelope();auto inputs=tx.Inputs();
        for(auto& input:inputs)input.txid_wire[0]^=0x80;
        const auto other=orchard::TransactionEnvelope::Create(tx.LockTime()+1,inputs,tx.Outputs(),tx.ExplicitFee(),load("candidate-spend.bundle"));
        return BuildOrchardConnectedBlockEffects(block(other));
    }
    struct Logger final : ILogger {
        bool fail=false;
        void info(const std::string& message) override {if(fail && message.find("Block 111 connected:")!=std::string::npos)throw std::runtime_error("fixture publication preparation failure");}
        void setLogLevel(LogLevel) override {}
        void setLogFile(const std::string&) override {}
        void shutdown() override {}
        void log(LogLevel,const std::string& message) override {info(message);}
        void warning(const std::string&) override {}
        void error(const std::string&) override {}
        void debug(const std::string&) override {}
    };
#endif
};
TEST_F(OrchardPoolConflicts, HistoricalBodiesHaveNoOrchardNullifiers) {
    const auto tx=ordinary({},17);const MempoolTransaction body(tx);
    EXPECT_TRUE(body.OrchardNullifiers().empty());
    EXPECT_THROW(MempoolTransaction{}.OrchardNullifiers(),std::logic_error);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body});
    ConnectedBlockEffects effects;effects.orchard_nullifiers.push_back({});
    EXPECT_EQ(pool.onBlockConnected(effects,111),0U);EXPECT_EQ(ids(pool),std::vector<uint256>{body.GetTxid().AsUint256()});
    EXPECT_EQ(pool.getMempoolEntry(body.GetTxid().AsUint256())->tx.Serialize(),body.Serialize());
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(OrchardPoolConflicts, AllActionIdentitiesSurviveBodyCopyAndBlockExtraction) {
    for(const auto& tx:std::vector<orchard::TransactionEnvelope>{envelope(),
            orchard::TransactionEnvelope::Create(0,{},{{1000,{0x51}}},100,load("combined-shield.bundle"))}) {
        auto body=MempoolTransaction::FromOrchard(tx);const auto retained=body;body=MempoolTransaction{};
        const auto& facts=tx.UnverifiedFacts();ASSERT_GT(facts.action_count,0U);
        ASSERT_EQ(retained.OrchardNullifiers().size(),facts.action_count);
        const auto candidate=block(tx);const auto effects=BuildOrchardConnectedBlockEffects(candidate);
        EXPECT_EQ(effects.orchard_nullifiers,retained.OrchardNullifiers());
        for(size_t i=0;i<facts.action_count;++i)
            EXPECT_TRUE(std::equal(retained.OrchardNullifiers()[i].begin(),retained.OrchardNullifiers()[i].end(),facts.nullifiers[i]));
        EXPECT_EQ(retained.Serialize(),tx.CanonicalBytes());
    }
    const auto shield=orchard::TransactionEnvelope::Create(0,{},{{1000,{0x51}}},100,load("combined-shield.bundle"));
    EXPECT_EQ(shield.UnverifiedFacts().flags & 1,0); // Disabled spends still have action nullifiers.
}
TEST_F(OrchardPoolConflicts, ConflictAndDescendantsAbandonThenPublish) {
    const auto body=MempoolTransaction::FromOrchard(envelope());ASSERT_GT(body.OutputCount(),0U);
    const auto child=ordinary({{body.GetTxid(),0}},21),grandchild=ordinary({{child.GetTxid(),0}},22),survivor=ordinary({},23);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body,MempoolTransaction(child),MempoolTransaction(grandchild),MempoolTransaction(survivor)});
    const auto before=ids(pool);const auto fees=pool.getTotalFees();const auto input=body.Inputs().at(0);
    auto effects=conflictEffects();ASSERT_FALSE(effects.orchard_nullifiers.empty());
    effects.orchard_nullifiers.push_back(effects.orchard_nullifiers.front()); // A repeated identity removes one branch once.
    ASSERT_EQ(std::count(effects.spent_transparent_inputs.begin(),effects.spent_transparent_inputs.end(),input),0);
    {auto prepared=pool.prepareBlockConnected(effects,111,{2});ASSERT_TRUE(prepared);EXPECT_EQ(prepared->EvictedCount(),3U);}
    EXPECT_EQ(ids(pool),before);EXPECT_EQ(pool.getTotalFees(),fees);EXPECT_EQ(pool.getStats().last_connected_height,0U);
    EXPECT_TRUE(pool.isOutputSpentInMempool(input));EXPECT_EQ(pool.getCoinsView().getCoin({grandchild.GetTxid(),0}).status(),Status::Ok);
    EXPECT_EQ(pool.getMempoolEntry(body.GetTxid().AsUint256())->tx.Serialize(),body.Serialize());
    {auto prepared=pool.prepareBlockConnected(effects,111,{2});EXPECT_EQ(prepared->EvictedCount(),3U);prepared->PublishAfterCommit();}
    EXPECT_EQ(ids(pool),std::vector<uint256>{survivor.GetTxid().AsUint256()});EXPECT_FALSE(pool.isOutputSpentInMempool(input));
    EXPECT_EQ(pool.getCoinsView().getCoin({grandchild.GetTxid(),0}).status(),Status::NotFound);
    EXPECT_EQ(pool.getCoinsView().getCoin({survivor.GetTxid(),0}).status(),Status::Ok);
    EXPECT_EQ(pool.onBlockConnected(effects,111),0U);
}
TEST_F(OrchardPoolConflicts, ConfirmationRetainsTransparentChildAndExactBody) {
    const auto body=MempoolTransaction::FromOrchard(envelope());const auto child=ordinary({{body.GetTxid(),0}},24);
    Mempool pool(&db,&coins);MempoolOrchardConflictTestPeer::Install(pool,{body,MempoolTransaction(child)});
    const auto effects=BuildOrchardConnectedBlockEffects(block(body.Orchard()));
    {auto prepared=pool.prepareBlockConnected(effects,111);EXPECT_EQ(prepared->EvictedCount(),0U);prepared->PublishAfterCommit();}
    EXPECT_EQ(ids(pool),std::vector<uint256>{child.GetTxid().AsUint256()});
    const auto entry=pool.getMempoolEntry(child.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_TRUE(entry->depends.empty());EXPECT_EQ(entry->tx.Serialize(),child.Serialize());
    EXPECT_TRUE(pool.isOutputSpentInMempool({body.GetTxid(),0}));
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
}
TEST_F(OrchardPoolConflicts, PreparationFailurePreservesEntriesIndexesAndRetry) {
    const auto body=MempoolTransaction::FromOrchard(envelope());const auto child=ordinary({{body.GetTxid(),0}},25);
    Logger logger;Mempool pool(&db,&coins);pool.setLogger(&logger);
    MempoolOrchardConflictTestPeer::Install(pool,{body,MempoolTransaction(child)});
    const auto before=ids(pool);const auto fees=pool.getTotalFees();const auto effects=conflictEffects();
    logger.fail=true;EXPECT_THROW(pool.prepareBlockConnected(effects,111),std::runtime_error);logger.fail=false;
    EXPECT_EQ(ids(pool),before);EXPECT_EQ(pool.getTotalFees(),fees);EXPECT_EQ(pool.getStats().last_connected_height,0U);
    EXPECT_EQ(pool.getMempoolEntry(body.GetTxid().AsUint256())->tx.Serialize(),body.Serialize());
    EXPECT_TRUE(pool.isOutputSpentInMempool(body.Inputs().at(0)));
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::Ok);
    EXPECT_EQ(pool.onBlockConnected(effects,111),2U);EXPECT_TRUE(pool.getTransactionIds().empty());
}
#endif
}
