// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
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
#include "daemon/daemon_context.h"
#include "daemon/services/mempool_service.h"
#include "mining/block_assembler.h"
#include "rpc/rpc_registry.h"
#include "common/json_adapter.h"
din::Json rpc_context_mempool_getbyfee(const ExecutionContext&, const din::Json&);
namespace dinero {
class MempoolServiceOwnerTestPeer {
public:
    static std::shared_ptr<MempoolService> Published(ChainDB& db, consensus::ConsensusUTXOSet& coins) {
        auto service = std::make_shared<MempoolService>();
        service->mempool_ = std::make_unique<Mempool>(&db, &coins);
        service->accepting_ = true;
        service->started_ = true;
        return service;
    }
};
class MempoolRankedRpcTestPeer {
public:
    // Reader-only synthetic index state; never admission or proof validation.
    static void InstallRankedEntries(Mempool& pool, std::vector<std::pair<double, MempoolEntry>> entries) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        if (!pool.m_transactions.empty() || !pool.m_fee_index.empty())
            throw std::logic_error("reader fixture requires empty pool");
        for (auto& [score, entry] : entries) {
            const auto id = entry.tx.GetTxid().AsUint256();
            if (!pool.m_transactions.emplace(id, std::move(entry)).second)
                throw std::logic_error("duplicate fixture entry");
            pool.m_fee_index.emplace(score, id);
        }
    }
    static void AddMissingIndexEntry(Mempool& pool) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        uint256 missing; missing.data[0] = 239;
        if (pool.m_transactions.count(missing)) throw std::logic_error("fixture identity collision");
        pool.m_fee_index.emplace(-1.0, missing);
    }
};
}
namespace {
using namespace dinero;
class MempoolRankedRpc : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    std::shared_ptr<MempoolService> service;
    DaemonContext daemon;
    ExecutionContext context;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_ranked_rpc_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        secret.back()=69;int parity=0;std::array<uint8_t,32> output{};
        ASSERT_TRUE(TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));ASSERT_TRUE(TaprootKeys::ComputeTweakedPubkey(internal,output));
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        service = MempoolServiceOwnerTestPeer::Published(db, coins);
        daemon.mempool = service;
        context.daemon = &daemon;
    }
    void TearDown() override {
        if (service) service->Stop();
        daemon.mempool.reset(); service.reset(); db.close(); std::filesystem::remove_all(root);
    }
    din::Json Request(int count = 100) {
        din::Json params = din::arr(); params.append(count);
        return rpc_context_mempool_getbyfee(context, params);
    }
    void CheckEntry(const din::Json& result, const MempoolEntry& entry) {
        EXPECT_EQ(result["txid"].asString(), entry.tx.GetTxid().AsUint256().GetHex());
        EXPECT_EQ(result["size"].asUInt64(), entry.tx.GetSize());
        EXPECT_DOUBLE_EQ(result["fee"].asDouble(), static_cast<double>(entry.fee) / 100000000.0);
        EXPECT_DOUBLE_EQ(result["feerate"].asDouble(), entry.fee_rate);
    }
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
TEST_F(MempoolRankedRpc, SignedRankedCaptureSurvivesRemoval) {
    const auto low = spend(fund(1), 1000000, 1000);
    const auto high = spend(fund(2), 1000000, 3000);
    const auto middle = spend(fund(3), 1000000, 2000);
    for (const auto& tx : {low, high, middle})
        ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    const auto captured = service->mempool().CaptureEntriesByFeeRate(2);
    ASSERT_EQ(captured.size(), 2U);
    EXPECT_EQ(captured[0].tx.GetTxid(), high.GetTxid());
    EXPECT_EQ(captured[1].tx.GetTxid(), middle.GetTxid());
    EXPECT_TRUE(service->mempool().CaptureEntriesByFeeRate(0).empty());
    const auto legacy = service->mempool().getTransactionsByFeeRate(2);
    ASSERT_EQ(legacy.size(), 2U);
    EXPECT_EQ(legacy[0].Serialize(true), high.Serialize(true));
    const auto result = Request(2);
    ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
    ASSERT_EQ(result["transactions"].size(), 2U);
    EXPECT_EQ(result["count"].asUInt64(), 2U);
    for (unsigned i = 0; i < 2; ++i) CheckEntry(result["transactions"][i], captured[i]);
    service->mempool().clear();
    EXPECT_EQ(captured[0].tx.Serialize(), high.Serialize(true));
    EXPECT_EQ(captured[0].fee, 3000U);
    for (unsigned i = 0; i < 2; ++i) CheckEntry(result["transactions"][i], captured[i]);
    const auto empty = Request();
    ASSERT_FALSE(empty.isMember("error"));
    EXPECT_EQ(empty["count"].asUInt64(), 0U);
}
TEST_F(MempoolRankedRpc, IndexedReadRefusesPartialResult) {
    const auto tx = spend(fund(4), 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    MempoolRankedRpcTestPeer::AddMissingIndexEntry(service->mempool());
    // The requested one-entry prefix is complete; the longer read must refuse.
    EXPECT_EQ(service->mempool().CaptureEntriesByFeeRate(1).size(), 1U);
    EXPECT_THROW((void)service->mempool().CaptureEntriesByFeeRate(2), std::runtime_error);
    EXPECT_THROW((void)service->mempool().getTransactionsByFeeRate(2), std::runtime_error);
    const auto result = Request(2);
    EXPECT_TRUE(result.isMember("error"));
    EXPECT_FALSE(result.isMember("transactions"));
    EXPECT_FALSE(result.isMember("count"));
    EXPECT_EQ(service->mempool().size(), 1U);
    EXPECT_TRUE(service->mempool().hasTransaction(tx.GetTxid().AsUint256()));
}
TEST_F(MempoolRankedRpc, ClosedServiceRefusesCapture) {
    const auto tx = spend(fund(5), 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    ASSERT_FALSE(Request().isMember("error"));
    service->Stop();
    const auto stopped = Request();
    EXPECT_TRUE(stopped.isMember("error"));
    EXPECT_FALSE(stopped.isMember("transactions"));
    daemon.mempool.reset();
    EXPECT_TRUE(Request().isMember("error"));
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolRankedRpc, CanonicalOrchardRankedReader) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    auto body = MempoolTransaction::FromOrchard(envelope);
    const auto historical = spend(fund(6), 1000000, 1000);
    MempoolEntry first(body, envelope.ExplicitFee(), 110);
    MempoolEntry second(historical, 1000, 110);
    ASSERT_NE(first.fee_rate, 100.0);
    MempoolRankedRpcTestPeer::InstallRankedEntries(service->mempool(), {{100.0, first}, {10.0, second}});
    const auto captured = service->mempool().CaptureEntriesByFeeRate(2);
    ASSERT_EQ(captured.size(), 2U);
    ASSERT_TRUE(captured[0].tx.IsOrchard());
    EXPECT_EQ(captured[0].tx.Serialize(), wire);
    EXPECT_EQ(captured[1].tx.Serialize(), historical.Serialize(true));
    const auto result = Request(2);
    ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
    ASSERT_EQ(result["transactions"].size(), 2U);
    for (unsigned i = 0; i < 2; ++i) CheckEntry(result["transactions"][i], captured[i]);
    EXPECT_THROW((void)service->mempool().getTransactionsByFeeRate(2), std::logic_error);
    service->mempool().clear();
    EXPECT_EQ(captured[0].tx.Serialize(), wire);
    for (unsigned i = 0; i < 2; ++i) CheckEntry(result["transactions"][i], captured[i]);
    // Actual RPC, structural mixed index only; no Orchard admission or proof claim.
}
#endif
}
