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
namespace {
using namespace dinero;
class MempoolTransactionBody : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_transaction_body_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        secret.back()=67;int parity=0;std::array<uint8_t,32> output{};
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
static_assert(!std::is_convertible_v<Transaction, MempoolTransaction>);
static_assert(!std::is_convertible_v<MempoolTransaction, Transaction>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<MempoolTransaction>().Historical())>>);
TEST_F(MempoolTransactionBody, HistoricalCopyRetainsBodyAndInputs) {
    auto original = spend(fund(1), 1000000, 1000);
    const auto bytes = original.Serialize(TxSerializationMode::WithWitness);
    const auto id = original.GetTxid();
    const auto witness_id = original.GetWtxid();
    const auto input = OutPoint{original.vin[0].prevout.txid, original.vin[0].prevout.vout};
    MempoolTransaction body(original);
    MempoolEntry entry(body, 1000, 110);
    original.vin.clear(); original.vout.clear(); original.lockTime = 999;
    EXPECT_EQ(entry.tx.Serialize(), bytes);
    EXPECT_EQ(entry.tx.GetTxid(), id);
    EXPECT_EQ(entry.tx.GetWtxid(), witness_id);
    EXPECT_EQ(entry.tx_size, bytes.size());
    EXPECT_EQ(entry.spends, std::vector<OutPoint>{input});
    EXPECT_EQ(entry.tx.Inputs(), entry.spends);
    EXPECT_FALSE(entry.tx.IsOrchard());
    auto retained = entry;
    entry = MempoolEntry{};
    EXPECT_FALSE(entry.tx.HasBody());
    EXPECT_EQ(retained.tx.Serialize(), bytes);
    EXPECT_EQ(retained.tx.GetWeight(), retained.tx.Historical().GetWeight());
    EXPECT_EQ(retained.tx.GetVirtualSize(), retained.tx.Historical().GetVirtualSize());
    EXPECT_EQ(retained.tx.GetBaseSize(), retained.tx.Historical().GetBaseSize());
}
TEST_F(MempoolTransactionBody, AdmissionCapturesBeforeChainCallback) {
    const auto first_input = fund(2);
    const auto second_input = fund(3);
    auto supplied = spend(first_input, 1000000, 1000);
    const auto replacement = spend(second_input, 1000000, 2000);
    const auto expected = supplied.Serialize(TxSerializationMode::WithWitness);
    const auto expected_id = supplied.GetTxid().AsUint256();
    Mempool pool(&db, &coins);
    struct Guard final : Mempool::ChainstateReadGuard {};
    unsigned acquisitions = 0, notifications = 0;
    pool.setChainstateReadGuardFactory([&] {
        ++acquisitions;
        // Same-thread callback, after the input copy: no concurrent mutation.
        supplied = replacement;
        return std::make_unique<Guard>();
    });
    pool.setTxAcceptedCallback([&](const Transaction& tx) {
        ++notifications;
        EXPECT_EQ(tx.Serialize(TxSerializationMode::WithWitness), expected);
        EXPECT_EQ(tx.GetTxid().AsUint256(), expected_id);
    });
    ASSERT_TRUE(pool.submitTransactionTestOnly(supplied, "fixture").accepted());
    EXPECT_EQ(pool.size(), 0U);
    EXPECT_EQ(notifications, 0U);
    supplied = spend(first_input, 1000000, 1000);
    // Signatures may use fresh auxiliary randomness; capture this exact body.
    const auto admitted_bytes = supplied.Serialize(TxSerializationMode::WithWitness);
    pool.setTxAcceptedCallback([&](const Transaction& tx) {
        ++notifications;
        EXPECT_EQ(tx.Serialize(TxSerializationMode::WithWitness), admitted_bytes);
    });
    const auto result = pool.submitTransaction(supplied, "fixture", false);
    ASSERT_TRUE(result.accepted()) << result.message;
    EXPECT_EQ(acquisitions, 2U);
    EXPECT_EQ(notifications, 1U);
    EXPECT_EQ(pool.getInputSpenders(first_input), std::vector<uint256>{expected_id});
    EXPECT_TRUE(pool.getInputSpenders(second_input).empty());
    const auto retained = pool.getMempoolEntry(expected_id);
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->tx.Serialize(), admitted_bytes);
    EXPECT_EQ(retained->fee, 1000U);
    ASSERT_TRUE(pool.removeTransaction(expected_id));
    EXPECT_EQ(retained->tx.Serialize(), admitted_bytes);
}
TEST_F(MempoolTransactionBody, EmptyAndWrongFamilyAccessRefuse) {
    MempoolTransaction empty;
    EXPECT_FALSE(empty.HasBody());
    EXPECT_THROW((void)empty.Historical(), std::logic_error);
    EXPECT_THROW((void)empty.Orchard(), std::logic_error);
    EXPECT_THROW((void)empty.GetTxid(), std::logic_error);
    EXPECT_THROW((void)empty.Serialize(), std::logic_error);
    EXPECT_THROW((void)MempoolEntry(empty, 0, 0), std::logic_error);
    MempoolTransaction historical(spend(fund(4), 1000000, 1000));
    EXPECT_THROW((void)historical.Orchard(), std::logic_error);
    EXPECT_THROW((void)historical.Serialize(static_cast<TxSerializationMode>(9)), std::invalid_argument);
    auto moved = std::move(historical);
    EXPECT_FALSE(historical.HasBody());
    EXPECT_TRUE(moved.HasBody());
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolTransactionBody, OrchardEntryOwnsCanonicalBodyWithoutLegacyConversion) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    auto body = MempoolTransaction::FromOrchard(envelope);
    ASSERT_TRUE(body.HasBody());
    ASSERT_TRUE(body.IsOrchard());
    EXPECT_THROW((void)body.Historical(), std::logic_error);
    EXPECT_EQ(body.Orchard().CanonicalBytes(), wire);
    EXPECT_EQ(body.Serialize(), wire);
    EXPECT_EQ(body.Serialize(TxSerializationMode::WithoutWitness), envelope.TxidPreimage());
    EXPECT_EQ(body.GetWeight(), 3 * body.GetBaseSize() + body.GetSize());
    EXPECT_EQ(body.GetVirtualSize(), (body.GetWeight() + 3) / 4);
    EXPECT_EQ(body.ExplicitFee(), std::optional<uint64_t>(envelope.ExplicitFee()));
    uint256 txid, wtxid;
    const auto expected_txid = envelope.Txid(), expected_wtxid = envelope.Wtxid();
    std::copy(expected_txid.begin(), expected_txid.end(), txid.begin());
    std::copy(expected_wtxid.begin(), expected_wtxid.end(), wtxid.begin());
    EXPECT_EQ(body.GetTxid(), TxId(txid)); EXPECT_EQ(body.GetWtxid(), WTxId(wtxid));
    MempoolEntry entry(body, envelope.ExplicitFee(), 111);
    EXPECT_EQ(entry.tx_size, wire.size());
    EXPECT_EQ(entry.fee, envelope.ExplicitFee());
    EXPECT_EQ(entry.spends.size(), envelope.Inputs().size());
    for (size_t i = 0; i < entry.spends.size(); ++i) {
        uint256 hash;
        std::copy(envelope.Inputs()[i].txid_wire.begin(), envelope.Inputs()[i].txid_wire.end(), hash.begin());
        EXPECT_EQ(entry.spends[i], (OutPoint{TxId(hash), envelope.Inputs()[i].output_index}));
    }
    EXPECT_THROW((void)MempoolEntry(body, envelope.ExplicitFee() ^ 1, 111), std::invalid_argument);
    body = MempoolTransaction{};
    EXPECT_EQ(entry.tx.Serialize(), wire);
    EXPECT_THROW((void)entry.tx.Historical(), std::logic_error);
    // Constructing a typed entry is not admission; no pool insertion API is invoked.
}
#endif
}
