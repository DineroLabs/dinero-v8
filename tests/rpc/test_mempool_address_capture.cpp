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
#include "daemon/services/chainstate_service.h"
#include "consensus/chain_state_view.h"
#include "rpc/rpc_registry.h"
#include "common/json_adapter.h"
namespace din {
Json rpc_getaddressbalance(const ExecutionContext&, const Json&);
Json rpc_getaddressmempool(const ExecutionContext&, const Json&);
Json rpc_getaddressbatch(const ExecutionContext&, const Json&);
}
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
class MempoolAddressCaptureTestPeer {
public:
    static void Install(Mempool& pool, const MempoolTransaction& body) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        if (!pool.m_transactions.emplace(body.GetTxid().AsUint256(),
                MempoolEntry(body, body.ExplicitFee().value_or(1000), 110)).second)
            throw std::logic_error("duplicate structural reader entry");
    }
    static void SetView(Mempool& pool, std::unique_ptr<consensus::ChainStateView> view) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        pool.chain_state_view_ = std::move(view);
    }
};
}
namespace {
using namespace dinero;
class MempoolAddressCapture : public ::testing::Test {
protected:
    struct SelectedGuard final : Mempool::ChainstateReadGuard {
        std::shared_ptr<ChainstateService> service;
        decltype(std::declval<ChainstateService&>().AcquireBlockIngressActivationLock()) lock;
        explicit SelectedGuard(std::shared_ptr<ChainstateService> owner)
            : service(std::move(owner)), lock(service->AcquireBlockIngressActivationLock()) {}
    };
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    std::shared_ptr<MempoolService> service;
    std::shared_ptr<ChainstateService> chainstate;
    std::string address, other;
    DaemonContext daemon;
    ExecutionContext context;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_address_capture_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        static uint8_t sequence = 80; secret.back()=++sequence;int parity=0;std::array<uint8_t,32> output{};
        ASSERT_TRUE(TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));ASSERT_TRUE(TaprootKeys::ComputeTweakedPubkey(internal,output));
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        address = TaprootKeys::CreateTaprootAddress(output, "din");
        output.back() ^= 1; other = TaprootKeys::CreateTaprootAddress(output, "din");
        ASSERT_FALSE(address.empty()); ASSERT_FALSE(other.empty());
        chainstate = std::make_shared<ChainstateService>(); chainstate->setChainDB(&db);
        daemon.chainstate = chainstate;
        service = MempoolServiceOwnerTestPeer::Published(db, coins);
        service->mempool().setChainstateReadGuardFactory([selected = chainstate] {
            return std::make_unique<SelectedGuard>(selected);
        });
        daemon.mempool = service;
        context.daemon = &daemon;
    }
    void TearDown() override {
        if (service) service->Stop();
        daemon.mempool.reset(); service.reset(); daemon.chainstate.reset(); chainstate.reset(); db.close(); std::filesystem::remove_all(root);
    }
    din::Json Params() const { din::Json p = din::arr(); p.append(address); return p; }
    din::Json Batch() const {
        din::Json p = din::obj(); p["addresses"] = Params(); p["addresses"].append(other);
        p["history_count"] = 1; return din::rpc_getaddressbatch(context, p);
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

TEST_F(MempoolAddressCapture, SignedParentChildAllAddressRpcs) {
    const auto funded = fund(1);
    const auto parent = spend(funded, 1000000, 1000);
    ASSERT_TRUE(service->Submit(parent, TxOrigin::INTERNAL).accepted());
    const auto child = spend({parent.GetTxid(), 0}, 999000, 1000);
    ASSERT_TRUE(service->Submit(child, TxOrigin::INTERNAL).accepted());
    const auto captured = service->mempool().CaptureEntriesWithInputCoins();
    ASSERT_EQ(captured.size(), 2U);
    for (const auto& item : captured) {
        ASSERT_EQ(item.input_coins.size(), 1U);
        EXPECT_EQ(item.input_coins[0].scriptPubKey, script);
        EXPECT_EQ(item.input_coins[0].value.GetUna(),
                  item.entry.tx.GetTxid() == parent.GetTxid() ? 1000000U : 999000U);
    }
    const auto pending = din::rpc_getaddressmempool(context, Params());
    ASSERT_FALSE(pending.isMember("error")) << pending.toStyledString();
    ASSERT_EQ(pending["transactions"].size(), 2U);
    for (const auto& item : pending["transactions"]) {
        EXPECT_EQ(item["type"].asString(), "send"); EXPECT_EQ(item["amount"].asInt64(), 1000);
    }
    const auto balance = din::rpc_getaddressbalance(context, Params());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    EXPECT_EQ(balance["unconfirmed"].asInt64(), -2000);
    const auto batch = Batch();
    ASSERT_FALSE(batch.isMember("error")) << batch.toStyledString();
    EXPECT_EQ(batch["addresses"][address]["unconfirmed"].asInt64(), -2000);
    EXPECT_EQ(batch["addresses"][other]["unconfirmed"].asInt64(), 0);
    service->mempool().clear();
    for (const auto& item : captured) {
        EXPECT_EQ(item.entry.tx.Serialize(), item.entry.tx.GetTxid() == parent.GetTxid()
            ? parent.Serialize(true) : child.Serialize(true));
        EXPECT_EQ(item.input_coins[0].scriptPubKey, script);
    }
    EXPECT_EQ(din::rpc_getaddressmempool(context, Params())["transactions"].size(), 0U);
}
TEST_F(MempoolAddressCapture, MissingInputAndClosedServiceRefuse) {
    const auto funded = fund(2); const auto tx = spend(funded, 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    const auto saved = coins.SpendCoin(funded); ASSERT_TRUE(saved);
    EXPECT_THROW((void)service->mempool().CaptureEntriesWithInputCoins(), std::runtime_error);
    for (const auto& result : {din::rpc_getaddressmempool(context, Params()),
            din::rpc_getaddressbalance(context, Params()), Batch()}) {
        EXPECT_TRUE(result.isMember("error")); EXPECT_FALSE(result.isMember("transactions"));
        EXPECT_FALSE(result.isMember("unconfirmed")); EXPECT_FALSE(result.isMember("addresses"));
    }
    EXPECT_EQ(service->mempool().size(), 1U);
    ASSERT_TRUE(coins.AddCoin(funded, *saved));
    EXPECT_FALSE(din::rpc_getaddressmempool(context, Params()).isMember("error"));
    service->Stop();
    EXPECT_TRUE(din::rpc_getaddressmempool(context, Params()).isMember("error"));
    EXPECT_TRUE(din::rpc_getaddressbalance(context, Params()).isMember("error"));
    daemon.mempool.reset(); // Explicitly absent optional consumer, not a closed service.
    EXPECT_FALSE(din::rpc_getaddressmempool(context, Params()).isMember("error"));
}
TEST_F(MempoolAddressCapture, PrebaseAuthorizationAndSingleBatchCapture) {
    const auto funded = fund(3); const auto tx = spend(funded, 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    auto& pool = service->mempool();
    bool live = false; unsigned resolves = 0;
    pool.setPreBaseCoinPredicate([&](const OutPoint& out) { return out == funded; });
    pool.setPreBaseCoinResolver([&](const OutPoint&) -> std::optional<consensus::UTXOEntry> {
        ++resolves; return live ? std::optional<consensus::UTXOEntry>(*coins.GetCoin(funded)) : std::nullopt;
    });
    // The ordinary auxiliary coin still exists. Only live authorization permits capture.
    EXPECT_THROW((void)pool.CaptureEntriesWithInputCoins(), std::runtime_error);
    live = true; resolves = 0;
    const auto result = Batch();
    ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
    EXPECT_EQ(resolves, 1U); // Both addresses consume the same captured input.
    EXPECT_EQ(result["addresses"][address]["unconfirmed"].asInt64(), -1000);
    pool.setChainstateReadGuardFactory([] { return std::unique_ptr<Mempool::ChainstateReadGuard>{}; });
    EXPECT_THROW((void)pool.CaptureEntriesWithInputCoins(), std::runtime_error);
    EXPECT_EQ(resolves, 1U); // Refusal precedes coin lookup.
}
class UnavailableAddressView final : public consensus::ChainStateView {
public:
    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint&) const override { return Status::Io; }
    bool hasCoin(const OutPoint&) const override { return false; }
    uint32_t getHeight() const override { return 110; }
};
TEST_F(MempoolAddressCapture, ReadFailureAndConfidentialAmountsRefuse) {
    const auto tx = spend(fund(4), 1000000, 1000);
    ASSERT_TRUE(service->Submit(tx, TxOrigin::INTERNAL).accepted());
    unsigned fallback = 0;
    service->mempool().setPreBaseCoinResolver([&](const OutPoint&) -> std::optional<consensus::UTXOEntry> {
        ++fallback; return consensus::UTXOEntry{AmountUna::Una(1000000), script, 1, false};
    });
    MempoolAddressCaptureTestPeer::SetView(service->mempool(), std::make_unique<UnavailableAddressView>());
    EXPECT_THROW((void)service->mempool().CaptureEntriesWithInputCoins(), std::runtime_error);
    EXPECT_EQ(fallback, 0U); // Read errors cannot be masked by a fallback.
    service->mempool().clear();
    Transaction confidential; confidential.version = 2;
    confidential.vout.emplace_back(AmountUna::Una(0), script);
    confidential.vout[0].is_confidential = true;
    confidential.vout[0].commitment.assign(33, 1);
    MempoolAddressCaptureTestPeer::Install(service->mempool(), MempoolTransaction(confidential));
    const auto result = din::rpc_getaddressmempool(context, Params());
    EXPECT_TRUE(result.isMember("error")); EXPECT_FALSE(result.isMember("transactions"));
    // Structural reader fixture only. No confidential transaction admission.
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolAddressCapture, TypedBodyAndMatchingInputsStayOwned) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good()); const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    const auto body = MempoolTransaction::FromOrchard(envelope);
    for (const auto& out : body.Inputs())
        ASSERT_TRUE(coins.AddCoin(out, {AmountUna::Una(1000000), script, 1, false}));
    MempoolAddressCaptureTestPeer::Install(service->mempool(), body);
    const auto captured = service->mempool().CaptureEntriesWithInputCoins();
    ASSERT_EQ(captured.size(), 1U); EXPECT_EQ(captured[0].entry.tx.Serialize(), wire);
    EXPECT_EQ(captured[0].input_coins.size(), body.Inputs().size());
    const auto result = din::rpc_getaddressmempool(context, Params());
    ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
    ASSERT_EQ(result["transactions"].size(), 1U);
    EXPECT_EQ(result["transactions"][0]["txid"].asString(), body.GetTxid().AsUint256().GetHex());
    EXPECT_EQ(result["transactions"][0]["size"].asUInt64(), wire.size());
    service->mempool().clear(); EXPECT_EQ(captured[0].entry.tx.Serialize(), wire);
    for (const auto& coin : captured[0].input_coins) EXPECT_EQ(coin.scriptPubKey, script);
    // Canonical structural Orchard body, synthetic input metadata; no proof/admission claim.
}
#endif
}
