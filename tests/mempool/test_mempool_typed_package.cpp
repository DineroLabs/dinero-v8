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
#include <filesystem>
#ifdef DINERO_TEST_ORCHARD_BODY
#include "orchard_transaction.h"
#include <fstream>
#include <iterator>
#endif
#include "orchard_pool_conflict_checks.h"
namespace dinero {
class MempoolTypedPackageTestPeer {
public:
    static void InstallGraph(Mempool& pool, const std::vector<MempoolTransaction>& bodies) {
        std::unique_lock<std::shared_mutex> lock(pool.m_mutex);
        if (!pool.m_transactions.empty()) throw std::logic_error("policy fixture requires empty pool");
        for (const auto& body : bodies) {
            const auto fee = body.ExplicitFee().value_or(1000);
            if (!pool.m_transactions.emplace(body.GetTxid().AsUint256(), MempoolEntry(body, fee, 110)).second)
                throw std::logic_error("duplicate policy fixture identity");
        }
    }
    static TxAcceptResult Check(Mempool& pool, const MempoolTransaction& body,
        const std::unordered_set<uint256>& replaced, bool auth,
        std::unordered_set<uint256>& ancestors) {
        std::shared_lock<std::shared_mutex> lock(pool.m_mutex);
        return pool.checkAdmissionPackageLocked(body, replaced, auth, ancestors);
    }
};
}
namespace {
using namespace dinero;
class MempoolTypedPackage : public ::testing::Test {
protected:
    ChainDB db;consensus::ConsensusUTXOSet coins;std::filesystem::path root;
    std::array<uint8_t,32> secret{},internal{};std::vector<uint8_t> script;
    void SetUp() override {
        SelectParams(Chain::MAINNET);root=std::filesystem::temp_directory_path()/("mempool_typed_package_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    void sign(Transaction& tx,const OutPoint& out,uint64_t value) {
        CanonicalWalletUTXO coin;coin.txid=out.txid.AsUint256();coin.vout=out.vout;coin.value=AmountUna::Una(value);coin.spk=script;
        const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,0,{coin});
        if(bytes.size()!=32)throw std::runtime_error("fixture sighash refused");
        std::array<uint8_t,32> hash{};std::copy(bytes.begin(),bytes.end(),hash.begin());std::array<uint8_t,64> signature{};
        if(!TaprootKeys::SignSchnorrWithInternalKey(signature,hash,secret,internal))throw std::runtime_error("fixture signature refused");
        tx.vin[0].witness={std::vector<uint8_t>(signature.begin(),signature.end())};
    }
    Transaction split(const OutPoint& out) {
        auto tx=spend(out,1000000,1000);tx.vout[0].value=AmountUna::Una(499500);
        tx.vout.emplace_back(AmountUna::Una(499500),script);sign(tx,out,1000000);return tx;
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

// These graph constructors do not submit transactions or claim authorization.
Transaction structural(const std::vector<OutPoint>& inputs, uint32_t marker, size_t padding = 0) {
    Transaction tx; tx.version = 2; tx.lockTime = marker;
    for (const auto& out : inputs) {
        tx.vin.emplace_back(); tx.vin.back().prevout.txid = out.txid;
        tx.vin.back().prevout.vout = out.vout; tx.vin.back().sequence = 0xfffffffd;
    }
    tx.vout.emplace_back(AmountUna::Una(1), std::vector<uint8_t>(padding + 1, 0x51));
    tx.vout.emplace_back(AmountUna::Una(1), std::vector<uint8_t>{0x51});
    return tx;
}
TEST_F(MempoolTypedPackage, SignedAdmissionUsesCapturedPolicyAncestors) {
    Mempool pool(&db, &coins); pool.setRBFEnabled(true);
    const auto parent = split(fund(7));
    ASSERT_TRUE(pool.submitTransaction(parent, "fixture", false).accepted());
    const auto child = spend({parent.GetTxid(), 0}, 499500, 1000);
    const auto before = pool.getTransactionIds();
    std::unordered_set<uint256> ancestors;
    ASSERT_TRUE(MempoolTypedPackageTestPeer::Check(pool, MempoolTransaction(child), {}, false, ancestors).accepted());
    EXPECT_EQ(ancestors, std::unordered_set<uint256>{parent.GetTxid().AsUint256()});
    EXPECT_EQ(pool.getTransactionIds(), before);
    ASSERT_TRUE(pool.submitTransactionTestOnly(child, "fixture").accepted());
    EXPECT_EQ(pool.getTransactionIds(), before);
    ASSERT_TRUE(pool.submitTransaction(child, "fixture", false).accepted());
    const auto entry = pool.getMempoolEntry(child.GetTxid().AsUint256()); ASSERT_TRUE(entry);
    EXPECT_EQ(entry->ancestor_fee, 2000U);
    EXPECT_EQ(entry->ancestor_size, parent.GetSize() + child.GetSize());
    const auto replacement = spend({parent.GetTxid(), 0}, 499500, 10000);
    ASSERT_TRUE(pool.submitTransaction(replacement, "fixture", false).accepted());
    EXPECT_FALSE(pool.hasTransaction(child.GetTxid().AsUint256()));
    EXPECT_EQ(pool.size(), 2U);
}
TEST_F(MempoolTypedPackage, SharedAncestorsAndReplacementExclusion) {
    Mempool pool(&db, &coins);
    const auto root_tx = structural({}, 1);
    const auto left = structural({{root_tx.GetTxid(), 0}}, 2);
    const auto right = structural({{root_tx.GetTxid(), 1}}, 3);
    const auto leaf = structural({{left.GetTxid(), 0}, {right.GetTxid(), 0}}, 4);
    MempoolTypedPackageTestPeer::InstallGraph(pool, {MempoolTransaction(root_tx), MempoolTransaction(left), MempoolTransaction(right)});
    const auto before = pool.getTransactionIds(); std::unordered_set<uint256> ancestors;
    ASSERT_TRUE(MempoolTypedPackageTestPeer::Check(pool, MempoolTransaction(leaf), {}, false, ancestors).accepted());
    EXPECT_EQ(ancestors.size(), 3U); EXPECT_EQ(ancestors.count(root_tx.GetTxid().AsUint256()), 1U);
    ASSERT_TRUE(MempoolTypedPackageTestPeer::Check(pool, MempoolTransaction(leaf), {left.GetTxid().AsUint256()}, false, ancestors).accepted());
    EXPECT_EQ(ancestors.size(), 2U); EXPECT_EQ(ancestors.count(left.GetTxid().AsUint256()), 0U);
    EXPECT_EQ(ancestors.count(root_tx.GetTxid().AsUint256()), 1U);
    EXPECT_EQ(pool.getTransactionIds(), before);
}
TEST_F(MempoolTypedPackage, RefusalPreservesPriorAncestorResult) {
    Mempool pool(&db, &coins);
    const auto root_tx = structural({}, 5);
    MempoolTypedPackageTestPeer::InstallGraph(pool, {MempoolTransaction(root_tx)});
    const auto leaf = structural({{root_tx.GetTxid(), 0}}, 6, 101 * 1024);
    const auto before = pool.getTransactionIds(); uint256 sentinel; sentinel.data[0] = 229;
    std::unordered_set<uint256> ancestors{sentinel};
    EXPECT_EQ(MempoolTypedPackageTestPeer::Check(pool, MempoolTransaction(leaf), {}, false, ancestors).code,
              TxRejectCode::ANCESTOR_SIZE_EXCEEDED);
    EXPECT_EQ(ancestors, std::unordered_set<uint256>{sentinel});
    EXPECT_EQ(pool.getTransactionIds(), before);
    // Structural policy boundary only; no signing, admission or wire input.
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(MempoolTypedPackage, MixedFamilyGraphKeepsOrdinaryByteProfile) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES) / "candidate-envelope.bin", std::ios::binary);
    ASSERT_TRUE(file.good()); const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), {}};
    const auto envelope = orchard::TransactionEnvelope::DecodeExact(wire);
    const auto orchard_body = MempoolTransaction::FromOrchard(envelope);
    ASSERT_GT(orchard_body.OutputCount(), 0U);
    const auto child = structural({{orchard_body.GetTxid(), 0}}, 7);
    const auto leaf = structural({{child.GetTxid(), 0}}, 8);
    Mempool pool(&db, &coins);
    MempoolTypedPackageTestPeer::InstallGraph(pool, {orchard_body, MempoolTransaction(child)});
    const auto before = pool.getTransactionIds(); std::unordered_set<uint256> ancestors;
    ASSERT_TRUE(MempoolTypedPackageTestPeer::Check(pool, MempoolTransaction(leaf), {}, true, ancestors).accepted());
    EXPECT_EQ(ancestors.size(), 2U); EXPECT_EQ(ancestors.count(orchard_body.GetTxid().AsUint256()), 1U);
    const auto retained = ancestors;
    const auto large_leaf = structural({{child.GetTxid(), 0}}, 9, 101 * 1024);
    EXPECT_EQ(MempoolTypedPackageTestPeer::Check(pool, MempoolTransaction(large_leaf), {}, true, ancestors).code,
              TxRejectCode::ANCESTOR_SIZE_EXCEEDED);
    EXPECT_EQ(ancestors, retained);
    // An Orchard candidate can be inspected by policy without Historical access.
    ASSERT_TRUE(MempoolTypedPackageTestPeer::Check(pool, orchard_body, {}, true, ancestors).accepted());
    EXPECT_TRUE(ancestors.empty()); EXPECT_EQ(pool.getTransactionIds(), before);
    EXPECT_EQ(orchard_body.Serialize(), wire);
    // Isolated structural graph only: no Orchard proof, admission or selection.
}
#endif
}

#include "typed_acceptance_owner_checks.h"

#include "typed_pool_publication_checks.h"
