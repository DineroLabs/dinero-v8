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
#include "mempool/mempool_persistence.h"
#include <fstream>
#include <iterator>
namespace {
using namespace dinero;
class MempoolRetainedLoad : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    std::unique_ptr<Mempool> pool;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_retained_load_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chaindb"),Status::Ok);coins.SetBestBlock(uint256{},110);
        ASSERT_EQ(db.setTip(ChainWriteToken::CreateForTesting(),uint256{},110,arith_uint256(110)),Status::Ok);
        secret.back()=69;int parity=0;std::array<uint8_t,32> output{};
        ASSERT_TRUE(TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));ASSERT_TRUE(TaprootKeys::ComputeTweakedPubkey(internal,output));
        script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        pool = std::make_unique<Mempool>(&db, &coins);
    }
    void TearDown() override {
        pool.reset(); db.close(); std::filesystem::remove_all(root);
    }
    std::vector<uint8_t> Read(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) throw std::runtime_error("fixture file unavailable");
        return {std::istreambuf_iterator<char>(f), {}};
    }
    void Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        f.close();
        if (!f.good()) throw std::runtime_error("fixture file write failed");
    }
    MempoolEntry Admit(const Transaction& tx) {
        const auto result = pool->submitTransaction(tx, "retained-load-fixture", false);
        if (!result.accepted()) throw std::runtime_error(result.message);
        return *pool->getMempoolEntry(tx.GetTxid().AsUint256());
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
TEST_F(MempoolRetainedLoad, SignedDependencyOrderAndReopen) {
    const auto path = root / "mempool.dat";
    const auto parent = spend(fund(1), 1000000, 1000);
    const auto child = spend({parent.GetTxid(), 0}, 999000, 1000);
    const auto parent_entry = Admit(parent);
    const auto child_entry = Admit(child);
    ASSERT_TRUE(MempoolPersistence::save({child_entry, parent_entry}, path.string()));
    pool->clear();
    unsigned observed = 0;
    pool->setTxAcceptedCallback([&](const Transaction&) {
        ++observed;
        EXPECT_FALSE(pool->saveToDisk(path.string()));
        EXPECT_FALSE(pool->loadFromDisk(path.string()));
    });
    ASSERT_TRUE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(observed, 2U);
    EXPECT_EQ(pool->size(), 2U);
    EXPECT_TRUE(pool->hasTransaction(parent.GetTxid().AsUint256()));
    EXPECT_TRUE(pool->hasTransaction(child.GetTxid().AsUint256()));
    ASSERT_TRUE(pool->saveToDisk(path.string()));
    pool->setTxAcceptedCallback({});
    pool.reset();
    pool = std::make_unique<Mempool>(&db, &coins);
    ASSERT_TRUE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(pool->size(), 2U);
    EXPECT_EQ(pool->getMempoolEntry(child.GetTxid().AsUint256())->tx.Serialize(), child.Serialize(true));
}
TEST_F(MempoolRetainedLoad, UnavailableCoinsRetainFileUntilRetry) {
    const auto path = root / "mempool.dat";
    const auto first = spend(fund(2), 1000000, 1000);
    const auto second_coin = fund(3);
    const auto second = spend(second_coin, 1000000, 1000);
    const auto a = Admit(first); const auto b = Admit(second);
    ASSERT_TRUE(MempoolPersistence::save({a, b}, path.string()));
    const auto original = Read(path);
    pool->clear();
    auto removed = coins.SpendCoin(second_coin); ASSERT_TRUE(removed);
    ASSERT_FALSE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(pool->size(), 1U);
    EXPECT_TRUE(pool->hasTransaction(first.GetTxid().AsUint256()));
    EXPECT_FALSE(pool->saveToDisk(path.string()));
    EXPECT_EQ(Read(path), original);
    EXPECT_FALSE(pool->loadFromDisk((root / "other.dat").string()));
    EXPECT_FALSE(pool->saveToDisk(path.string()));
    EXPECT_EQ(Read(path), original);
    ASSERT_TRUE(std::filesystem::remove(path));
    EXPECT_FALSE(pool->loadFromDisk(path.string()));
    EXPECT_FALSE(pool->saveToDisk(path.string()));
    EXPECT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(MempoolPersistence::save({}, path.string()));
    const auto empty_replacement = Read(path);
    EXPECT_FALSE(pool->loadFromDisk(path.string()));
    EXPECT_FALSE(pool->saveToDisk(path.string()));
    EXPECT_EQ(Read(path), empty_replacement);
    Write(path, original);
    ASSERT_TRUE(coins.AddCoin(second_coin, *removed));
    ASSERT_TRUE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(pool->size(), 2U);
    EXPECT_TRUE(pool->hasTransaction(second.GetTxid().AsUint256()));
    EXPECT_TRUE(pool->saveToDisk(path.string()));
}
TEST_F(MempoolRetainedLoad, WholeFileFailureHasNoAdmission) {
    const auto path = root / "mempool.dat";
    const auto tx = spend(fund(4), 1000000, 1000);
    const auto entry = Admit(tx);
    ASSERT_TRUE(MempoolPersistence::save({entry}, path.string()));
    const auto original = Read(path); ASSERT_GT(original.size(), 1U);
    auto trailing = original; trailing.push_back(0);
    auto truncated = original; truncated.pop_back();
    for (const auto& malformed : {trailing, truncated}) {
        pool->clear(); Write(path, malformed);
        EXPECT_THROW((void)MempoolPersistence::load(path.string()), std::runtime_error);
        EXPECT_FALSE(pool->loadFromDisk(path.string()));
        EXPECT_EQ(pool->size(), 0U);
        EXPECT_FALSE(pool->saveToDisk(path.string()));
        EXPECT_EQ(Read(path), malformed);
        Write(path, original);
        ASSERT_TRUE(pool->loadFromDisk(path.string()));
        EXPECT_EQ(pool->size(), 1U);
        EXPECT_TRUE(pool->saveToDisk(path.string()));
    }
    pool->clear();
    ASSERT_TRUE(MempoolPersistence::save({}, path.string()));
    ASSERT_TRUE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(pool->size(), 0U);
    ASSERT_TRUE(pool->loadFromDisk((root / "missing.dat").string()));
    EXPECT_TRUE(pool->saveToDisk((root / "missing.dat").string()));
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolRetainedLoad, UnavailableOrchardFileRetainsCanonicalBody) {
    const auto path = root / "mempool.dat";
    std::ifstream f(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(f.good());
    const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(f), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    const auto body = MempoolTransaction::FromOrchard(envelope);
    const auto historical = spend(fund(5), 1000000, 1000);
    const auto historical_entry = Admit(historical);
    // A structural persistence fixture, never unchecked mempool insertion.
    ASSERT_TRUE(MempoolPersistence::save({historical_entry, MempoolEntry(body, envelope.ExplicitFee(), 110)}, path.string()));
    const auto original = Read(path);
    const auto entries = MempoolPersistence::load(path.string());
    ASSERT_EQ(entries.size(), 2U); EXPECT_EQ(entries[1].tx_bytes, wire);
    pool->clear();
    EXPECT_FALSE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(pool->size(), 1U);
    EXPECT_TRUE(pool->hasTransaction(historical.GetTxid().AsUint256()));
    EXPECT_FALSE(pool->hasTransaction(body.GetTxid().AsUint256()));
    EXPECT_FALSE(pool->saveToDisk(path.string()));
    EXPECT_EQ(Read(path), original);
    EXPECT_FALSE(pool->loadFromDisk(path.string()));
    EXPECT_EQ(pool->size(), 1U);
    EXPECT_FALSE(pool->saveToDisk(path.string()));
    EXPECT_EQ(Read(path), original);
}
#endif
}
