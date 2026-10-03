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
din::Json rpc_context_mempool_gettransaction(const ExecutionContext&, const din::Json&);
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
class MempoolRawRpcTestPeer {
public:
    // Reader-only synthetic state: no admission, proof, indexes or relay claim.
    static void InstallReaderEntry(Mempool& pool, MempoolEntry entry) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        const auto id = entry.tx.GetTxid().AsUint256();
        if (!pool.m_transactions.empty()) throw std::logic_error("reader fixture requires empty pool");
        pool.m_transactions.emplace(id, std::move(entry));
    }
};
}
namespace {
using namespace dinero;
class MempoolRawRpc : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    std::shared_ptr<MempoolService> service;
    DaemonContext daemon;
    ExecutionContext context;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_raw_rpc_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    din::Json Request(const std::string& id) {
        din::Json params = din::arr(); params.append(id);
        return rpc_context_mempool_gettransaction(context, params);
    }
    void Check(const din::Json& result, const MempoolTransaction& body) {
        ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
        EXPECT_EQ(result["txid"].asString(), body.GetTxid().AsUint256().GetHex());
        EXPECT_EQ(result["hex"].asString(), dinero::jj::toHex(body.Serialize()));
        EXPECT_EQ(result["size"].asUInt64(), body.GetSize());
        EXPECT_EQ(result["vsize"].asUInt64(), body.GetVirtualSize());
        EXPECT_EQ(result["weight"].asUInt64(), body.GetWeight());
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
TEST_F(MempoolRawRpc, SignedHistoricalBytesAndWeight) {
    const auto tx = spend(fund(1), 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    const MempoolTransaction body(tx);
    ASSERT_NE(body.GetSize(), body.GetVirtualSize());
    ASSERT_NE(body.GetWeight(), 4 * body.GetSize());
    const auto result = Request(tx.GetTxid().AsUint256().GetHex());
    Check(result, body);
    service->mempool().clear();
    Check(result, body);
    const auto missing = Request(tx.GetTxid().AsUint256().GetHex());
    EXPECT_TRUE(missing.isMember("error"));
    EXPECT_FALSE(missing.isMember("hex"));
    EXPECT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    Check(Request(tx.GetTxid().AsUint256().GetHex()), body);
}
TEST_F(MempoolRawRpc, InvalidAbsentAndClosedServiceRefuse) {
    const auto tx = spend(fund(2), 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    for (const auto& id : std::vector<std::string>{"", "0", std::string(63, '0'), std::string(65, '0'), std::string(64, 'g'), "0x" + std::string(62, '0'), std::string(64, '0')}) {
        const auto result = Request(id);
        EXPECT_TRUE(result.isMember("error"));
        EXPECT_FALSE(result.isMember("hex"));
    }
    din::Json object = din::obj(); object["txid"] = tx.GetTxid().AsUint256().GetHex();
    const auto malformed = rpc_context_mempool_gettransaction(context, object);
    EXPECT_TRUE(malformed.isMember("error"));
    EXPECT_FALSE(malformed.isMember("hex"));
    EXPECT_EQ(service->mempool().size(), 1U);
    service->Stop();
    const auto closed = Request(tx.GetTxid().AsUint256().GetHex());
    EXPECT_TRUE(closed.isMember("error"));
    EXPECT_FALSE(closed.isMember("hex"));
    daemon.mempool.reset();
    EXPECT_TRUE(Request(tx.GetTxid().AsUint256().GetHex()).isMember("error"));
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolRawRpc, OrchardReaderRetainsExactCanonicalBytes) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    auto body = MempoolTransaction::FromOrchard(envelope);
    const auto id = body.GetTxid().AsUint256().GetHex();
    MempoolRawRpcTestPeer::InstallReaderEntry(service->mempool(), MempoolEntry(body, envelope.ExplicitFee(), 110));
    const auto result = Request(id);
    Check(result, body);
    EXPECT_EQ(result["hex"].asString(), dinero::jj::toHex(wire));
    EXPECT_THROW((void)body.Historical(), std::logic_error);
    service->mempool().clear();
    Check(result, body);
    EXPECT_FALSE(Request(id).isMember("hex"));
    // Actual RPC reader, synthetic captured entry: not Orchard admission.
}
#endif
}
