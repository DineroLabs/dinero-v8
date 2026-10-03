// Benign patched-path policy invariants. No unsafe-original execution.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "daemon/tx_relay_manager.h"
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
class MempoolTypedRelay : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_typed_relay_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
TEST_F(MempoolTypedRelay, CapturedBodyAndSendSurviveReplacement) {
    const auto tx = spend(fund(1), 1000000, 1000);
    const auto id = tx.GetTxid().AsUint256();
    Mempool pool(&db, &coins);
    ASSERT_TRUE(pool.submitTransaction(tx, "fixture", false).accepted());
    TxRelayManager relay(nullptr);
    size_t original = 0, replacement = 0;
    std::vector<uint8_t> payload;
    relay.SetSendMessageCallback([&](const auto& peer, const auto& command, const auto& bytes) {
        EXPECT_EQ(peer, "fixture-peer"); EXPECT_EQ(command, "tx");
        ++original; payload = bytes;
    });
    relay.SetRetrieveBodyCallback([&](const uint256& requested) -> std::optional<MempoolTransaction> {
        const auto captured = pool.getMempoolEntry(requested);
        pool.clear();
        relay.SetRetrieveBodyCallback({});
        relay.SetSendMessageCallback([&](const auto&, const auto&, const auto&) { ++replacement; });
        if (!captured) return std::nullopt;
        return captured->tx;
    });
    relay.HandleGetData("fixture-peer", id);
    EXPECT_EQ(original, 1U); EXPECT_EQ(replacement, 0U);
    EXPECT_EQ(payload, tx.Serialize()); EXPECT_EQ(pool.size(), 0U);
    relay.HandleGetData("fixture-peer", id);
    EXPECT_EQ(original, 1U); EXPECT_EQ(replacement, 0U);
}
TEST_F(MempoolTypedRelay, MissingEmptyAndWrongIdentityDoNotSend) {
    const auto tx = spend(fund(2), 1000000, 1000);
    const auto id = tx.GetTxid().AsUint256();
    TxRelayManager relay(nullptr); size_t sent = 0;
    relay.SetSendMessageCallback([&](const auto&, const auto&, const auto&) { ++sent; });
    relay.SetRetrieveBodyCallback([](const auto&) -> std::optional<MempoolTransaction> { return std::nullopt; });
    relay.HandleGetData("fixture-peer", id);
    relay.SetRetrieveBodyCallback([](const auto&) -> std::optional<MempoolTransaction> { return MempoolTransaction{}; });
    relay.HandleGetData("fixture-peer", id);
    const auto other = spend(fund(3), 1000000, 1000);
    relay.SetRetrieveBodyCallback([&](const auto&) -> std::optional<MempoolTransaction> { return MempoolTransaction(other); });
    relay.HandleGetData("fixture-peer", id);
    EXPECT_EQ(sent, 0U); EXPECT_FALSE(relay.IsTxSeen(id));
    relay.SetRetrieveBodyCallback([&](const auto&) -> std::optional<MempoolTransaction> { return MempoolTransaction(tx); });
    relay.HandleGetData("fixture-peer", id); EXPECT_EQ(sent, 1U);
}
TEST_F(MempoolTypedRelay, HistoricalAdapterUsesSameCallbackOwner) {
    const auto tx = spend(fund(4), 1000000, 1000);
    const auto id = tx.GetTxid().AsUint256();
    TxRelayManager relay(nullptr); size_t typed = 0, historical = 0, sent = 0;
    relay.SetSendMessageCallback([&](const auto&, const auto&, const auto& bytes) { ++sent; EXPECT_EQ(bytes, tx.Serialize()); });
    relay.SetRetrieveBodyCallback([&](const auto&) -> std::optional<MempoolTransaction> { ++typed; return MempoolTransaction(tx); });
    relay.SetRetrieveTxCallback([&](const auto& requested, Transaction& output) { ++historical; EXPECT_EQ(requested,id); output=tx; return true; });
    relay.HandleGetData("fixture-peer", id);
    EXPECT_EQ(typed, 0U); EXPECT_EQ(historical, 1U); EXPECT_EQ(sent, 1U);
    relay.SetRetrieveBodyCallback([&](const auto&) -> std::optional<MempoolTransaction> { ++typed; return MempoolTransaction(tx); });
    relay.HandleGetData("fixture-peer", id);
    EXPECT_EQ(typed, 1U); EXPECT_EQ(historical, 1U); EXPECT_EQ(sent, 2U);
    relay.SetRetrieveTxCallback({}); relay.HandleGetData("fixture-peer", id);
    EXPECT_EQ(sent, 2U);
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolTypedRelay, OrchardCanonicalWireWithoutHistoricalConversion) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    auto body = MempoolTransaction::FromOrchard(envelope);
    const auto id = body.GetTxid().AsUint256();
    TxRelayManager relay(nullptr); size_t sent = 0;
    relay.SetSendMessageCallback([&](const auto& peer, const auto& command, const auto& bytes) {
        EXPECT_EQ(peer,"fixture-peer"); EXPECT_EQ(command,"tx"); EXPECT_EQ(bytes,wire); ++sent;
    });
    relay.SetRetrieveBodyCallback([body](const auto&) -> std::optional<MempoolTransaction> { return body; });
    body = MempoolTransaction{};
    relay.HandleGetData("fixture-peer", id); EXPECT_EQ(sent,1U);
    // Real serialization/relay callback; fixture body is not admitted/proved.
}
#endif
}
